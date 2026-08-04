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
static int s_sidew, s_crumb_y;  /* side peek width, breadcrumb top */
#define SIDE_DIM 120           /* /256 brightness for the two side peeks */

static int      s_W = 480, s_H = 800;
static uint16_t s_bg, s_fg, s_dim, s_missing, s_accent, s_platform, s_titlebar, s_track;

/* one reusable PSRAM scratch buffer, sized to the largest thing blitted (the 384x384 centre window) */
static uint16_t *s_scratch = nullptr;

/* ---------- tiny PICO-8 font text (3x5 glyphs, '#'=on) ---------- */
static const char *const *glyph_rows(char c) {
    if (c >= 'a' && c <= 'z') c -= 32;
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
        int ox = ci * 4 * scale;
        for (int r = 0; r < 5; r++)
            for (int c = 0; c < 3; c++)
                if (g[r][c] == '#')
                    for (int sy = 0; sy < scale; sy++)
                        for (int sx = 0; sx < scale; sx++)
                            buf[(r * scale + sy) * tw + (ox + c * scale + sx)] = fg;
    }
    board_lcd_blit(x, y, tw, th, buf);
    heap_caps_free(buf);
    return tw;
}
static void draw_text_centered(int cx, int y, const char *str, int scale, uint16_t fg, uint16_t bg) {
    int tw = (int)strlen(str) * 4 * scale;
    draw_text(cx - tw / 2, y, str, scale, fg, bg);
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
 * gaps). Used to place a folder's name on its thumbnail. Uppercased via glyph_rows; unknown glyphs skipped. */
