/* Phase-0 Gate #3 — minimal ESP32Host: run a trivial PICO-8 cart end-to-end.
 *
 * z8lua runs the cart's _update/_draw; a minimal PICO-8 draw API writes into a
 * 128x128 indexed framebuffer; the frame is palette-expanded + 2x scaled to
 * 256x256 RGB565 and blitted via the board display (boards/<BOARD>/board.cpp). Prints FPS and a
 * per-frame framebuffer checksum over UART.
 *
 * The checksum is the camera-independent correctness check: the SAME cart + draw API
 * built for the host produces the SAME checksums, so "correct pixels in the framebuffer"
 * is verified without the bench camera; only "pixels on the physical panel" needs it.
 *
 * This is the seam that grows into the full ESP32Host (fake-08 port) in Phase 1. */

#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

/* ---- 128x128 indexed framebuffer + PICO-8 palette ---- */
static uint8_t  fb[128*128];
static uint8_t  g_col = 6;
static uint16_t pal565[16];
static const uint8_t PAL888[16][3] = {
  {0,0,0},{29,43,83},{126,37,83},{0,135,81},{171,82,54},{95,87,79},{194,195,199},{255,241,232},
  {255,0,77},{255,163,0},{255,236,39},{0,228,54},{41,173,255},{131,118,156},{255,119,168},{255,204,170},
};

/* HG-6 draw state, applied at the single pixel-write chokepoint below so every op (spr/map/print/
 * shapes) honours it. Defaults are the identity — no camera offset, full-screen clip, identity palette
 * — so nothing here changes output until a cart sets it (the existing carts are byte-identical). */
static int g_cam_x=0, g_cam_y=0;                                         /* camera() offset */
static int g_clip_x0=0, g_clip_y0=0, g_clip_x1=127, g_clip_y1=127;       /* clip() rect (inclusive) */
static uint8_t g_pal[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};      /* pal() draw palette */
static inline void px(int x,int y,int c){
    x-=g_cam_x; y-=g_cam_y;                                              /* camera */
    if(x<g_clip_x0||x>g_clip_x1||y<g_clip_y0||y>g_clip_y1) return;       /* clip */
    if((unsigned)x<128 && (unsigned)y<128) fb[y*128+x]=g_pal[c&15];      /* pal remap */
}
static inline void hline(int x0,int x1,int y,int c){ if(x0>x1){int t=x0;x0=x1;x1=t;} for(int x=x0;x<=x1;x++) px(x,y,c); }

/* ---- minimal PICO-8 draw API (C) ---- */
static int argi(lua_State *L,int i){ return (int)(int32_t)luaL_checknumber(L,i); }
static int col_or(lua_State *L,int i){ if(lua_gettop(L)>=i && !lua_isnil(L,i)){ g_col=argi(L,i)&15; } return g_col; }

static int l_cls(lua_State *L){ int c=lua_gettop(L)>=1?argi(L,1)&15:0; memset(fb,c,sizeof fb); return 0; }
static int l_color(lua_State *L){ g_col=argi(L,1)&15; return 0; }
static int l_pset(lua_State *L){ int x=argi(L,1),y=argi(L,2),c=col_or(L,3); px(x,y,c); return 0; }
static int l_rectfill(lua_State *L){
    int x0=argi(L,1),y0=argi(L,2),x1=argi(L,3),y1=argi(L,4),c=col_or(L,5);
    if(y0>y1){int t=y0;y0=y1;y1=t;} for(int y=y0;y<=y1;y++) hline(x0,x1,y,c); return 0;
}
static int l_rect(lua_State *L){
    int x0=argi(L,1),y0=argi(L,2),x1=argi(L,3),y1=argi(L,4),c=col_or(L,5);
    hline(x0,x1,y0,c); hline(x0,x1,y1,c); for(int y=y0;y<=y1;y++){px(x0,y,c);px(x1,y,c);} return 0;
}
static int l_circfill(lua_State *L){
    int cx=argi(L,1),cy=argi(L,2),r=argi(L,3),c=col_or(L,4);
    for(int dy=-r;dy<=r;dy++){ int dx=0; while(dx*dx+dy*dy<=r*r) dx++; hline(cx-dx+1,cx+dx-1,cy+dy,c); }
    return 0;
}
static int l_circ(lua_State *L){
    int cx=argi(L,1),cy=argi(L,2),r=argi(L,3),c=col_or(L,4);
    int x=r,y=0,err=1-r;
    while(x>=y){ px(cx+x,cy+y,c);px(cx+y,cy+x,c);px(cx-y,cy+x,c);px(cx-x,cy+y,c);
                 px(cx-x,cy-y,c);px(cx-y,cy-x,c);px(cx+y,cy-x,c);px(cx+x,cy-y,c);
                 y++; if(err<0) err+=2*y+1; else {x--; err+=2*(y-x)+1;} }
    return 0;
}
static int l_line(lua_State *L){
    int x0=argi(L,1),y0=argi(L,2),x1=argi(L,3),y1=argi(L,4),c=col_or(L,5);
    int dx=x1-x0<0?x0-x1:x1-x0, sx=x0<x1?1:-1;
    int dy=y1-y0<0?y1-y0:y0-y1, sy=y0<y1?1:-1, e=dx+dy;
    for(;;){ px(x0,y0,c); if(x0==x1&&y0==y1) break; int e2=2*e; if(e2>=dy){e+=dy;x0+=sx;} if(e2<=dx){e+=dx;y0+=sy;} }
    return 0;
}
static int l_nop(lua_State *L){ (void)L; return 0; }

