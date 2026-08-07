/* Native cover-art carousel launcher — see carousel_launcher.h.
 *
 * A .p8.png is a 160x205 PNG whose visible pixels ARE the cart's cover art (the cart data is steganographic,
 * in the low bits, and irrelevant here). We decode that art with lodepng (the same decoder fake-08's Cart
 * loader uses), nearest-neighbour-scale it to RGB565, and blit a 3-up carousel to the panel. Folders become
 * labelled tiles; the touch deck drives it (left/right = scroll, O = enter/launch, X = up a level). The whole
 * thing is a pre-game modal: it returns the chosen cart's path and app_main loads it into the VM as usual. */

#include "carousel_launcher.h"
#include "board.h"
#include "host.h"
#include "input.h"
#include "lodepng.h"
#include "pico8_font.h"

#include <vector>
#include <string>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cctype>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_app_desc.h"   /* main-menu About screen: version / IDF / build date */
#include "wifi_manager.h"
#include "ota_manager.h"   /* Settings -> SYSTEM UPDATE (WC-4a) + confirming a freshly-installed image */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "carousel";

/* ---- layout (portrait 480x800; content sits ABOVE the touch deck, which occupies the lower panel) ---- */
#define COVER_W_SRC 160
#define COVER_H_SRC 205
/* Layout is per-board (board_carousel_layout) so the launcher lays out correctly on any panel — the game
 * column, centre thumbnail size/position, side-peek width and breadcrumb position all come from the board.
 * Set once in carousel_launcher_run. See boards/<board>/board.{h,cpp}. */
static int s_gx, s_gw;          /* game column (x, width) — content confined here to match the game footprint */
static int s_tw, s_th, s_ty;    /* centre cover thumbnail size + top */
static std::string s_sd_mount;  /* where the SD is mounted (from main) — see run_wifi / WC-6 */
static int s_sidew, s_crumb_y;  /* side peek width, breadcrumb top */
static int s_title_y, s_title_scale, s_body_y, s_body_dy, s_body_scale, s_info_scale;   /* main-menu / settings layout (per board) */
#define SIDE_DIM 120           /* /256 brightness for the two side peeks */

static int      s_W = 480, s_H = 800;
static uint16_t s_bg, s_fg, s_dim, s_missing, s_accent, s_platform, s_titlebar, s_track;
static uint16_t s_case;   /* capitals (WC-2) — fixed, NOT the accent: see case_col */

/* one reusable PSRAM scratch buffer, sized to the largest thing blitted (the 384x384 centre window) */
static uint16_t *s_scratch = nullptr;

/* ---------- tiny PICO-8 font text (3x5 glyphs, '#'=on) ---------- */
/* The font has ONE set of letterforms (cap-shaped), so case cannot be carried by shape — a 3x5 cell has no
 * descender row, and lowercase drawn without one makes a/g/q identical, which is useless in the password field
 * this exists for. Case is carried by COLOUR instead (WC-2): the shapes are the default, and a capital is drawn
 * in the accent. No cell-height change, so no re-layout.
 *
 * Deliberately NOT the accent. The accent means "selected row" and is user-configurable, so reusing it both
 * collided with selection and made capitals change colour with the theme — case is a property of the text, not
 * a theme choice. s_case is fixed (see the definition), NOT the accent.
 *
 * It only applies on the normal background. Anywhere the caller has inverted the row — a selected pill, a
 * highlighted key — fg/bg are chosen as a contrasting PAIR, and dropping a third fixed colour into that pair
 * lands cyan-on-accent, the worst contrast on screen. When s_case *was* the accent an equality test caught
 * that by luck; a fixed colour can never equal the accent, so test the background explicitly. */
static inline uint16_t case_col(char c, uint16_t fg, uint16_t bg) {
    if (c < 'A' || c > 'Z') return fg;
    if (bg != s_bg) return fg;                        /* inverted/highlighted row — keep the caller's pair */
    return (s_case == bg || s_case == fg) ? fg : s_case;
}

static const char *const *glyph_rows(char c) {
    if (c >= 'a' && c <= 'z') c -= 32;   /* one glyph per letter; case is colour, not shape */
    const size_t n = sizeof(PICO8_FONT_GLYPHS) / sizeof(PICO8_FONT_GLYPHS[0]);
    for (size_t i = 0; i < n; i++)
        if (PICO8_FONT_GLYPHS[i].c == c) return PICO8_FONT_GLYPHS[i].rows;
    return nullptr;   /* unknown glyph -> blank cell */
}

/* Render `str` into a scratch RGB565 buffer and blit once at (x,y). Cells are 4px wide (3 glyph + 1 gap),
 * 5 tall, times `scale`. Returns the pixel width drawn. */
static int draw_text(int x, int y, const char *str, int scale, uint16_t fg, uint16_t bg) {
    int len = (int)strlen(str);
    if (len <= 0) return 0;
    int tw = len * 4 * scale, th = 5 * scale;
    uint16_t *buf = (uint16_t *)heap_caps_malloc((size_t)tw * th * 2, MALLOC_CAP_SPIRAM);
    if (!buf) return 0;
    for (int i = 0; i < tw * th; i++) buf[i] = bg;
    for (int ci = 0; ci < len; ci++) {
        const char *const *g = glyph_rows(str[ci]);
        if (!g) continue;
        const uint16_t cc = case_col(str[ci], fg, bg);   /* capitals stand out; see case_col */
        int ox = ci * 4 * scale;
        for (int r = 0; r < 5; r++)
            for (int c = 0; c < 3; c++)
                if (g[r][c] == '#')
                    for (int sy = 0; sy < scale; sy++)
                        for (int sx = 0; sx < scale; sx++)
                            buf[(r * scale + sy) * tw + (ox + c * scale + sx)] = cc;
    }
    board_lcd_blit(x, y, tw, th, buf);
    heap_caps_free(buf);
    return tw;
}
static void draw_text_centered(int cx, int y, const char *str, int scale, uint16_t fg, uint16_t bg) {
    int len = (int)strlen(str);
    /* Centre on the visible ink, not the cell box: each cell carries a trailing 1px inter-glyph gap, and the
     * last one has no glyph after it — counting it (len*4) biases the whole string half a cell to the left. */
    int ink = len > 0 ? (len * 4 - 1) * scale : 0;
    draw_text(cx - ink / 2, y, str, scale, fg, bg);
}

/* ---------- cover cache (decoded RGBA in PSRAM, small LRU) ---------- */
struct Cover { std::string path; uint8_t *rgba; bool ok; };
static std::vector<Cover> s_cache;
static const size_t CACHE_MAX = 6;

static void cache_clear() {
    for (auto &c : s_cache) if (c.rgba) heap_caps_free(c.rgba);
    s_cache.clear();
}
/* Decode a cover to 160x205 RGBA (cached). Returns the RGBA pointer or nullptr (not a valid 160x205 PNG). */
static const uint8_t *cover_rgba(const std::string &path) {
    for (auto &c : s_cache) if (c.path == path) return c.ok ? c.rgba : nullptr;

    Cover cov{ path, nullptr, false };
    {
        std::vector<unsigned char> tmp;   /* transient (internal RAM); copied to PSRAM then freed */
        unsigned w = 0, h = 0;
        if (lodepng::decode(tmp, w, h, path) == 0 && w == COVER_W_SRC && h == COVER_H_SRC) {
            cov.rgba = (uint8_t *)heap_caps_malloc((size_t)w * h * 4, MALLOC_CAP_SPIRAM);
            if (cov.rgba) { memcpy(cov.rgba, tmp.data(), (size_t)w * h * 4); cov.ok = true; }
        }
    }
    if (s_cache.size() >= CACHE_MAX) {           /* evict oldest */
        if (s_cache.front().rgba) heap_caps_free(s_cache.front().rgba);
        s_cache.erase(s_cache.begin());
    }
    s_cache.push_back(cov);
    return cov.ok ? cov.rgba : nullptr;
}

/* nearest-neighbour scale RGBA(160x205) -> RGB565 dst (dw*dh) with a brightness scale (dim/256). */
/* Letterbox-fit the 160x205 cover into a bw*bh box, centred, keeping aspect (bg shows in the margins).
 * s_scratch must already be bg-filled. Used for the centre window so the whole cover is visible undistorted. */
static void cover_fit(int bw, int bh, const uint8_t *rgba, int dim) {
    int fw = bw, fh = bw * COVER_H_SRC / COVER_W_SRC;
    if (fh > bh) { fh = bh; fw = bh * COVER_W_SRC / COVER_H_SRC; }
    int ox = (bw - fw) / 2, oy = (bh - fh) / 2;
    for (int y = 0; y < fh; y++) {
        int sy = y * COVER_H_SRC / fh;
        const uint8_t *srow = rgba + (size_t)sy * COVER_W_SRC * 4;
        uint16_t *drow = s_scratch + (size_t)(oy + y) * bw + ox;
        for (int x = 0; x < fw; x++) {
            const uint8_t *p = srow + (size_t)(x * COVER_W_SRC / fw) * 4;
            int r = p[0] * dim >> 8, g = p[1] * dim >> 8, b = p[2] * dim >> 8;
            drow[x] = board_lcd_rgb565((uint8_t)r, (uint8_t)g, (uint8_t)b);
        }
    }
}

/* Crop-fill the cover into a bw*bh box (centre-crop, no letterbox) — for the thin side slivers, which should
 * show a slice of the neighbour's art rather than a tiny letterboxed thumbnail. */