static void glyphs_into(uint16_t *buf, int bufw, int bufh, int x0, int y0, const char *str, int scale, uint16_t fg) {
    int len = (int)strlen(str);
    for (int ci = 0; ci < len; ci++) {
        const char *const *g = glyph_rows(str[ci]);
        if (!g) continue;
        int ox = x0 + ci * 4 * scale;
        for (int r = 0; r < 5; r++)
            for (int c = 0; c < 3; c++)
                if (g[r][c] == '#')
                    for (int sy = 0; sy < scale; sy++)
                        for (int sx = 0; sx < scale; sx++) {
                            int px = ox + c * scale + sx, py = y0 + r * scale + sy;
                            if (px >= 0 && px < bufw && py >= 0 && py < bufh) buf[py * bufw + px] = fg;
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
        int ny = fy + fh + h * 9 / 100;
        int tw1 = (int)l1.size() * 4 * scale;
        glyphs_into(s_scratch, w, h, (w - tw1) / 2, ny, l1.c_str(), scale, fg);
        if (!l2.empty()) {
            int tw2 = (int)l2.size() * 4 * scale;
            glyphs_into(s_scratch, w, h, (w - tw2) / 2, ny + 6 * scale, l2.c_str(), scale, fg);
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
        std::string crumb = (curdir == floor) ? std::string("SD CARD") : display_name(basename_of(curdir));
        draw_text_centered(s_W / 2, s_crumb_y, crumb.c_str(), 3, s_fg, s_bg);
        blit_round_rect(s_W / 2 - 26, s_crumb_y + 24, 52, 3, 1, s_accent);
        board_draw_touch_deck();
    }

    if (N == 0) {
        if (full) draw_text_centered(s_W / 2, s_ty + s_th / 2, "EMPTY", 4, s_fg, s_bg);
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
#ifdef FB_DUMP
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
/* Deflate the full-res framebuffer and stream it compressed — see the declaration in carousel_launcher.h.
 * A flat carousel screen crushes to tens of KB, so the transfer stays well under the ~165 KB USB-JTAG stall
 * floor while keeping full 480x800 resolution. Reused by main.cpp's FB_DUMP game loop. */
void fb_dump_compressed(const unsigned short *fb, int w, int h) {
    if (!fb || w <= 0 || h <= 0) return;
    size_t raw = (size_t)w * h * 2;
    size_t bound = raw + raw / 2 + 256;
    static uint8_t *cbuf = nullptr; static size_t cap = 0;
    if (cap < bound) { if (cbuf) heap_caps_free(cbuf); cbuf = (uint8_t *)heap_caps_malloc(bound, MALLOC_CAP_SPIRAM); cap = bound; }
    if (!cbuf) return;
    /* Stream through a caller-owned tdefl_compressor in PSRAM. The one-call helpers (tdefl_compress_mem_to_mem)
     * MZ_MALLOC the ~300 KB compressor in INTERNAL RAM, which is exhausted here -> they silently return 0.
     * TDEFL_WRITE_ZLIB_HEADER gives a zlib stream Python zlib.decompress reads directly; low 12 bits = probes. */
    static tdefl_compressor *comp = nullptr;
    if (!comp) comp = (tdefl_compressor *)heap_caps_malloc(sizeof(tdefl_compressor), MALLOC_CAP_SPIRAM);
    if (!comp) return;
    /* Flags for a valid zlib stream = the level-6 probe count (128) | zlib header | ADLER32. The one-call
     * wrappers OR in TDEFL_COMPUTE_ADLER32 (miniz.c:198); omitting it leaves the checksum uncomputed and
     * Python zlib rejects the stream. (tdefl_create_comp_flags_from_zip_params is compiled out here.) */
    /* Callback mode: tdefl flushes its internal output buffer to fb_put_cb as it goes. This is the tested path
     * (what tdefl_compress_mem_to_output uses) — the direct pOut_buf mode produced a stream that broke ~4 KB in
     * here. We supply `comp` in PSRAM so nothing is MZ_MALLOC'd in the exhausted internal RAM. */
    int flags = TDEFL_WRITE_ZLIB_HEADER | TDEFL_COMPUTE_ADLER32 | 128;
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
    fwrite(hdr, 1, sizeof hdr, stdout);
    fwrite(cbuf, 1, clen, stdout);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(60));
    esp_log_level_set("*", ESP_LOG_INFO);
}
static void carousel_fb_dump() {
    int w = 0, h = 0;
    const uint16_t *fb = board_lcd_framebuffer(&w, &h);
    fb_dump_compressed(fb, w, h);
}
#endif

std::string carousel_launcher_run(Host *host, const std::string &start_dir) {
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

    /* pull the per-panel layout from the board (see boards/<board>/board.cpp) */
    board_carousel_layout_t L;
    board_carousel_layout(&L);
    s_gx = L.game_x; s_gw = L.game_w;
    s_tw = L.thumb_w; s_th = L.thumb_h; s_ty = L.thumb_y;
    s_sidew = L.side_w; s_crumb_y = L.crumb_y;
    ESP_LOGI(TAG, "carousel layout: game %d+%d, thumb %dx%d @y%d, side %d",
             s_gx, s_gw, s_tw, s_th, s_ty, s_sidew);

    /* sized to the largest blit = the centre thumbnail */
    s_scratch = (uint16_t *)heap_caps_malloc((size_t)s_tw * s_th * 2, MALLOC_CAP_SPIRAM);
    if (!s_scratch) { ESP_LOGE(TAG, "no PSRAM for scratch"); return ""; }

    input_init();   /* the selected backend (touch when shipped, serial for headless HITL) — same seam the game uses */

    std::string curdir = start_dir;
    std::vector<Entry> entries = build_entries(host, curdir, start_dir);
    int sel = 0;
    bool dirty = true, full = true;   /* full = repaint everything (dir change / first frame); else in-place */
    uint8_t prev = 0;
    std::string result;

    ESP_LOGI(TAG, "carousel up: %d entries in %s (free heap: %uK internal, %uK PSRAM)",
             (int)entries.size(), curdir.c_str(),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    for (;;) {
        if (dirty) { render(entries, sel, curdir, start_dir, full); dirty = false; full = false; }

        uint8_t m = input_poll();
        uint8_t pressed = (uint8_t)(m & ~prev);   /* act on the rising edge only */
        prev = m;
        int N = (int)entries.size();

#ifdef FB_DUMP
        if (pressed & INPUT_PAUSE) carousel_fb_dump();   /* dev: snapshot the current carousel screen */
#endif

        if (pressed & (INPUT_RIGHT | INPUT_DOWN)) {
            if (N) { sel = (sel + 1) % N; dirty = true; }
        } else if (pressed & (INPUT_LEFT | INPUT_UP)) {
            if (N) { sel = (sel - 1 + N) % N; dirty = true; }
        } else if (pressed & INPUT_O) {
            if (N) {
                Entry &e = entries[sel];
                if (e.isDir) {                                  /* enter folder / go up */
                    curdir = e.path;
                    cache_clear();
                    entries = build_entries(host, curdir, start_dir);
                    sel = 0; dirty = true; full = true;         /* breadcrumb + list change -> full repaint */
                    ESP_LOGI(TAG, "cd %s (%d entries)", curdir.c_str(), (int)entries.size());
                } else {                                        /* launch this cart */
                    result = e.path;
                    ESP_LOGI(TAG, "launch %s", result.c_str());
                    break;
                }
            }
        } else if (pressed & INPUT_X) {
            if (curdir != start_dir) {                          /* back / up a level */
                curdir = curdir.substr(0, curdir.find_last_of('/'));
                if (curdir.size() < start_dir.size()) curdir = start_dir;
                cache_clear();
                entries = build_entries(host, curdir, start_dir);
                sel = 0; dirty = true; full = true;             /* breadcrumb + list change -> full repaint */
            }
        }
        vTaskDelay(pdMS_TO_TICKS(40));
    }

    cache_clear();
    heap_caps_free(s_scratch); s_scratch = nullptr;
    board_lcd_fill(s_bg);   /* clear before the game boots */
    return result;
}