/* ---- HG-3: sprite sheet + spr() ---- */
/* The active sprite sheet: 128x128 palette indices, one byte per pixel (the unpacked layout
 * gen_celeste_cart.py emits as CELESTE_GFX). Null when the cart has no sheet (the trivial cart),
 * in which case spr() is a no-op — so nothing here changes the Gate #3 trivial-cart output. */
static const uint8_t *g_gfx = nullptr;

/* Transparency table: colour c is skipped by spr() when g_palt[c]. PICO-8's default is colour 0
 * transparent, everything else opaque. l_palt below covers palt(); the pal() remap is HG-6. */
static uint8_t g_palt[16] = { 1 };

/* Blit sprite n — an 8x8 tile, or a w*8 x h*8 block — from the sheet to (x,y), skipping transparent
 * colours. Sprite n's top-left in the sheet is (n%16*8, n/16*8); flip_x/flip_y mirror the whole block
 * about its own centre. Shared by spr() and map(). */
static void draw_sprite(int n, int x, int y, int w, int h, bool fx, bool fy){
    if(!g_gfx) return;
    int sx=(n%16)*8, sy=(n/16)*8, pw=w*8, ph=h*8;
    for(int dy=0; dy<ph; dy++){
        int syy = sy + (fy ? ph-1-dy : dy);
        if((unsigned)syy>=128) continue;
        for(int dx=0; dx<pw; dx++){
            int sxx = sx + (fx ? pw-1-dx : dx);
            if((unsigned)sxx>=128) continue;
            uint8_t c = g_gfx[syy*128 + sxx] & 15;
            if(g_palt[c]) continue;          /* transparent */
            px(x+dx, y+dy, c);
        }
    }
}

/* spr(n, x, y, [w=1], [h=1], [flip_x=false], [flip_y=false]) */
static int l_spr(lua_State *L){
    int n=argi(L,1), x=argi(L,2), y=argi(L,3);
    int w = (lua_gettop(L)>=4 && !lua_isnil(L,4)) ? argi(L,4) : 1;
    int h = (lua_gettop(L)>=5 && !lua_isnil(L,5)) ? argi(L,5) : 1;
    bool fx = lua_gettop(L)>=6 && lua_toboolean(L,6);
    bool fy = lua_gettop(L)>=7 && lua_toboolean(L,7);
    draw_sprite(n,x,y,w,h,fx,fy);
    return 0;
}

/* HG-7: sspr(sx,sy,sw,sh, dx,dy, [dw=sw],[dh=dh], [flip_x],[flip_y])
 * Blit a source rect (sx,sy,sw,sh) from the sheet to (dx,dy,dw,dh), nearest-neighbour scaled, skipping
 * transparent colours. Source coords are PIXELS (not sprite units). */
static int l_sspr(lua_State *L){
    if(!g_gfx) return 0;
    int sx=argi(L,1), sy=argi(L,2), sw=argi(L,3), sh=argi(L,4), dx=argi(L,5), dy=argi(L,6);
    int dw = (lua_gettop(L)>=7 && !lua_isnil(L,7)) ? argi(L,7) : sw;
    int dh = (lua_gettop(L)>=8 && !lua_isnil(L,8)) ? argi(L,8) : sh;
    bool fx = lua_gettop(L)>=9  && lua_toboolean(L,9);
    bool fy = lua_gettop(L)>=10 && lua_toboolean(L,10);
    if(sw<=0||sh<=0||dw<=0||dh<=0) return 0;
    for(int oy=0; oy<dh; oy++){
        int syy = sy + (fy ? sh-1 - oy*sh/dh : oy*sh/dh);
        if((unsigned)syy>=128) continue;
        for(int ox=0; ox<dw; ox++){
            int sxx = sx + (fx ? sw-1 - ox*sw/dw : ox*sw/dw);
            if((unsigned)sxx>=128) continue;
            uint8_t c = g_gfx[syy*128 + sxx] & 15;
            if(g_palt[c]) continue;
            px(dx+ox, dy+oy, c);
        }
    }
    return 0;
}