static void cover_fill(int bw, int bh, const uint8_t *rgba, int dim) {
    int sw, sh;
    if (bw * COVER_H_SRC >= bh * COVER_W_SRC) { sw = COVER_W_SRC; sh = COVER_W_SRC * bh / bw; }
    else                                      { sh = COVER_H_SRC; sw = COVER_H_SRC * bw / bh; }
    int sx0 = (COVER_W_SRC - sw) / 2, sy0 = (COVER_H_SRC - sh) / 2;
    for (int y = 0; y < bh; y++) {
        int sy = sy0 + y * sh / bh;
        const uint8_t *srow = rgba + (size_t)sy * COVER_W_SRC * 4;
        uint16_t *drow = s_scratch + (size_t)y * bw;
        for (int x = 0; x < bw; x++) {
            const uint8_t *p = srow + (size_t)(sx0 + x * sw / bw) * 4;
            int r = p[0] * dim >> 8, g = p[1] * dim >> 8, b = p[2] * dim >> 8;
            drow[x] = board_lcd_rgb565((uint8_t)r, (uint8_t)g, (uint8_t)b);
        }
    }
}

static void fill_rect(uint16_t *dst, int w, int h, uint16_t col) {
    for (int i = 0; i < w * h; i++) dst[i] = col;
}

static inline uint16_t dimrgb(int r, int g, int b, int dim) {
    return board_lcd_rgb565((uint8_t)(r * dim >> 8), (uint8_t)(g * dim >> 8), (uint8_t)(b * dim >> 8));
}

/* Round the corners of the w*h s_scratch tile to `radius` (outside-corner pixels -> bg) and, if border>0,
 * stroke a border-px frame in `bordercol` that follows the rounded edge. */
static void finish_tile(int w, int h, int radius, int border, uint16_t bordercol) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int dx = 0, dy = 0;
            if (x < radius) dx = radius - x; else if (x >= w - radius) dx = x - (w - radius) + 1;
            if (y < radius) dy = radius - y; else if (y >= h - radius) dy = y - (h - radius) + 1;
            uint16_t *px = &s_scratch[y * w + x];
            if (dx && dy) {
                int d2 = dx * dx + dy * dy;
                if (d2 > radius * radius) { *px = s_bg; continue; }
                if (border && d2 > (radius - border) * (radius - border)) { *px = bordercol; continue; }
            } else if (border && (x < border || x >= w - border || y < border || y >= h - border)) {
                *px = bordercol;
            }
        }
    }
}

/* Blit a filled rounded rectangle (via s_scratch) — used for the platform behind the centre cover and the
 * position bar / breadcrumb rule. */
static void blit_round_rect(int x, int y, int w, int h, int radius, uint16_t col) {
    fill_rect(s_scratch, w, h, col);
    if (radius > 0) finish_tile(w, h, radius, 0, 0);
    board_lcd_blit(x, y, w, h, s_scratch);
}

/* Render `str` transparently INTO a buffer (only the glyph pixels are written; the tile art shows through the
 * gaps). Used to place a folder's name on its thumbnail. Capitals take `hi` (WC-2) — passed in already dimmed
 * by the caller, because the side peeks dim their text to say "not selected" and a full-brightness capital
 * would punch straight through that cue. Unknown glyphs skipped. */
static void glyphs_into(uint16_t *buf, int bufw, int bufh, int x0, int y0, const char *str, int scale,
                        uint16_t fg, uint16_t hi) {
    int len = (int)strlen(str);
    for (int ci = 0; ci < len; ci++) {
        const uint16_t cc = (str[ci] >= 'A' && str[ci] <= 'Z') ? hi : fg;
        const char *const *g = glyph_rows(str[ci]);
        if (!g) continue;
        int ox = x0 + ci * 4 * scale;
        for (int r = 0; r < 5; r++)
            for (int c = 0; c < 3; c++)
                if (g[r][c] == '#')
                    for (int sy = 0; sy < scale; sy++)
                        for (int sx = 0; sx < scale; sx++) {
                            int px = ox + c * scale + sx, py = y0 + r * scale + sy;
                            if (px >= 0 && px < bufw && py >= 0 && py < bufh) buf[py * bufw + px] = cc;
                        }
    }
}

/* Draw a folder glyph (body + tab) into s_scratch with the folder `name` beneath it; an up-triangle (no name)
 * for the ".." back entry. `dim` scales it for the side peeks. Tile bg = page bg so finish_tile rounds cleanly. */
static void draw_folder_tile(int w, int h, bool isUp, int dim, const char *name) {
    fill_rect(s_scratch, w, h, s_bg);
    int fw = w * 60 / 100, fh = h * 26 / 100;
    int fx = (w - fw) / 2, fy = h * 20 / 100;      /* icon in the upper portion, name below */
    int tabw = fw * 44 / 100, tabh = fh * 24 / 100;
    uint16_t body = isUp ? dimrgb(74, 82, 104, dim) : dimrgb(62, 120, 206, dim);
    uint16_t tab  = isUp ? dimrgb(102, 110, 134, dim) : dimrgb(96, 158, 236, dim);
    for (int y = fy - tabh; y < fy + 2; y++)
        for (int x = fx; x < fx + tabw; x++) if (y >= 0) s_scratch[y * w + x] = tab;
    for (int y = fy; y < fy + fh; y++)
        for (int x = fx; x < fx + fw; x++) s_scratch[y * w + x] = body;
    if (isUp) {                                    /* up-triangle => "back / up a level" */
        uint16_t ar = dimrgb(226, 230, 240, dim);
        int apex = fy + fh / 4, base = fy + fh * 3 / 4, cx = w / 2, half = fw / 4;
        for (int y = apex; y <= base; y++) {
            int hw = half * (y - apex) / (base - apex);
            for (int x = cx - hw; x <= cx + hw; x++) s_scratch[y * w + x] = ar;
        }
        return;
    }
    if (name && *name) {                           /* folder name, wrapped to <=2 lines, centred below the icon */
        int usable = w * 68 / 100;                 /* narrow text area (~16% padding each side); long names wrap
                                                    * to two shorter lines rather than running to the border */
        int scale = usable / (11 * 4);             /* keeps the larger ~scale-3 size on the centre tile */
        if (scale < 1) scale = 1;
        if (scale > 3) scale = 3;
        int per = usable / (4 * scale);            /* chars that fit on a line */
        if (per < 1) per = 1;
        std::string s(name), l1 = s, l2;
        if ((int)s.size() > per) {                 /* break on the last space or hyphen at/before the limit */
            int bp = -1, hi = (per < (int)s.size() ? per : (int)s.size() - 1);
            for (int i = hi; i > per / 3; i--) { if (s[i] == ' ' || s[i] == '-') { bp = i; break; } }
            if (bp >= 0) { bool hy = (s[bp] == '-'); l1 = s.substr(0, hy ? bp + 1 : bp); l2 = s.substr(bp + 1); }
            else         { l1 = s.substr(0, per); l2 = s.substr(per); }
            if ((int)l2.size() > per) l2 = l2.substr(0, per - 1) + ".";
        }
        uint16_t fg = dimrgb(232, 235, 245, dim);
        uint16_t hi = dimrgb(110, 220, 220, dim);   /* s_case, dimmed the same as the body text */
        int ny = fy + fh + h * 9 / 100;
        int tw1 = (int)l1.size() * 4 * scale;
        glyphs_into(s_scratch, w, h, (w - tw1) / 2, ny, l1.c_str(), scale, fg, hi);
        if (!l2.empty()) {
            int tw2 = (int)l2.size() * 4 * scale;
            glyphs_into(s_scratch, w, h, (w - tw2) / 2, ny + 6 * scale, l2.c_str(), scale, fg, hi);
        }
    }
}