/* palt([c],[t]) — transparency control. palt() with no args resets to the default (0 transparent);
 * palt(c,t) sets colour c transparent/opaque. (The colour-remap half of pal/palt is HG-6.) */
static int l_palt(lua_State *L){
    if(lua_gettop(L)==0){ memset(g_palt,0,sizeof g_palt); g_palt[0]=1; return 0; }
    int c=argi(L,1)&15; g_palt[c] = lua_toboolean(L,2) ? 1 : 0; return 0;
}

/* ---- HG-4: map + sprite flags ---- */
/* The active map (128x64 tile ids) and sprite-flag table (256 bytes), poke-able like PICO-8's memory
 * (mset/fset write them). Null when the cart has none — map() is then a no-op, so the trivial cart is
 * unaffected. gen_celeste_cart.py emits the FULL 128x64 map (rows 32-63 unpacked from gfx rows
 * 64-127), so rooms 16-31 are present, not just 0-15. */
static uint8_t *g_map = nullptr;
static uint8_t *g_gff = nullptr;

/* map(celx, cely, sx, sy, [celw=128], [celh=64], [layer=0])
 * Draw a celw x celh block of map cells, top-left cell (celx,cely), to screen pixel (sx,sy). Each
 * non-zero tile draws sprite `tile` (8x8); tile 0 is empty (skipped). With `layer` set, only tiles
 * whose sprite flags (fget) intersect it are drawn. */
static int l_map(lua_State *L){
    if(!g_map) return 0;
    int celx=argi(L,1), cely=argi(L,2), sx=argi(L,3), sy=argi(L,4);
    int celw = (lua_gettop(L)>=5 && !lua_isnil(L,5)) ? argi(L,5) : 128;
    int celh = (lua_gettop(L)>=6 && !lua_isnil(L,6)) ? argi(L,6) : 64;
    int layer= (lua_gettop(L)>=7 && !lua_isnil(L,7)) ? argi(L,7) : 0;
    for(int dy=0; dy<celh; dy++){
        int my=cely+dy; if((unsigned)my>=64) continue;
        for(int dx=0; dx<celw; dx++){
            int mx=celx+dx; if((unsigned)mx>=128) continue;
            uint8_t tile=g_map[my*128+mx];
            if(tile==0) continue;                                   /* tile 0 = empty */
            if(layer && g_gff && !(g_gff[tile] & layer)) continue;  /* layer filter via sprite flags */
            draw_sprite(tile, sx+dx*8, sy+dy*8, 1,1, false,false);
        }
    }
    return 0;
}
static int l_mget(lua_State *L){
    int x=argi(L,1), y=argi(L,2);
    uint8_t t = (g_map && (unsigned)x<128 && (unsigned)y<64) ? g_map[y*128+x] : 0;
    lua_pushnumber(L, z8::fix32((int)t)); return 1;
}
static int l_mset(lua_State *L){
    int x=argi(L,1), y=argi(L,2), v=argi(L,3);
    if(g_map && (unsigned)x<128 && (unsigned)y<64) g_map[y*128+x]=(uint8_t)v;
    return 0;
}
static int l_fget(lua_State *L){
    int n=argi(L,1);
    uint8_t fl = (g_gff && (unsigned)n<256) ? g_gff[n] : 0;
    if(lua_gettop(L)>=2 && !lua_isnil(L,2)){ int f=argi(L,2); lua_pushboolean(L,(fl>>f)&1); }
    else lua_pushnumber(L, z8::fix32((int)fl));
    return 1;
}
static int l_fset(lua_State *L){
    int n=argi(L,1); if(!g_gff || (unsigned)n>=256) return 0;
    if(lua_gettop(L)>=3){ int f=argi(L,2)&7; if(lua_toboolean(L,3)) g_gff[n]|=(1<<f); else g_gff[n]&=~(1<<f); }
    else g_gff[n]=(uint8_t)argi(L,2);
    return 0;
}

/* ---- HG-5: print() + a 3x5 font ----
 * The glyph shapes live in assets/pico8_font.h (a hand-authored PLACEHOLDER to be regenerated from a
 * real PICO-8 font — see that file's banner and docs/runtime/pico-e32-host-graphics.md). Geometry
 * matches PICO-8: 3px-wide glyphs, cursor advances 4px per char, 6px per line. PICO-8 renders
 * lowercase as caps, so a-z map to A-Z. Decoded once into g_font[char][row] (bit 2 = left pixel). */
#include "pico8_font.h"
static uint8_t g_font[128][5];
static int g_cur_x = 0, g_cur_y = 0;
static void font_init(void){
    static bool done=false; if(done) return; done=true;
    for(size_t i=0;i<sizeof(PICO8_FONT_GLYPHS)/sizeof(PICO8_FONT_GLYPHS[0]);i++){
        uint8_t *g = g_font[(unsigned char)PICO8_FONT_GLYPHS[i].c];
        for(int r=0;r<5;r++){ uint8_t b=0;
            for(int c=0;c<3;c++) if(PICO8_FONT_GLYPHS[i].rows[r][c]=='#') b |= (4>>c);
            g[r]=b; }
    }
    for(int c='a';c<='z';c++) memcpy(g_font[c], g_font[c-32], 5);   /* lowercase -> caps glyphs */
}
static void draw_char(unsigned char ch,int x,int y,int col){
    if(ch>=128) return;
    const uint8_t *g=g_font[ch];
    for(int r=0;r<5;r++) for(int c=0;c<3;c++) if(g[r]&(4>>c)) px(x+c,y+r,col);
}
/* print(str, [x], [y], [col]) — draws str; \n starts a new line. Advances the cursor 6px down, PICO-8
 * style. With x/y omitted, continues from the cursor. */
static int l_print(lua_State *L){
    size_t len=0; const char *s = lua_isstring(L,1) ? lua_tolstring(L,1,&len) : "";
    int x,y;
    if(lua_gettop(L)>=3 && !lua_isnil(L,2) && !lua_isnil(L,3)){ x=argi(L,2); y=argi(L,3); }
    else { x=g_cur_x; y=g_cur_y; }
    int col = (lua_gettop(L)>=4 && !lua_isnil(L,4)) ? (argi(L,4)&15) : g_col;
    int x0=x;
    for(size_t i=0;i<len;i++){
        char ch=s[i];
        if(ch=='\n'){ x=x0; y+=6; continue; }
        draw_char((unsigned char)ch, x, y, col); x+=4;
    }
    g_cur_x=x0; g_cur_y=y+6;   /* PICO-8 leaves the cursor at the next line */
    return 0;
}

/* ---- HG-6: pal / camera / clip ---- */
/* pal(c0,c1) remaps drawn colour c0 -> c1 (the DRAW palette — Celeste's flash effect). The p=1 SCREEN
 * palette isn't modelled (Celeste doesn't use it). pal() with no args resets the palette AND
 * transparency to defaults, as PICO-8 does. */
static int l_pal(lua_State *L){
    if(lua_gettop(L)==0){
        for(int i=0;i<16;i++) g_pal[i]=i;
        memset(g_palt,0,sizeof g_palt); g_palt[0]=1;
        return 0;
    }
    g_pal[argi(L,1)&15] = argi(L,2)&15;
    return 0;
}
/* camera([x],[y]) offsets all subsequent drawing by (-x,-y) — Celeste uses it for screen shake. */
static int l_camera(lua_State *L){
    g_cam_x = (lua_gettop(L)>=1 && !lua_isnil(L,1)) ? argi(L,1) : 0;
    g_cam_y = (lua_gettop(L)>=2 && !lua_isnil(L,2)) ? argi(L,2) : 0;
    return 0;
}
/* clip([x,y,w,h]) bounds drawing to a rectangle (in screen space, after camera); clip() resets to full. */
static int l_clip(lua_State *L){
    if(lua_gettop(L)==0){ g_clip_x0=0; g_clip_y0=0; g_clip_x1=127; g_clip_y1=127; return 0; }
    int x=argi(L,1), y=argi(L,2), w=argi(L,3), h=argi(L,4);
    g_clip_x0=x; g_clip_y0=y; g_clip_x1=x+w-1; g_clip_y1=y+h-1;
    return 0;
}

static void reg(lua_State *L,const char*n,lua_CFunction f){ lua_pushcfunction(L,f); lua_setglobal(L,n); }

/* Trivial cart: animated shapes (uses only cls/rectfill/circfill/pset + z8lua sin/cos). */
static const char *CART = R"lua(
-- Orientation + colour test. The L-shape fixes which way is up and whether the panel is
-- mirrored, using SHAPE only (colour-independent). The corner squares then name the colours.
function _draw()
 cls(0)
 -- thick bar along the TOP edge, full width
 rectfill(0,0,127,7,7)
 -- shorter bar down the LEFT edge -> "L" is asymmetric under rotation AND mirroring
 rectfill(0,0,7,63,7)
 -- one notch at top-left so we can tell top from bottom even if the L is ambiguous
 rectfill(0,0,15,15,7)
 -- corner markers: red=TL  green=TR  blue=BL  yellow=BR
 rectfill(20,20,35,35,8)
 rectfill(92,20,107,35,11)
 rectfill(20,92,35,107,12)
 rectfill(92,92,107,107,10)
 -- a single white pixel run pointing right along the middle: disambiguates left/right
 rectfill(64,62,120,65,7)