/* ---------- display-name helpers ---------- */
static std::string strip_ext(const std::string &n) {
    auto ends = [&](const char *e){ size_t le = strlen(e); return n.size() >= le && n.compare(n.size()-le, le, e) == 0; };
    if (ends(".p8.png")) return n.substr(0, n.size() - 7);
    if (ends(".png"))    return n.substr(0, n.size() - 4);
    if (ends(".p8"))     return n.substr(0, n.size() - 3);
    return n;
}
static std::string display_name(std::string n) {           /* strip [Category] brackets + extension for the label */
    n = strip_ext(n);
    if (n.size() >= 2 && n.front() == '[' && n.back() == ']') n = n.substr(1, n.size() - 2);
    if (n.size() > 24) n = n.substr(0, 23) + ".";
    return n;
}
static std::string basename_of(const std::string &p) {
    size_t s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

/* ---------- entries for the current directory ---------- */
struct Entry { std::string name; std::string path; bool isDir; bool isUp; };

static std::vector<Entry> build_entries(Host *host, const std::string &dir, const std::string &floor) {
    host->setCartDirectory(dir);
    std::vector<Entry> out;
    if (dir != floor) {
        std::string parent = dir.substr(0, dir.find_last_of('/'));
        if (parent.size() < floor.size()) parent = floor;
        out.push_back({ "..", parent, true, true });
    }
    std::vector<std::string> dirs = host->listdirs();          /* names */
    std::sort(dirs.begin(), dirs.end());
    for (auto &d : dirs) out.push_back({ d, dir + "/" + d, true, false });

    std::vector<std::string> carts = host->listcarts();        /* full paths */
    std::sort(carts.begin(), carts.end(), [](const std::string &a, const std::string &b){
        return basename_of(a) < basename_of(b);
    });
    for (auto &c : carts) out.push_back({ basename_of(c), c, false, false });
    return out;
}

/* ---------- drawing one carousel slot ---------- */
/* `center` uses the full 384x384 game window with letterbox-fit covers + an accent frame; side slivers
 * crop-fill so a slice of the neighbour's art shows. */
static void draw_slot(const Entry &e, int x, int y, int w, int h, int dim, bool center) {
    fill_rect(s_scratch, w, h, s_bg);
    if (e.isDir) {
        draw_folder_tile(w, h, e.isUp, dim, display_name(e.name).c_str());
    } else {
        const uint8_t *rgba = cover_rgba(e.path);
        if (rgba) {
            if (center) cover_fit(w, h, rgba, dim);
            else        cover_fill(w, h, rgba, dim);
        } else {
            int mw = w * 76 / 100, mh = mw * COVER_H_SRC / COVER_W_SRC;   /* grey placeholder box */
            if (mh > h * 88 / 100) { mh = h * 88 / 100; mw = mh * COVER_W_SRC / COVER_H_SRC; }
            for (int yy = (h - mh) / 2; yy < (h + mh) / 2; yy++)
                for (int xx = (w - mw) / 2; xx < (w + mw) / 2; xx++) s_scratch[yy * w + xx] = s_missing;
        }
    }
    finish_tile(w, h, center ? 14 : 8, center ? 3 : 0, s_accent);
    board_lcd_blit(x, y, w, h, s_scratch);
}

/* Clear a rect to the page background (used to erase a peek slot that has no neighbour). */
static void clear_rect(int x, int y, int w, int h) {
    fill_rect(s_scratch, w, h, s_bg);
    board_lcd_blit(x, y, w, h, s_scratch);
}

/* `full` = repaint the whole screen (background + breadcrumb + deck) — only on a directory change. On a plain
 * item move it's false, so only the thumbnail / peeks / position bar are repainted in place and the rest of
 * the panel (deck, background, breadcrumb) is left untouched — no full-screen clear, hence no flicker. */
static void render(const std::vector<Entry> &entries, int sel, const std::string &curdir,
                   const std::string &floor, bool full) {
    int N = (int)entries.size();
    int cx = (s_W - s_tw) / 2;
    /* The game renders in a game_w-wide column centred on the panel with black borders either side (board-
     * provided: 384 at x48 on the P4, 256 at x32 on the S3). Keep ALL carousel content inside that column. */
    int glx = s_gx;                             /* left edge of the game column */
    int grx = s_gx + s_gw;                      /* right edge */

    if (full) {
        board_lcd_fill(s_bg);
        std::string crumb = (curdir == floor) ? std::string("sd card") : display_name(basename_of(curdir));
        draw_text_centered(s_W / 2, s_crumb_y, crumb.c_str(), 3, s_fg, s_bg);
        blit_round_rect(s_W / 2 - 26, s_crumb_y + 24, 52, 3, 1, s_accent);
        board_draw_touch_deck();
    }

    if (N == 0) {
        if (full) draw_text_centered(s_W / 2, s_ty + s_th / 2, "empty", 4, s_fg, s_bg);
        return;
    }

    /* side peeks (crop-filled slices of the neighbours, dimmed) — kept INSIDE the game column, flanking the
     * thumbnail. Fixed slots: draw the neighbour, or clear the slot if there isn't one. */
    int sh = s_th * 84 / 100, sy = s_ty + (s_th - sh) / 2;
    if (sel - 1 >= 0)  draw_slot(entries[sel - 1], glx + 4, sy, s_sidew, sh, SIDE_DIM, false);
    else               clear_rect(glx + 4, sy, s_sidew, sh);
    if (sel + 1 <  N)  draw_slot(entries[sel + 1], grx - 4 - s_sidew, sy, s_sidew, sh, SIDE_DIM, false);
    else               clear_rect(grx - 4 - s_sidew, sy, s_sidew, sh);

    /* centre thumbnail (fully overwrites its previous contents — no clear needed) */
    draw_slot(entries[sel], cx, s_ty, s_tw, s_th, 256, true);

    /* position bar below the thumbnail — a visible track with an accent knob showing where `sel` sits */
    {
        int tw = s_tw - 46; if (tw < 40) tw = 40;
        int th = 6, tx = (s_W - tw) / 2, ty = s_ty + s_th + 12;
        if (N > 1) {
            fill_rect(s_scratch, tw, th, s_track);
            int kw = tw / N; if (kw < 16) kw = 16; if (kw > tw) kw = tw;
            int kx = sel * (tw - kw) / (N - 1);
            for (int yy = 0; yy < th; yy++)
                for (int xx = kx; xx < kx + kw && xx < tw; xx++) s_scratch[yy * tw + xx] = s_accent;
            finish_tile(tw, th, 3, 0, 0);          /* rounded ends */
            board_lcd_blit(tx, ty, tw, th, s_scratch);
        } else {
            clear_rect(tx, ty, tw, th);            /* single item: nothing to indicate */
        }
    }
}

/* ---------- dev screenshot: dump the live framebuffer over the console (same 0xFB SHOT framing as main.cpp's
 * FB_DUMP), triggered by the PAUSE button, so the carousel can be captured crisply at any state. ---------- */
/* FB_DUMP needs board_lcd_framebuffer: the P4 has a persistent DPI framebuffer; the S3 mirrors every blit into
 * a host-side shadow (its GRAM panel can't be read back). The two console types differ, so the dump splits:
 * the P4 (USB-JTAG) deflates first (the bulk endpoint stalls on big raw writes); the S3 (UART) streams raw
 * (no miniz — LovyanGFX bundles a colliding copy — and UART drains a big transfer fine, just slower). */
#ifdef FB_DUMP
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag_vfs.h"   /* set_tx_line_endings: disable the LF->CRLF mangling that corrupts binary */
#include "driver/usb_serial_jtag.h"       /* usb_serial_jtag_wait_tx_done: force the final TX packet out */
#elif CONFIG_ESP_CONSOLE_UART
#include "esp_rom_serial_output.h"        /* UART console (S3): push bytes straight to the FIFO, bypassing the VFS */
#endif

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
/* ---- P4 (USB-JTAG): compressed SHTZ dump (miniz; only linked here — the S3's LovyanGFX has a colliding copy) ---- */
static void fb_write_raw(const void *p, size_t n) { fwrite(p, 1, n, stdout); fflush(stdout); }
#include "miniz.h"
/* put-buffer sink for tdefl's callback mode: append each compressed chunk into a caller buffer. */
struct FbOutState { uint8_t *buf; size_t len, cap; };
static mz_bool fb_put_cb(const void *p, int len, void *user) {
    FbOutState *o = (FbOutState *)user;
    if (o->len + (size_t)len > o->cap) return MZ_FALSE;
    memcpy(o->buf + o->len, p, (size_t)len);
    o->len += (size_t)len;
    return MZ_TRUE;
}
/* Deflate the full-res framebuffer and stream it framed as `FB FB FB FB 'S' 'H' 'T' 'Z' w(2) h(2) clen(4)` +
 * clen zlib bytes. Compression keeps the blob well under the P4 USB-JTAG bulk stall floor. Host: fb_menu_shot.py.
 * The two real gotchas (which made FB_DUMP look like a dead end): the console mapped \n->\r\n and corrupted the
 * binary (fixed by ESP_LINE_ENDINGS_LF around the write), and the driver held the final TX packet (fixed by a
 * trailing pad + usb_serial_jtag_wait_tx_done). */
static void fb_dump_compressed(const unsigned short *fb, int w, int h) {
    if (!fb || w <= 0 || h <= 0) return;
    size_t raw = (size_t)w * h * 2;
    size_t bound = raw + raw / 2 + 256;
    static uint8_t *cbuf = nullptr; static size_t cap = 0;
    if (cap < bound) { if (cbuf) heap_caps_free(cbuf); cbuf = (uint8_t *)heap_caps_malloc(bound, MALLOC_CAP_SPIRAM); cap = bound; }
    if (!cbuf) return;
    /* caller-owned tdefl_compressor in PSRAM: the one-call helpers MZ_MALLOC ~300 KB in exhausted internal RAM. */
    static tdefl_compressor *comp = nullptr;
    if (!comp) comp = (tdefl_compressor *)heap_caps_malloc(sizeof(tdefl_compressor), MALLOC_CAP_SPIRAM);
    if (!comp) return;
    int flags = TDEFL_WRITE_ZLIB_HEADER | TDEFL_COMPUTE_ADLER32 | 128;   /* valid zlib stream + level-6 probes */
    FbOutState os = { cbuf, 0, cap };
    if (tdefl_init(comp, fb_put_cb, &os, flags) != TDEFL_STATUS_OKAY) return;
    if (tdefl_compress_buffer(comp, (const void *)fb, raw, TDEFL_FINISH) != TDEFL_STATUS_DONE) return;
    size_t clen = os.len;
    esp_log_level_set("*", ESP_LOG_NONE);
    vTaskDelay(pdMS_TO_TICKS(60));
    uint8_t hdr[16] = { 0xFB, 0xFB, 0xFB, 0xFB, 'S', 'H', 'T', 'Z',
                        (uint8_t)(w & 0xff), (uint8_t)(w >> 8), (uint8_t)(h & 0xff), (uint8_t)(h >> 8),
                        (uint8_t)(clen & 0xff), (uint8_t)((clen >> 8) & 0xff),
                        (uint8_t)((clen >> 16) & 0xff), (uint8_t)((clen >> 24) & 0xff) };
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);   /* binary: no \n -> \r\n */
    fb_write_raw(hdr, sizeof hdr);
    fb_write_raw(cbuf, clen);
    static const uint8_t pad[256] = { 0 };
    fb_write_raw(pad, sizeof pad);                                  /* flush the held final packet */
    usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(2000));
    vTaskDelay(pdMS_TO_TICKS(60));
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF); /* restore for readable logs */
    esp_log_level_set("*", ESP_LOG_INFO);
}
void carousel_fb_dump(void) {
    int w = 0, h = 0;
    const uint16_t *fb = board_lcd_framebuffer(&w, &h);
    fb_dump_compressed(fb, w, h);
}
#else   /* non-USB-JTAG console (S3 UART): raw SHOT dump, no miniz */
/* Stream the shadow framebuffer raw, framed as `FB FB FB FB 'S' 'H' 'O' 'T' w(2) h(2)` + w*h*2 RGB565 bytes.
 * No compression (avoids the LovyanGFX miniz collision); a UART console drains the whole ~300 KB fine (just
 * slower, ~27 s at 115200). Host: tools/fb_menu_shot_raw.py.
 *
 * Two hazards corrupt a naive binary dump here, both by injecting bytes MID-STREAM (which shears every row
 * after the injection point, so the image looks progressively rolled):
 *   1. The console VFS maps \n -> \r\n. Bytes go straight to the ROM console FIFO (esp_rom_output_tx_one_char),
 *      NOT through stdout, so no translation touches the data.
 *   2. The ~27 s stream is long enough to starve the idle task and trip the Task Watchdog, whose backtrace
 *      print lands in the middle of the pixel data. So yield briefly every few KB: the delay lets the FIFO
 *      drain and, crucially, lets the idle task run and feed the WDT. */
static void fb_rom_write(const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) {
        esp_rom_output_tx_one_char(b[i]);
        if ((i & 0xFFF) == 0xFFF) vTaskDelay(1);   /* every 4 KB: yield so the WDT is fed mid-dump */
    }
}
static void fb_dump_raw(const unsigned short *fb, int w, int h) {
    if (!fb || w <= 0 || h <= 0) return;
    uint8_t hdr[12] = { 0xFB, 0xFB, 0xFB, 0xFB, 'S', 'H', 'O', 'T',
                        (uint8_t)(w & 0xff), (uint8_t)(w >> 8), (uint8_t)(h & 0xff), (uint8_t)(h >> 8) };
    esp_log_level_set("*", ESP_LOG_NONE);
    fflush(stdout);                                  /* drain any pending log text before the binary frame */
    vTaskDelay(pdMS_TO_TICKS(60));
    fb_rom_write(hdr, sizeof hdr);
    fb_rom_write(fb, (size_t)w * h * 2);
    esp_rom_output_tx_wait_idle(CONFIG_ESP_CONSOLE_UART_NUM);   /* let the last bytes clock out */
    vTaskDelay(pdMS_TO_TICKS(60));
    esp_log_level_set("*", ESP_LOG_INFO);
}
void carousel_fb_dump(void) {
    int w = 0, h = 0;
    const uint16_t *fb = board_lcd_framebuffer(&w, &h);
    fb_dump_raw(fb, w, h);
}
#endif  /* CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG */
#endif  /* FB_DUMP */

/* ================= main menu (Games / Settings / About) ================= */

static const char *const MENU_ITEMS[] = { "games", "settings", "about" };
#define MENU_N 3

/* Launcher accent theme — the one live Settings knob (self-contained, no board API). */
static const struct { uint8_t r, g, b; const char *name; } ACCENTS[] = {
    {  90, 162, 255, "blue"  },
    {  80, 200, 140, "green" },
    { 240, 180,  70, "amber" },
    { 235, 120, 180, "pink"  },
};
#define ACCENT_N (int)(sizeof(ACCENTS) / sizeof(ACCENTS[0]))
static int s_accent_idx = 0;
static void apply_accent(void) {
    s_accent = board_lcd_rgb565(ACCENTS[s_accent_idx].r, ACCENTS[s_accent_idx].g, ACCENTS[s_accent_idx].b);
}

/* The SD games carousel — the former whole launcher. Returns the chosen cart's path, or "" if the user
 * pressed X at the root (back to the main menu). Uses the file-scope layout/scratch set up in the run fn. */