end
function _update() end
)lua";

/* FNV-1a over the framebuffer — a per-frame content fingerprint for host/device compare. */
static uint32_t fb_hash(void){ uint32_t h=2166136261u; for(size_t i=0;i<sizeof fb;i++){ h^=fb[i]; h*=16777619u; } return h; }

static lua_State *host_open_cart(const char *cart){
    for(int i=0;i<16;i++) pal565[i]=((PAL888[i][0]&0xF8)<<8)|((PAL888[i][1]&0xFC)<<3)|(PAL888[i][2]>>3);
    /* swap bytes for the 16-bit i80 bus (matches the board driver's SWAP_COLOR_BYTES) */
    for(int i=0;i<16;i++) pal565[i]=(uint16_t)((pal565[i]>>8)|(pal565[i]<<8));
    lua_State *L=luaL_newstate(); luaL_openlibs(L);
    reg(L,"cls",l_cls); reg(L,"color",l_color); reg(L,"pset",l_pset);
    reg(L,"rectfill",l_rectfill); reg(L,"rect",l_rect);
    reg(L,"circfill",l_circfill); reg(L,"circ",l_circ); reg(L,"line",l_line);
    reg(L,"spr",l_spr); reg(L,"palt",l_palt);   /* HG-3 */
    reg(L,"map",l_map); reg(L,"mget",l_mget); reg(L,"mset",l_mset);   /* HG-4 */
    reg(L,"fget",l_fget); reg(L,"fset",l_fset);
    font_init(); reg(L,"print",l_print);   /* HG-5 */
    reg(L,"pal",l_pal); reg(L,"camera",l_camera); reg(L,"clip",l_clip);   /* HG-6 */
    reg(L,"sspr",l_sspr);   /* HG-7 */
    /* remaining stubs a real cart might touch (audio) */
    const char *nops[]={"fillp","sfx","music",0};
    for(int i=0;nops[i];i++) reg(L,nops[i],l_nop);
    if(luaL_dostring(L,cart)){ printf("cart error: %s\n", lua_tostring(L,-1)); }
    lua_getglobal(L,"_init"); if(lua_isfunction(L,-1)) lua_pcall(L,0,0,0); else lua_pop(L,1);
    return L;
}
static lua_State *host_open(void){ return host_open_cart(CART); }
static void call_opt(lua_State *L, const char *fn){
    lua_getglobal(L, fn);
    if(!lua_isfunction(L,-1)){ lua_pop(L,1); return; }   /* absent: pop the non-function */
    if(lua_pcall(L,0,0,0)){ printf("%s: %s\n", fn, lua_tostring(L,-1)); lua_pop(L,1); }  /* success pops fn; error leaves msg */
}
static void host_frame(lua_State *L){ call_opt(L,"_update"); call_opt(L,"_draw"); }

#ifdef HOST_MAIN   /* host: run frames, print framebuffer checksums (no display) */
#include <stdlib.h>
/* Dump the raw indexed framebuffer so a frame can be *looked at*, not just hashed:
 *   P8_DUMP=dir ./bench            -> dir/frame_000.raw ... (128*128 bytes, 1 index/px)
 *   tools/p8_png.py dir/frame_000.raw out.png
 * The hash proves host and device agree; the image proves either is actually *right* —
 * a matching pair of wrong framebuffers hashes just fine. Camera-independent, so it works
 * while the bench rig is down (docs/runtime/pico-e32-host-graphics.md, HG-2). */
static void dump_frame(const char *dir, int f){
    char path[512];
    snprintf(path, sizeof path, "%s/frame_%03d.raw", dir, f);
    FILE *fp = fopen(path, "wb");
    if(!fp){ printf("dump: cannot write %s\n", path); return; }
    fwrite(fb, 1, sizeof fb, fp);
    fclose(fp);
}

/* ---- HG-3 spr() verification (host-only, P8_SPRTEST=1) ----
 * Loads a sprite sheet and a cart that exercises spr(): a grid of the first 64 sprites, the four
 * flip combos, and a 2x2 (w=h=2) block. Prefers Celeste's REAL sheet (the gitignored CELESTE_GFX,
 * so the grid shows Madeline/objects — HG-3's "Celeste sprites appear" check); falls back to a
 * synthetic asymmetric sheet when the asset hasn't been generated, so the test still runs. */