static std::string run_carousel(Host *host, const std::string &start_dir) {
    std::string curdir = start_dir;
    std::vector<Entry> entries = build_entries(host, curdir, start_dir);
    int sel = 0;
    bool dirty = true, full = true;
    uint8_t prev = input_poll();   /* seed with held buttons so a press carried in from the previous screen doesn't re-fire */
    ESP_LOGI(TAG, "games: %d entries in %s", (int)entries.size(), curdir.c_str());
    for (;;) {
        if (dirty) { render(entries, sel, curdir, start_dir, full); dirty = false; full = false; }
        uint8_t m = input_poll();
        uint8_t pressed = (uint8_t)(m & ~prev);
        prev = m;
        int N = (int)entries.size();
#ifdef FB_DUMP
        if (pressed & INPUT_PAUSE) carousel_fb_dump();
#endif
        if (pressed & (INPUT_RIGHT | INPUT_DOWN)) {
            if (N) { sel = (sel + 1) % N; dirty = true; }
        } else if (pressed & (INPUT_LEFT | INPUT_UP)) {
            if (N) { sel = (sel - 1 + N) % N; dirty = true; }
        } else if (pressed & INPUT_O) {
            if (N) {
                Entry &e = entries[sel];
                if (e.isDir) {
                    curdir = e.path;
                    cache_clear();
                    entries = build_entries(host, curdir, start_dir);
                    sel = 0; dirty = true; full = true;
                    ESP_LOGI(TAG, "cd %s (%d entries)", curdir.c_str(), (int)entries.size());
                } else {
                    ESP_LOGI(TAG, "launch %s", e.path.c_str());
                    return e.path;                      /* -> app_main loads this cart */
                }
            }
        } else if (pressed & INPUT_X) {
            if (curdir != start_dir) {                  /* up a level */
                curdir = curdir.substr(0, curdir.find_last_of('/'));
                if (curdir.size() < start_dir.size()) curdir = start_dir;
                cache_clear();
                entries = build_entries(host, curdir, start_dir);
                sel = 0; dirty = true; full = true;
            } else {
                cache_clear();
                return "";                              /* at root -> back to main menu */
            }
        }
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

/* All menu/settings/about content is laid out from the per-board layout (title_y/body_y/body_dy + the
 * [s_ty, s_ty+s_th] band), so it stays above the board's touch deck on any panel — no hardcoded positions. */
#define CB_BOT (s_ty + s_th)   /* band bottom = the "above the deck" content floor (board-provided) */

/* Redraw one menu row in place (no full-screen clear -> no flicker on move). */
static void draw_menu_row(int i, int sel) {
    int y = s_body_y + i * s_body_dy;
    int py = y - 6;                                    /* pill / clear-band top */
    int bh = s_body_dy * 80 / 100;                     /* pill height */
    int gh = 5 * s_body_scale;                         /* label glyph height at the board's menu scale */
    int ty = py + (bh - gh) / 2;                        /* vertically centre the label inside the pill/band */
    /* Clear the full column width so moving the selection erases the previous (possibly wider) pill cleanly. */
    fill_rect(s_scratch, s_gw - 40, bh + 12, s_bg);
    board_lcd_blit(s_gx + 20, py, s_gw - 40, bh + 12, s_scratch);
    if (i == sel) {
        int ink = ((int)strlen(MENU_ITEMS[i]) * 4 - 1) * s_body_scale;
        int pw = ink + 2 * gh;                          /* snug pill: text + ~one glyph-height of padding each side */
        if (pw > s_gw - 40) pw = s_gw - 40;
        blit_round_rect(s_W / 2 - pw / 2, py, pw, bh, bh / 2, s_accent);   /* centred full-radius pill */
        draw_text_centered(s_W / 2, ty, MENU_ITEMS[i], s_body_scale, s_bg, s_accent);
    } else {
        draw_text_centered(s_W / 2, ty, MENU_ITEMS[i], s_body_scale, s_fg, s_bg);
    }
}

static void render_menu(int sel, bool full) {
    if (full) {
        board_lcd_fill(s_bg);
        draw_text_centered(s_W / 2, s_title_y, "pico-e32", s_title_scale, s_accent, s_bg);
        draw_text_centered(s_W / 2, s_title_y + s_title_scale * 8, "pico-8 handheld", 2, s_dim, s_bg);
        board_draw_touch_deck();
    }
    for (int i = 0; i < MENU_N; i++) draw_menu_row(i, sel);
}

/* Small helper: a labelled info/setting row (label left, value right) inside the game column. Label and value
 * carry independent scales (the value is usually a touch smaller); the value is vertically centred on the label. */
static void draw_kv(int y, const char *label, const char *value, int lscale, int vscale, uint16_t lcol, uint16_t vcol) {
    int bandh = 8 * (lscale > vscale ? lscale : vscale);
    fill_rect(s_scratch, s_gw - 40, bandh, s_bg);              /* clear band */
    board_lcd_blit(s_gx + 20, y - 4, s_gw - 40, bandh, s_scratch);
    draw_text(s_gx + 24, y, label, lscale, lcol, s_bg);
    if (value && *value) {
        int vy = y + (lscale - vscale) * 5 / 2;               /* centre the (shorter) value on the label */
        draw_text(s_gx + s_gw - 24 - (int)strlen(value) * 4 * vscale, vy, value, vscale, vcol, s_bg);
    }
}

#ifdef BOARD_HAS_WIFI
/* ===================== Settings -> WiFi (STA: scan / on-screen keyboard / connect / persist) ===================== */

/* Deck-driven on-screen keyboard (the deck has no letters): d-pad moves, O presses the highlighted key, X is a
 * backspace shortcut. Two case layers; the bottom control row is [aA] [SPACE] [DEL] [BACK] [OK]. `out` is edited
 * in place (must hold maxlen+1); returns true if the user pressed OK, false if BACK. */
static const char *const KB_LOW[4]  = { "1234567890", "qwertyuiop", "asdfghjkl@", "zxcvbnm.-_" };
static const char *const KB_UPP[4]  = { "!?#$%&*()+", "QWERTYUIOP", "ASDFGHJKL:", "ZXCVBNM,;/" };
static const char *const KB_CTRL[5] = { "aA", "space", "del", "back", "ok" };
#define KB_COLS 10
#define KB_ROWS 4        /* character rows; row 4 is the control row */

static bool keyboard_input(const char *title, char *out, int maxlen) {
    const int ks   = s_info_scale;
    const int HS   = s_info_scale + 1;
    const int fy   = s_crumb_y + HS * 8 + 8;           /* text field top */
    const int fh   = ks * 8 + 8;
    const int gy   = fy + fh + 8;                       /* key grid top */
    const int rows = KB_ROWS + 1;
    const int rowh = (CB_BOT - gy) / rows;
    const int colw = (s_gw - 8) / KB_COLS;
    const int gx   = s_gx + 4;
    int cr = 1, cc = 0;                                 /* start on 'q' */
    bool shift = false;

    auto keychar = [&](int r, int c) -> char {
        const char *row = (shift ? KB_UPP : KB_LOW)[r];
        return (c >= 0 && c < (int)strlen(row)) ? row[c] : '\0';
    };
    auto draw_field = [&]() {
        fill_rect(s_scratch, s_gw - 8, fh, s_platform);
        board_lcd_blit(gx, fy, s_gw - 8, fh, s_scratch);
        draw_text(gx + 6, fy + (fh - 5 * ks) / 2, out[0] ? out : "_", ks, s_fg, s_platform);
    };
    auto draw_key = [&](int x, int y, int w, const char *label, bool hl) {
        int h = rowh - 3;
        blit_round_rect(x, y, w - 3, h, 4, hl ? s_accent : s_titlebar);
        int tw = (int)strlen(label) * 4 * ks;
        draw_text(x + (w - 3 - tw) / 2, y + (h - 5 * ks) / 2, label, ks, hl ? s_bg : s_fg, hl ? s_accent : s_titlebar);
    };
    auto redraw_keys = [&]() {
        char lab[2] = { 0 };
        for (int r = 0; r < KB_ROWS; r++)
            for (int c = 0; c < KB_COLS; c++) {
                lab[0] = keychar(r, c);
                draw_key(gx + c * colw, gy + r * rowh, colw, lab[0] ? lab : " ", cr == r && cc == c);
            }
        int cw = (s_gw - 8) / 5;
        for (int c = 0; c < 5; c++)
            draw_key(gx + c * cw, gy + KB_ROWS * rowh, cw, KB_CTRL[c], cr == KB_ROWS && cc == c);
    };

    board_lcd_fill(s_bg);
    draw_text_centered(s_W / 2, s_crumb_y, title, HS, s_fg, s_bg);
    board_draw_touch_deck();
    draw_field();
    redraw_keys();

    uint8_t prev = input_poll();   /* seed with held buttons so a press carried in from the previous screen doesn't re-fire */
    for (;;) {
        uint8_t m = input_poll();
        uint8_t p = (uint8_t)(m & ~prev);
        prev = m;
#ifdef FB_DUMP
        if (p & INPUT_PAUSE) carousel_fb_dump();
#endif
        int cols_here = (cr == KB_ROWS) ? 5 : KB_COLS;
        if      (p & INPUT_DOWN)  cr = (cr + 1) % rows;
        else if (p & INPUT_UP)    cr = (cr + rows - 1) % rows;
        else if (p & INPUT_RIGHT) cc = (cc + 1) % cols_here;
        else if (p & INPUT_LEFT)  cc = (cc + cols_here - 1) % cols_here;
        else if (p & INPUT_X)     { int l = (int)strlen(out); if (l) { out[l - 1] = '\0'; draw_field(); } vTaskDelay(pdMS_TO_TICKS(40)); continue; }
        else if (p & INPUT_O) {
            if (cr < KB_ROWS) {
                char ch = keychar(cr, cc);
                int l = (int)strlen(out);
                if (ch && l < maxlen) { out[l] = ch; out[l + 1] = '\0'; draw_field(); }
            } else switch (cc) {
                case 0: shift = !shift; redraw_keys(); break;
                case 1: { int l = (int)strlen(out); if (l < maxlen) { out[l] = ' '; out[l + 1] = '\0'; draw_field(); } break; }
                case 2: { int l = (int)strlen(out); if (l) { out[l - 1] = '\0'; draw_field(); } break; }
                case 3: return false;
                case 4: return true;
            }
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        } else { vTaskDelay(pdMS_TO_TICKS(40)); continue; }
        if (cc >= cols_here && cr != KB_ROWS) cc = KB_COLS - 1;   /* leaving the (narrower) ctrl row */
        { int cw2 = (cr == KB_ROWS) ? 5 : KB_COLS; if (cc >= cw2) cc = cw2 - 1; }
        redraw_keys();
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

/* A modal message ("connecting...", "connected", "scan failed", ...); if wait_x, block until X. */
static void wifi_msg(const char *title, const char *msg, uint16_t col, bool wait_x) {
    const int HS = s_info_scale + 1;
    board_lcd_fill(s_bg);
    draw_text_centered(s_W / 2, s_crumb_y, title, HS, s_fg, s_bg);
    blit_round_rect(s_W / 2 - 26, s_crumb_y + HS * 8, 52, 3, 1, s_accent);
    board_draw_touch_deck();
    draw_text_centered(s_W / 2, s_ty + s_th / 2, msg, s_info_scale + 1, col, s_bg);
    if (!wait_x) return;
    uint8_t prev = input_poll();   /* seed with held buttons so a press carried in from the previous screen doesn't re-fire */
    for (;;) {
        uint8_t m = input_poll(); uint8_t p = (uint8_t)(m & ~prev); prev = m;
#ifdef FB_DUMP
        if (p & INPUT_PAUSE) carousel_fb_dump();
#endif
        if (p & (INPUT_X | INPUT_O)) return;
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

/* One selectable list row inside the game column: label (info_scale+1) on the left, optional sub-label on the
 * right, both vertically centred in a snug accent pill when highlighted. Row height/pitch below are derived
 * from the same text size, so the pill hugs the text (not the old too-tall bar). y = row (pill) top. */
static int wifi_row_h(void)  { return 5 * (s_info_scale + 1) + 8; }   /* pill height: label glyph + padding */
static int wifi_row_dy(void) { return wifi_row_h() + 8; }             /* row pitch */
static void draw_pill_row(int y, const char *label, const char *sub, bool hl) {
    const int ls = s_info_scale + 1, ss = s_info_scale, rh = wifi_row_h();
    if (hl) blit_round_rect(s_gx + 16, y, s_gw - 32, rh, rh / 2, s_accent);
    /* A row with a sub-label is a LIST item (SSID left, signal right), so it stays left-aligned. A row without
     * one is a BUTTON — centre it in the pill, which is what it reads as. */
    const int ty = y + (rh - 5 * ls) / 2;
    if (sub && *sub) draw_text(s_gx + 24, ty, label, ls, hl ? s_bg : s_fg, hl ? s_accent : s_bg);
    else             draw_text_centered(s_gx + s_gw / 2, ty, label, ls, hl ? s_bg : s_fg, hl ? s_accent : s_bg);
    if (sub && *sub)
        draw_text(s_gx + s_gw - 24 - (int)strlen(sub) * 4 * ss, y + (rh - 5 * ss) / 2,
                  sub, ss, hl ? s_bg : s_dim, hl ? s_accent : s_bg);
}

/* A scrolling selectable list (title + up to `n` rows, right-aligned sub-labels). Returns the chosen index on
 * O, or -1 on X. `first` (persisted by caller) tracks the scroll window. */
static int wifi_list(const char *title, const char *hint, const char *const *items,
                     const char *const *subs, int n, int *sel_io) {
    const int HS = s_info_scale + 1, VS = s_info_scale;
    const int top = s_crumb_y + HS * 8 + VS * 8 + 24;
    const int dy  = wifi_row_dy();
    const int vis = (CB_BOT - top) / dy;                /* rows that fit above the deck */
    int sel = *sel_io, first = 0;
    uint8_t prev = input_poll();   /* seed with held buttons so a press carried in from the previous screen doesn't re-fire */
    for (;;) {
        if (sel < first) first = sel;
        if (sel >= first + vis) first = sel - vis + 1;
        board_lcd_fill(s_bg);
        draw_text_centered(s_W / 2, s_crumb_y, title, HS, s_fg, s_bg);
        blit_round_rect(s_W / 2 - 26, s_crumb_y + HS * 8, 52, 3, 1, s_accent);
        if (hint) draw_text_centered(s_W / 2, s_crumb_y + HS * 8 + 12, hint, VS, s_dim, s_bg);
        board_draw_touch_deck();
        for (int i = first; i < n && i < first + vis; i++)
            draw_pill_row(top + (i - first) * dy, items[i], subs ? subs[i] : NULL, i == sel);
        for (;;) {
            uint8_t m = input_poll(); uint8_t p = (uint8_t)(m & ~prev); prev = m;
#ifdef FB_DUMP
            if (p & INPUT_PAUSE) carousel_fb_dump();
#endif
            if (p & INPUT_X) { *sel_io = sel; return -1; }
            if (p & INPUT_O) { *sel_io = sel; return sel; }
            if (n && (p & INPUT_DOWN)) { sel = (sel + 1) % n; break; }
            if (n && (p & INPUT_UP))   { sel = (sel + n - 1) % n; break; }
            vTaskDelay(pdMS_TO_TICKS(40));
        }
    }
}

static const char *rssi_bars(int8_t r) {
    return r >= -55 ? "||||" : r >= -66 ? "|||" : r >= -76 ? "||" : "|";
}

/* Scan -> pick a network -> (keyboard for the password unless open) -> connect -> persist on success. */
/* Lending the SDMMC host to the radio (WC-6). On the P4 the SD and the C6 share one host, so any screen that
 * brings the radio up must give the card back for the duration. Tied to scope rather than written at each exit:
 * forgetting it on some future error path would not crash — it would leave the card unmounted and the games
 * carousel silently empty, with the cause several screens away. A no-op on boards whose SD has its own bus. */
struct SdHostLoan {
    bool lent = false;
    void lend() {
#ifdef BOARD_HAS_SDMMC
        lent = (board_sd_unmount(s_sd_mount.c_str()) == ESP_OK);
#endif
    }
    ~SdHostLoan() {
#ifdef BOARD_HAS_SDMMC
        if (lent && board_sd_mount(s_sd_mount.c_str()) != ESP_OK)
            wifi_msg("wifi", "sd remount failed", s_missing, true);   /* don't fail silently into an empty carousel */
#endif
    }
};

static void run_wifi_scan_connect(void) {
    wifi_msg("wifi", "scanning...", s_dim, false);
    static wifi_ap_t aps[16];
    int n = wifi_mgr_scan(aps, 16);
    if (n <= 0) { wifi_msg("wifi", n == 0 ? "no networks" : "scan failed", s_dim, true); return; }

    static const char *items[16];
    static char subbuf[16][8];
    static const char *subs[16];
    for (int i = 0; i < n; i++) {
        items[i] = aps[i].ssid;
        snprintf(subbuf[i], sizeof subbuf[i], "%s", aps[i].open ? "open" : rssi_bars(aps[i].rssi));
        subs[i] = subbuf[i];
    }
    int sel = 0;
    for (;;) {
        int pick = wifi_list("networks", "o join   x back", items, subs, n, &sel);
        if (pick < 0) return;
        char pass[WIFI_PASS_MAXLEN + 1] = { 0 };
        if (!aps[pick].open && !keyboard_input("password", pass, WIFI_PASS_MAXLEN)) continue;
        wifi_msg("wifi", "connecting...", s_accent, false);
        if (wifi_mgr_connect(aps[pick].ssid, pass, 15000) == ESP_OK) {
            wifi_mgr_save(aps[pick].ssid, pass);
            wifi_msg("wifi", "connected", s_accent, true);
            return;
        }
        wifi_msg("wifi", "failed to connect", s_missing, true);   /* back to the list to retry */
    }
}

static void run_wifi(void) {
    /* The radio is off until someone asks for it (WC-5). Hold a reference for as long as this screen is open and
     * drop it on the way out, which tears the stack (and, on the P4, the C6) back down. The bring-up is blocking
     * — ~2.3 s on the P4 for the esp-hosted handshake — so tell the user what the wait is. */
    wifi_msg("wifi", "starting radio...", s_dim, false);
    /* WC-6: this board's SD and its C6 radio share one SDMMC host, so the SD has to give it up while the radio
     * is in use. Safe here because nothing in this screen touches storage, and the launcher holds no open file
     * handles (cover art is decoded into PSRAM at browse time, not streamed).
     *
     * The remount is tied to scope rather than written at each exit ON PURPOSE. Forgetting it on some future
     * error path would not crash — it would leave the card unmounted and the games carousel silently empty, with
     * the cause several screens away. A guard makes that unforgettable. */
    SdHostLoan sd_host;
    sd_host.lend();

    esp_err_t werr = wifi_mgr_acquire();
    if (werr != ESP_OK) {                  /* no radio (P4: the C6 link never came up) — say so, don't pretend */
        wifi_msg("wifi", "radio unavailable", s_missing, true);
        return;                            /* ~SdHostLoan gives the card back */
    }
    /* Rejoin the saved network so the screen opens showing the real state rather than a bare OFFLINE. Failure is
     * fine and expected (out of range, credentials changed) — the user can scan from here. */
    if (wifi_mgr_autoconnect(12000) == ESP_ERR_NOT_FOUND) { /* nothing saved yet: straight to the menu */ }

    const int HS = s_info_scale + 1, VS = s_info_scale;
    int sel = 0;
    uint8_t prev = input_poll();   /* seed with held buttons so a press carried in from the previous screen doesn't re-fire */
    for (;;) {
        wifi_status_t st; wifi_mgr_status(&st);
        board_lcd_fill(s_bg);
        draw_text_centered(s_W / 2, s_crumb_y, "wifi", HS, s_fg, s_bg);
        blit_round_rect(s_W / 2 - 26, s_crumb_y + HS * 8, 52, 3, 1, s_accent);
        draw_text_centered(s_W / 2, s_crumb_y + HS * 8 + 12, "o select   x back", VS, s_dim, s_bg);
        board_draw_touch_deck();
        int y = s_crumb_y + HS * 8 + VS * 8 + 30;
        if (st.connected) {
            draw_kv(y, "status", "online", VS, VS, s_accent, s_fg);      y += VS * 11;
            draw_kv(y, "ssid",   st.ssid, VS, VS, s_fg, s_fg);           y += VS * 11;
            draw_kv(y, "ip",     st.ip,   VS, VS, s_fg, s_fg);           y += VS * 11;
        } else {
            draw_kv(y, "status", "offline", VS, VS, s_dim, s_dim);       y += VS * 11;
        }
        y += VS * 10;
        const char *actions[2] = { st.connected ? "scan / reconnect" : "scan & connect", "forget network" };
        for (int i = 0; i < 2; i++)
            draw_pill_row(y + i * wifi_row_dy(), actions[i], NULL, i == sel);
        for (;;) {
            uint8_t m = input_poll(); uint8_t p = (uint8_t)(m & ~prev); prev = m;
#ifdef FB_DUMP
            if (p & INPUT_PAUSE) carousel_fb_dump();
#endif
            if (p & INPUT_X) {
                wifi_mgr_release();   /* last reference out => radio powers down, then ~SdHostLoan takes the
                                       * SDMMC host back for the SD. Order matters: the radio must let go first. */
                return;
            }
            if (p & (INPUT_DOWN | INPUT_UP)) { sel ^= 1; break; }
            if (p & INPUT_O) {
                if (sel == 0) run_wifi_scan_connect();
                else { wifi_mgr_forget(); wifi_msg("wifi", "forgotten", s_dim, true); }
                prev = input_poll();   /* the button that closed the submenu may still be held — see run_settings */
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(40));
        }
    }
}


/* ---------- Settings -> SYSTEM UPDATE (WC-4a) ----------------------------------------------------------- */

/* Where to look for the manifest — CONFIG_PICO_E32_OTA_MANIFEST_URL (main/Kconfig.projbuild), so a build is
 * always explicit about what it will trust. Empty = the menu says so instead of pretending.
 * Kconfig rather than a -D define because a quoted string does not survive make -> idf.py -> CMake -> compiler:
 * the quotes are stripped and the macro silently ends up empty. Verified the hard way. */
#ifdef CONFIG_PICO_E32_OTA_MANIFEST_URL
#define OTA_MANIFEST_URL CONFIG_PICO_E32_OTA_MANIFEST_URL
#else
#define OTA_MANIFEST_URL ""
#endif

static int  s_ota_pct = -1;      /* last painted percentage, so the bar only repaints when it moves */

static void ota_draw_progress(size_t done, size_t total) {
    int pct = total ? (int)((done * 100) / total) : 0;
    if (pct == s_ota_pct) return;
    s_ota_pct = pct;

    const int VS = s_info_scale;
    int y  = s_crumb_y + (s_info_scale + 1) * 8 + VS * 8 + 60;
    int bw = s_gw - 40, bx = s_gx + 20, bh = VS * 6;

    blit_round_rect(bx, y, bw, bh, 3, s_platform);                       /* track */
    if (pct > 0) blit_round_rect(bx, y, (bw * pct) / 100, bh, 3, s_accent);

    char line[40];
    snprintf(line, sizeof line, "%d%%  %uk / %uk", pct,
             (unsigned)(done / 1024), (unsigned)(total / 1024));
    int ty = y + bh + 10;
    blit_round_rect(s_gx + 20, ty, s_gw - 40, VS * 6, 0, s_bg);          /* clear the old figures */
    draw_text_centered(s_W / 2, ty, line, VS, s_fg, s_bg);
}

static bool ota_progress_cb(size_t done, size_t total, void *user) {
    (void)user;
    ota_draw_progress(done, total);
    /* X cancels mid-download; the component aborts and leaves the boot slot untouched. */
    return !(input_poll() & INPUT_X);
}

/* Copy `src` into `dst`, clipped to `max_chars` glyphs (ellipsis on the tail when it does not fit). Used for
 * anything drawn from a manifest: draw_kv right-aligns by string length and neither it nor draw_text clips, so
 * an over-long value walks left through its own label and, far enough, hands a negative x to the panel blit. */
static void fit_text(char *dst, size_t dst_sz, const char *src, int max_chars) {
    if (!dst || dst_sz == 0) return;
    if (max_chars < 1) max_chars = 1;
    if ((size_t)max_chars > dst_sz - 1) max_chars = (int)dst_sz - 1;
    size_t n = strnlen(src ? src : "", (size_t)max_chars);
    memcpy(dst, src ? src : "", n);
    dst[n] = '\0';
    if (src && src[n] != '\0' && n >= 1) dst[n - 1] = '~';   /* the font has no ellipsis glyph */
}

static void run_update(void) {
    const int HS = s_info_scale + 1, VS = s_info_scale;
    char cur[OTA_VERSION_MAXLEN + 1];
    ota_current_version(cur, sizeof cur);

    auto header = [&](const char *sub) {
        board_lcd_fill(s_bg);
        draw_text_centered(s_W / 2, s_crumb_y, "system update", HS, s_fg, s_bg);
        blit_round_rect(s_W / 2 - 26, s_crumb_y + HS * 8, 52, 3, 1, s_accent);
        draw_text_centered(s_W / 2, s_crumb_y + HS * 8 + 12, sub, VS, s_dim, s_bg);
        board_draw_touch_deck();
    };

    if (!OTA_MANIFEST_URL[0]) {           /* no endpoint compiled in — say so rather than fail obscurely */
        wifi_msg("update", "no update url in build", s_missing, true);
        return;
    }

    int sel = 0;
    uint8_t prev = input_poll();
    for (;;) {
        header("o select   x back");
        int y = s_crumb_y + HS * 8 + VS * 8 + 30;
        draw_kv(y, "current", cur, VS, VS, s_fg, s_fg);   y += VS * 11;
        y += VS * 10;
        draw_pill_row(y, "check for update", NULL, sel == 0);

        bool act = false;
        while (!act) {
            uint8_t m = input_poll(); uint8_t p = (uint8_t)(m & ~prev); prev = m;
#ifdef FB_DUMP
            if (p & INPUT_PAUSE) carousel_fb_dump();
#endif
            if (p & INPUT_X) return;
            if (p & INPUT_O) { act = true; }
            vTaskDelay(pdMS_TO_TICKS(40));
        }

        /* --- one network session: radio up (and, on the P4, the SD host lent out) for check + install --- */
        wifi_msg("update", "starting radio...", s_dim, false);
        SdHostLoan sd_host;
        sd_host.lend();
        if (wifi_mgr_acquire() != ESP_OK) { wifi_msg("update", "radio unavailable", s_missing, true); return; }

        wifi_msg("update", "connecting...", s_dim, false);
        esp_err_t cerr = wifi_mgr_autoconnect(15000);
        if (cerr != ESP_OK) {
            wifi_mgr_release();
            /* "no saved network" and "the join failed" send the user to different places — the first to the WiFi
             * screen to join one, the second to try again or move closer. Don't collapse them into one message. */
            wifi_msg("update", cerr == ESP_ERR_NOT_FOUND ? "no saved wifi - join one first" : "cant reach network",
                     s_missing, true);                          /* ~SdHostLoan gives the card back */
            prev = input_poll();
            continue;          /* back to the screen: a transient failure should be retryable in place */
        }

        wifi_msg("update", "checking...", s_dim, false);
        ota_release_t rel;
        esp_err_t err = ota_check(OTA_MANIFEST_URL, &rel, 10000);
        if (err != ESP_OK) {
            wifi_mgr_release();
            const char *cm = "check failed";
            if (err == ESP_ERR_INVALID_RESPONSE)     cm = "bad manifest";
            else if (err == ESP_ERR_INVALID_VERSION) cm = "update is for another board";
            wifi_msg("update", cm, s_missing, true);
            prev = input_poll();
            continue;
        }
        if (!ota_is_newer(&rel)) {
            wifi_mgr_release();
            wifi_msg("update", "already up to date", s_accent, true);
            prev = input_poll();
            continue;
        }

        /* Offer it, with the version actually on the other end — never install without showing what. */
        header("o install   x cancel");
        y = s_crumb_y + HS * 8 + VS * 8 + 30;
        /* These strings come from the MANIFEST, i.e. off the network, and draw_kv right-aligns without
         * clipping: a long enough value overruns its label and a longer one produces a negative x that goes
         * straight to the panel blit. Clamp to what the row can actually hold. */
        const int val_px  = s_gw - 24 - 24 - (int)strlen("current") * 4 * VS;
        const int val_max = val_px / (4 * VS) > 0 ? val_px / (4 * VS) : 1;
        char nv[OTA_VERSION_MAXLEN + 1], bv[32];
        fit_text(nv, sizeof nv, rel.version, val_max);
        fit_text(bv, sizeof bv, rel.build,   val_max);
        draw_kv(y, "current", cur,  VS, VS, s_dim, s_dim);   y += VS * 11;
        draw_kv(y, "new",     nv,   VS, VS, s_fg, s_accent); y += VS * 11;
        char sz[24]; snprintf(sz, sizeof sz, "%uk", (unsigned)(rel.size / 1024));
        draw_kv(y, "size",    sz,   VS, VS, s_fg, s_fg);     y += VS * 11;
        if (bv[0]) { draw_kv(y, "built", bv, VS, VS, s_dim, s_dim); y += VS * 11; }

        prev = input_poll();
        bool go = false, done_choosing = false;
        while (!done_choosing) {
            uint8_t m = input_poll(); uint8_t p = (uint8_t)(m & ~prev); prev = m;
#ifdef FB_DUMP
            if (p & INPUT_PAUSE) carousel_fb_dump();   /* every other screen is capturable; so is this one */
#endif
            if (p & INPUT_O) { go = true;  done_choosing = true; }
            if (p & INPUT_X) { go = false; done_choosing = true; }
            vTaskDelay(pdMS_TO_TICKS(40));
        }
        if (!go) { wifi_mgr_release(); prev = input_poll(); continue; }   /* cancelled the install, not the screen */

        header("downloading   x cancel");
        s_ota_pct = -1;
        err = ota_apply(&rel, ota_progress_cb, NULL);
        wifi_mgr_release();

        if (err == ESP_OK) {
            wifi_msg("update", "installed - rebooting", s_accent, false);
            vTaskDelay(pdMS_TO_TICKS(1200));
            esp_restart();                       /* new image boots pending-verify; the launcher confirms it */
        }
        /* Every failure below left the running firmware untouched — say which, don't just say "failed". */
        const char *why = "update failed";
        if (err == ESP_ERR_INVALID_CRC)        why = "bad image - not installed";
        else if (err == ESP_ERR_INVALID_SIZE)  why = "wrong size - not installed";
        else if (err == ESP_ERR_INVALID_STATE) why = "cancelled";
        wifi_msg("update", why, s_missing, true);
        prev = input_poll();          /* the loop is real: failures land back on the screen, not in Settings */
    }
}

#endif  /* BOARD_HAS_WIFI */

static void run_settings(void) {
    const int HS = s_info_scale + 1;   /* header title */
    const int LS = s_info_scale + 1;   /* row label */
    const int VS = s_info_scale;       /* subtitle + row value */

    /* Rows are a selectable list now (up/down move, L/R adjusts the row if it adjusts, O opens a submenu). The
     * set is board-dependent: WIFI only appears where there's a radio (BOARD_HAS_WIFI). */
    enum { R_ACCENT, R_WIFI, R_UPDATE, R_BRIGHT, R_VOL };
    int kinds[5], nrows = 0;
    kinds[nrows++] = R_ACCENT;
#ifdef BOARD_HAS_WIFI
    kinds[nrows++] = R_WIFI;
    kinds[nrows++] = R_UPDATE;
#endif
    kinds[nrows++] = R_BRIGHT;
    kinds[nrows++] = R_VOL;

    int sel = 0;
    const int y0 = s_crumb_y + HS * 8 + VS * 8 + 30;
    const int dy = LS * 12;
    auto draw_all = [&]() {
        board_lcd_fill(s_bg);
        draw_text_centered(s_W / 2, s_crumb_y, "settings", HS, s_fg, s_bg);
        blit_round_rect(s_W / 2 - 26, s_crumb_y + HS * 8, 52, 3, 1, s_accent);
        draw_text_centered(s_W / 2, s_crumb_y + HS * 8 + 12, "l/r adjust  o open  x back", VS, s_dim, s_bg);
        board_draw_touch_deck();
        for (int i = 0; i < nrows; i++) {
            int ry = y0 + i * dy;
            bool hl = (i == sel);
            uint16_t lc = hl ? s_accent : s_fg;
            switch (kinds[i]) {
            case R_ACCENT: {
                draw_kv(ry, "accent", ACCENTS[s_accent_idx].name, LS, VS, lc, s_fg);
                int vw = (int)strlen(ACCENTS[s_accent_idx].name) * 4 * VS;
                int sw = LS * 13, sh = LS * 7;
                blit_round_rect(s_gx + s_gw - 24 - vw - 14 - sw, ry + (LS * 5 - sh) / 2, sw, sh, 5, s_accent);
                break; }
#ifdef BOARD_HAS_WIFI
            case R_WIFI: {
                wifi_status_t st; wifi_mgr_status(&st);
                draw_kv(ry, "wifi", st.connected ? st.ssid : "off", LS, VS, lc, st.connected ? s_accent : s_dim);
                break; }
#endif
#ifdef BOARD_HAS_WIFI
            case R_UPDATE: {
                /* "system update" + a full git-describe version is wider than the row and the two collide
                 * mid-line (seen on the panel, not in any log). Shorten both: the screen this opens is titled
                 * SYSTEM UPDATE, and the short hash is the part anyone reads. */
                char v[OTA_VERSION_MAXLEN + 1]; ota_current_version(v, sizeof v);
                char *dash = strchr(v, '-');
                if (dash) *dash = '\0';            /* drop the "-dirty"/"-N-gxxxx" tail */
                if (strlen(v) > 10) v[10] = '\0';
                draw_kv(ry, "update", v, LS, VS, lc, s_dim);
                break; }
#endif
            case R_BRIGHT: draw_kv(ry, "brightness", "soon", LS, VS, hl ? s_accent : s_dim, s_dim); break;
            case R_VOL:    draw_kv(ry, "volume",     "soon", LS, VS, hl ? s_accent : s_dim, s_dim); break;
            }
            if (hl) blit_round_rect(s_gx + 6, ry + 2, 4, LS * 5, 2, s_accent);   /* selection caret */
        }
    };
    draw_all();
    uint8_t prev = input_poll();   /* seed with held buttons so a press carried in from the previous screen doesn't re-fire */
    for (;;) {
        uint8_t m = input_poll();
        uint8_t p = (uint8_t)(m & ~prev);
        prev = m;
#ifdef FB_DUMP
        if (p & INPUT_PAUSE) carousel_fb_dump();
#endif
        if (p & INPUT_X) return;
        else if (p & INPUT_DOWN) { sel = (sel + 1) % nrows; draw_all(); }
        else if (p & INPUT_UP)   { sel = (sel + nrows - 1) % nrows; draw_all(); }
        else if (p & (INPUT_LEFT | INPUT_RIGHT)) {
            if (kinds[sel] == R_ACCENT) {   /* ACCENT is the one adjustable row; recolours the whole UI live */
                s_accent_idx = (s_accent_idx + ((p & INPUT_RIGHT) ? 1 : ACCENT_N - 1)) % ACCENT_N;
                apply_accent(); draw_all();
            }
        }
#ifdef BOARD_HAS_WIFI
        /* Re-seed prev after the submenu: the X that closed it is very likely STILL held (the serial backend
         * holds a tap for 6 frames, and a finger rests on the deck longer than that), and with a stale prev
         * that reads as a fresh X edge here — backing out of Settings too, two screens for one press. */
        else if ((p & INPUT_O) && kinds[sel] == R_WIFI)   { run_wifi();   draw_all(); prev = input_poll(); }
        else if ((p & INPUT_O) && kinds[sel] == R_UPDATE) { run_update(); draw_all(); prev = input_poll(); }
#endif
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

static void run_about(void) {
    const esp_app_desc_t *d = esp_app_get_description();
    const char *boardname =
#if CONFIG_IDF_TARGET_ESP32P4
        "esp32-p4";
#else
        "esp32-s3";
#endif
    const int HS = s_info_scale + 1;   /* ABOUT header */
    const int VS = s_info_scale;       /* info rows */
    const int PS = s_info_scale + 2;   /* PICO-E32 wordmark */
    board_lcd_fill(s_bg);
    draw_text_centered(s_W / 2, s_crumb_y, "about", HS, s_fg, s_bg);
    blit_round_rect(s_W / 2 - 26, s_crumb_y + HS * 8, 52, 3, 1, s_accent);
    int top = s_crumb_y + HS * 8 + 16;
    draw_text_centered(s_W / 2, top, "pico-e32", PS, s_accent, s_bg);
    draw_text_centered(s_W / 2, top + PS * 7, "fake-08 on z8lua", VS, s_dim, s_bg);
    board_draw_touch_deck();

    /* 7 info rows evenly packed between the subtitle and the band bottom (above the deck) — fits any panel. */
    char panel[24];  snprintf(panel, sizeof panel, "%dx%d", s_W, s_H);
    char psram[24];  snprintf(psram, sizeof psram, "%umb free",
                              (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / (1024 * 1024)));
    int y = top + PS * 7 + VS * 8 + 16, dy = (CB_BOT - y) / 7; if (dy < VS * 8) dy = VS * 8;
    draw_kv(y, "board",      boardname,               VS, VS, s_fg, s_fg);  y += dy;
    draw_kv(y, "panel",      panel,                    VS, VS, s_fg, s_fg);  y += dy;
    draw_kv(y, "version",    d ? d->version : "?",     VS, VS, s_fg, s_fg);  y += dy;
    draw_kv(y, "idf",        d ? d->idf_ver : "?",     VS, VS, s_fg, s_fg);  y += dy;
    draw_kv(y, "built",      d ? d->date : "?",        VS, VS, s_fg, s_fg);  y += dy;
    draw_kv(y, "psram",      psram,                    VS, VS, s_fg, s_fg);  y += dy;
    /* maintainer as one centered line — label+value won't both fit the column side by side (kept small: it's long) */
    draw_text_centered(s_W / 2, y, "maintainer: aldwin hermanudin", 2, s_dim, s_bg);

    uint8_t prev = input_poll();   /* seed with held buttons so a press carried in from the previous screen doesn't re-fire */
    for (;;) {
        uint8_t m = input_poll();
        uint8_t p = (uint8_t)(m & ~prev);
        prev = m;
#ifdef FB_DUMP
        if (p & INPUT_PAUSE) carousel_fb_dump();
#endif
        if (p & INPUT_X) return;
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

std::string carousel_launcher_run(Host *host, const std::string &start_dir) {
    s_sd_mount = start_dir;   /* WC-6: the WiFi session unmounts/remounts this to hand over the SDMMC host */
    s_W = board_lcd_width();
    s_H = board_lcd_height();
    s_bg       = board_lcd_rgb565(12, 13, 20);
    s_fg       = board_lcd_rgb565(240, 242, 248);
    s_dim      = board_lcd_rgb565(120, 124, 144);
    s_missing  = board_lcd_rgb565(46, 50, 62);
    s_accent   = board_lcd_rgb565(90, 162, 255);
    s_platform = board_lcd_rgb565(26, 30, 46);
    s_titlebar = board_lcd_rgb565(10, 12, 20);
    s_track    = board_lcd_rgb565(54, 60, 82);   /* position-bar track — clearly visible against the bg */
    s_case     = board_lcd_rgb565(110, 220, 220);  /* capitals: cyan (WC-2) */

    /* pull the per-panel layout from the board (see boards/<board>/board.cpp) */
    board_carousel_layout_t L;
    board_carousel_layout(&L);
    s_gx = L.game_x; s_gw = L.game_w;
    s_tw = L.thumb_w; s_th = L.thumb_h; s_ty = L.thumb_y;
    s_sidew = L.side_w; s_crumb_y = L.crumb_y;
    s_title_y = L.title_y; s_title_scale = L.title_scale; s_body_y = L.body_y; s_body_dy = L.body_dy;
    s_body_scale = L.body_scale; s_info_scale = L.info_scale;
    ESP_LOGI(TAG, "carousel layout: game %d+%d, thumb %dx%d @y%d, side %d",
             s_gx, s_gw, s_tw, s_th, s_ty, s_sidew);

    /* sized to the largest blit = the centre thumbnail */
    s_scratch = (uint16_t *)heap_caps_malloc((size_t)s_tw * s_th * 2, MALLOC_CAP_SPIRAM);
    if (!s_scratch) { ESP_LOGE(TAG, "no PSRAM for scratch"); return ""; }

    input_init();   /* the selected backend (touch when shipped, serial for headless HITL) — same seam the game uses */

    apply_accent();

#ifdef BOARD_HAS_WIFI
    /* Deliberately nothing here: the radio stays OFF at boot (WC-5). It is brought up only by whoever needs it
     * — Settings->WIFI while that screen is open, and later OTA/downloads — and torn down again after. */
#endif

    /* Top-level menu: Games (the SD carousel) / Settings / About. Games returns a cart path to launch;
     * Settings and About return here on X. Nav: up/down move, O select. */
    int menu_sel = 0;
    std::string result;
    for (;;) {
        render_menu(menu_sel, true);
        uint8_t prev = input_poll();   /* seed with held buttons so a press carried in from the previous screen doesn't re-fire */
        bool chosen = false;
        while (!chosen) {
            uint8_t m = input_poll();
            uint8_t pressed = (uint8_t)(m & ~prev);
            prev = m;
#ifdef FB_DUMP
            if (pressed & INPUT_PAUSE) carousel_fb_dump();
#endif
            if (pressed & (INPUT_DOWN | INPUT_RIGHT)) { menu_sel = (menu_sel + 1) % MENU_N; render_menu(menu_sel, false); }
            else if (pressed & (INPUT_UP | INPUT_LEFT)) { menu_sel = (menu_sel + MENU_N - 1) % MENU_N; render_menu(menu_sel, false); }
            else if (pressed & INPUT_O) chosen = true;
            vTaskDelay(pdMS_TO_TICKS(40));
        }
        if (menu_sel == 0) {
            result = run_carousel(host, start_dir);
            if (!result.empty()) break;      /* a cart was chosen -> launch it */
            /* else: backed out at the root -> the loop repaints the menu */
        } else if (menu_sel == 1) {
            run_settings();
        } else {
            run_about();
        }
    }

#ifdef BOARD_HAS_WIFI
    /* A cart never needs the network, and the game should not be sharing cores (or, on the P4, the SDIO bus and
     * esp-hosted's priority-23 tasks) with a radio nobody is using. Force it down whatever the refcount. */
    wifi_mgr_shutdown();
#endif
    cache_clear();          /* free the decoded cover cache (up to CACHE_MAX PSRAM RGBAs) before the VM boots */
    heap_caps_free(s_scratch); s_scratch = nullptr;
    board_lcd_fill(s_bg);   /* clear before the game boots */
    return result;
}