#if __has_include("celeste_cart.h")
#include "celeste_cart.h"
#define HAVE_CELESTE_GFX 1
#endif
/* Working sheet = the chosen source, plus a known asymmetric "F" painted into sprite 64 (0,32),
 * which sits BELOW the 64-sprite grid so it doesn't disturb it. The flip test uses sprite 64: an
 * asymmetric, non-blank glyph makes the mirror check meaningful (Celeste's sprite 0 is blank). */
static uint8_t s_sheet[128*128];
static const uint8_t *spr_test_sheet(const char **name){
#ifdef HAVE_CELESTE_GFX
    memcpy(s_sheet, CELESTE_GFX, sizeof s_sheet); *name = "CELESTE_GFX";
#else
    memset(s_sheet, 0, sizeof s_sheet); *name = "synthetic";
    for(int y=0;y<8;y++) for(int x=8;x<16;x++)                /* sprite 1: red 8x8 w/ transparent */
        s_sheet[y*128+x] = (y<4 && x>=12) ? 0 : 8;           /*   top-right quad, so the grid + */
    for(int y=0;y<8;y++) for(int x=16;x<24;x++) s_sheet[y*128+x]=12;  /* sprite 2: blue, non-blank */
#endif
    const int fx=0, fy=32;                                   /* sprite 64 = (64%16*8, 64/16*8) */
    for(int y=0;y<8;y++) for(int x=0;x<8;x++) s_sheet[(fy+y)*128+fx+x]=0;
    for(int i=0;i<7;i++) s_sheet[(fy+0)*128+fx+i]=7;         /* "F": top bar   */
    for(int i=0;i<8;i++) s_sheet[(fy+i)*128+fx+0]=7;         /*      left stem */
    for(int i=0;i<4;i++) s_sheet[(fy+3)*128+fx+i]=7;         /*      mid stub  */
    return s_sheet;
}
static const char *SPRTEST_CART = R"lua(
function _draw()
 cls(1)
 -- a grid of the first 64 sprites, one spr() call each (reproduces the sheet's top 32 rows)
 for ry=0,3 do for rx=0,15 do spr(ry*16+rx, rx*8, ry*8) end end
 -- flip test: the asymmetric "F" (sprite 64) drawn four ways -> it must point four different ways
 spr(64, 40,100)                  spr(64, 52,100, 1,1, true)
 spr(64, 40,112, 1,1, false,true) spr(64, 52,112, 1,1, true,true)
 -- size test: a 2x2 (w=h=2) block = sprites 0,1,16,17 (assembles a full character)
 spr(0, 80,104, 2,2)
end
function _update() end
)lua";

/* ---- HG-4 map() verification (host-only, P8_MAPTEST=1, P8_ROOM=n) ----
 * Draws Celeste room n full-screen via map(). Needs the real cart (there is no synthetic map).
 * Room n is the 16x16-cell block at map (n%8*16, n/8*16): rooms 0-15 come from __map__ (rows 0-31),
 * rooms 16-31 from the SHARED region (rows 32-63) — so P8_ROOM>=16 is the test that the shared-memory
 * extraction is real map, not noise. */
static const char *MAPTEST_CART = R"lua(
function _draw()
 cls(0)
 camera(CAMX, CAMY)                        -- HG-6: screen shake / scroll
 if FLASH==1 then for i=0,15 do pal(i,7) end end  -- HG-6: death-flash everything white
 map(RX*16, RY*16, 0, 0, 16, 16, LAYER)    -- LAYER=0 draws all; a set bit filters on sprite flags
 pal() camera()
end
function _update() end
)lua";

/* HG-5 print() verification (host-only, P8_TEXTTEST=1): the full glyph set + Celeste's real UI strings. */
static const char *TEXTTEST_CART = R"lua(
function _draw()
 cls(1)
 print("abcdefghijklm",3,3,7)   print("nopqrstuvwxyz",3,11,7)
 print("0123456789",3,20,10)    print(".,:;!?-+=/%*#()",3,28,12)
 print("old site",3,40,7)       print("deaths:12",70,40,7)
 print("x1000",3,49,8)          print("100 m",70,49,6)
 print("matt thorson",3,58,5)   print("noel berry",3,67,5)
 print("summit",3,76,13)        print("time 3:07",70,76,13)
end
function _update() end
)lua";

/* HG-6 pal/camera/clip verification (host-only, P8_GFXTEST=1). */
static const char *GFXTEST_CART = R"lua(
function _draw()
 cls(0)
 -- CLIP: fill the whole screen, but clipped to a box -> only the box fills
 clip(8,8,48,48)  rectfill(0,0,127,127,3)  clip()
 -- PAL: red(8) remapped to blue(12); then a control red with the palette reset
 pal(8,12)  rectfill(72,8,112,28,8)   pal()  rectfill(72,34,112,54,8)
 -- CAMERA: a marker at world (100,100) with camera(36,36) -> appears at screen (64,64)
 camera(36,36)  rectfill(100,100,112,112,10)  camera()
end
function _update() end
)lua";

int main(void){
    if(getenv("P8_TEXTTEST")){
        lua_State *L = host_open_cart(TEXTTEST_CART);
        host_frame(L);
        printf("texttest: fb_hash=%08x\n", fb_hash());
        const char *dump=getenv("P8_DUMP");
        if(dump){ dump_frame(dump,0); printf("dumped to %s/frame_000.raw\n", dump); }
        lua_close(L); return 0;
    }
    if(getenv("P8_GFXTEST")){
        lua_State *L = host_open_cart(GFXTEST_CART);
        host_frame(L);
        printf("gfxtest: fb_hash=%08x\n", fb_hash());
        const char *dump=getenv("P8_DUMP");
        if(dump){ dump_frame(dump,0); printf("dumped to %s/frame_000.raw\n", dump); }
        lua_close(L); return 0;
    }
    if(getenv("P8_MAPTEST")){
#ifdef HAVE_CELESTE_GFX
        static uint8_t map_buf[8192], gff_buf[256];
        memcpy(map_buf, CELESTE_MAP, sizeof map_buf);
        memcpy(gff_buf, CELESTE_GFF, sizeof gff_buf);
        g_map = map_buf; g_gff = gff_buf; g_gfx = CELESTE_GFX;
        int room = getenv("P8_ROOM") ? atoi(getenv("P8_ROOM")) : 1;
        int layer = getenv("P8_LAYER") ? atoi(getenv("P8_LAYER")) : 0;
        lua_State *L = host_open_cart(MAPTEST_CART);
        lua_pushnumber(L, z8::fix32(room%8)); lua_setglobal(L,"RX");
        lua_pushnumber(L, z8::fix32(room/8)); lua_setglobal(L,"RY");
        lua_pushnumber(L, z8::fix32(layer));  lua_setglobal(L,"LAYER");
        lua_pushnumber(L, z8::fix32(getenv("P8_CAMX")?atoi(getenv("P8_CAMX")):0)); lua_setglobal(L,"CAMX");
        lua_pushnumber(L, z8::fix32(getenv("P8_CAMY")?atoi(getenv("P8_CAMY")):0)); lua_setglobal(L,"CAMY");
        lua_pushnumber(L, z8::fix32(getenv("P8_FLASH")?atoi(getenv("P8_FLASH")):0)); lua_setglobal(L,"FLASH");
        host_frame(L);
        printf("maptest: room %d (map cell %d,%d, %s)  fb_hash=%08x\n",
               room, room%8*16, room/8*16, room<16 ? "__map__" : "SHARED rows 32-63", fb_hash());
        const char *dump=getenv("P8_DUMP");
        if(dump){ dump_frame(dump,0); printf("dumped to %s/frame_000.raw\n", dump); }
        lua_close(L); return 0;
#else
        printf("maptest needs assets/celeste_cart.h — run: python3 assets/gen_celeste_cart.py\n");
        return 1;
#endif
    }
    if(getenv("P8_SPRTEST")){
        const char *name;
        g_gfx = spr_test_sheet(&name);
        lua_State *L = host_open_cart(SPRTEST_CART);
        host_frame(L);
        printf("sprtest: sheet=%s  fb_hash=%08x\n", name, fb_hash());
        const char *dump = getenv("P8_DUMP");
        if(dump){ dump_frame(dump, 0); printf("dumped to %s/frame_000.raw\n", dump); }
        lua_close(L); return 0;
    }
    if(getenv("P8_SSPRTEST")){   /* HG-7 */
        const char *name;
        g_gfx = spr_test_sheet(&name);   /* the "F" test glyph is at sheet pixels (0,32), 8x8 */
        lua_State *L = host_open_cart(
            "function _draw()\n cls(1)\n"
            " spr(64, 4, 4)\n"                       /* reference: plain spr of the F (sprite 64 = sheet 0,32) */
            " sspr(0,32, 8,8, 20,4)\n"               /* sspr 1:1 -> must match spr exactly */
            " sspr(0,32, 8,8, 40,4, 16,16)\n"        /* sspr 2x (16x16) */
            " sspr(0,32, 8,8, 70,4, 16,16, true)\n"  /* sspr 2x, flip-x */
            " sspr(0,32, 8,8, 4,40, 24,16)\n"        /* stretched 3x wide, 2x tall */
            "end\nfunction _update() end");
        host_frame(L);
        printf("ssprtest: sheet=%s  fb_hash=%08x\n", name, fb_hash());
        const char *dump = getenv("P8_DUMP");
        if(dump){ dump_frame(dump, 0); printf("dumped to %s/frame_000.raw\n", dump); }
        lua_close(L); return 0;
    }
    lua_State *L=host_open();
    const char *dump = getenv("P8_DUMP");
    for(int f=0; f<10; f++){
        host_frame(L);
        printf("frame %d fb_hash=%08x\n", f, fb_hash());
        if(dump) dump_frame(dump, f);
    }
    if(dump) printf("dumped 10 frames to %s/ (render: tools/p8_png.py %s/frame_009.raw out.png)\n", dump, dump);
    lua_close(L); return 0;
}
#else              /* device: full pipeline — draw -> scale -> blit, measure FPS */
#include "board.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
/* On-panel Celeste demo (HG-4): build with -D CELESTE_DEMO=1 to draw real Celeste rooms via map()
 * instead of the trivial Gate-3 cart, cycling through __map__ and shared-region rooms so both can be
 * photographed on the glass. Needs the (gitignored) celeste_cart.h; degrades to the trivial cart if
 * absent. The default build is unchanged — this is off unless the flag is set. */
#if defined(CELESTE_DEMO) && __has_include("celeste_cart.h")
#include "celeste_cart.h"
#define HAVE_CELESTE_DEMO 1
#endif
#define OUT 256
#define OX  ((BOARD_LCD_H_RES-OUT)/2)
#define OY  ((BOARD_LCD_V_RES-OUT)/2)
extern "C" void app_main(void){
    ESP_ERROR_CHECK(board_lcd_init());
    board_lcd_fill(0);
    static uint16_t *scaled = (uint16_t*)heap_caps_malloc(OUT*OUT*2, MALLOC_CAP_DMA);
    if(!scaled){ printf("no DMA buffer\n"); return; }
    auto present=[&](void){
        for(int y=0;y<OUT;y++){ const uint8_t*s=&fb[(y>>1)*128]; uint16_t*d=&scaled[y*OUT];
            for(int x=0;x<OUT;x++) d[x]=pal565[s[x>>1]&15]; }
        board_lcd_blit(OX,OY,OUT,OUT,scaled);
    };

#ifdef HAVE_CELESTE_DEMO
    /* draw real Celeste rooms via map(), cycling every ~3s through __map__ and shared rooms */
    static uint8_t gff_copy[256]; memcpy(gff_copy, CELESTE_GFF, sizeof gff_copy);
    g_gfx = CELESTE_GFX; g_map = CELESTE_MAP; g_gff = gff_copy;
    lua_State *L = host_open_cart(
        "function _draw() cls(0) map(RX*16,RY*16,0,0,16,16) end\nfunction _update() end");
    lua_pushnumber(L, z8::fix32(0)); lua_setglobal(L,"LAYER");
    const int rooms[] = { 1, 5, 16, 24, 30 };   /* rooms 0-15 = __map__, 16-31 = shared region */
    const int NR = (int)(sizeof(rooms)/sizeof(rooms[0]));
    printf("\n=== HG-4 on-panel Celeste demo (map()) ===\n");
    int64_t t0=esp_timer_get_time(), shown=-1;
    while(true){
        int idx = (int)(((esp_timer_get_time()-t0)/3000000) % NR);
        if(idx!=shown){ shown=idx; int r=rooms[idx];
            lua_pushnumber(L,z8::fix32(r%8)); lua_setglobal(L,"RX");
            lua_pushnumber(L,z8::fix32(r/8)); lua_setglobal(L,"RY");
            printf("room %d (%s)  fb_hash=%08x\n", r, r<16?"__map__":"shared", (unsigned)fb_hash()); }
        host_frame(L); present();
        vTaskDelay(pdMS_TO_TICKS(40));
    }
#else
    /* Gate #3: the trivial cart end-to-end (the orientation/colour test pattern) */
    lua_State *L=host_open();
    printf("\n=== pico-e32 Gate #3 — trivial cart end-to-end ===\n");
    /* first 10 frames: print fb checksums to compare against the host build (correctness, no camera) */
    for(int f=0; f<10; f++){ host_frame(L); present(); printf("frame %d fb_hash=%08x\n", f, (unsigned)fb_hash()); }

    /* then run free + measure FPS */
    int frames=0; int64_t t0=esp_timer_get_time(), t=t0;
    while(true){
        host_frame(L); present();
        frames++;
        t=esp_timer_get_time();
        if(t-t0>=1000000LL){ printf("Gate #3: %.1f fps  fb_hash=%08x  (128x128 cart -> 256x256 blit)\n",
                                    frames/((t-t0)/1e6), (unsigned)fb_hash()); frames=0; t0=t; }
    }
#endif  /* HAVE_CELESTE_DEMO */
}
#endif  /* HOST_MAIN */
