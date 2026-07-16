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

static inline void px(int x,int y,int c){ if((unsigned)x<128 && (unsigned)y<128) fb[y*128+x]=c&15; }
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

static lua_State *host_open(void){
    for(int i=0;i<16;i++) pal565[i]=((PAL888[i][0]&0xF8)<<8)|((PAL888[i][1]&0xFC)<<3)|(PAL888[i][2]>>3);
    /* swap bytes for the 16-bit i80 bus (matches the board driver's SWAP_COLOR_BYTES) */
    for(int i=0;i<16;i++) pal565[i]=(uint16_t)((pal565[i]>>8)|(pal565[i]<<8));
    lua_State *L=luaL_newstate(); luaL_openlibs(L);
    reg(L,"cls",l_cls); reg(L,"color",l_color); reg(L,"pset",l_pset);
    reg(L,"rectfill",l_rectfill); reg(L,"rect",l_rect);
    reg(L,"circfill",l_circfill); reg(L,"circ",l_circ); reg(L,"line",l_line);
    /* stubs the trivial cart doesn't need but a real cart might touch */
    const char *nops[]={"spr","sspr","map","print","camera","pal","palt","clip","fillp","sfx","music",0};
    for(int i=0;nops[i];i++) reg(L,nops[i],l_nop);
    if(luaL_dostring(L,CART)){ printf("cart error: %s\n", lua_tostring(L,-1)); }
    lua_getglobal(L,"_init"); if(lua_isfunction(L,-1)) lua_pcall(L,0,0,0); else lua_pop(L,1);
    return L;
}
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
int main(void){
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
#define OUT 256
#define OX  ((BOARD_LCD_H_RES-OUT)/2)
#define OY  ((BOARD_LCD_V_RES-OUT)/2)
extern "C" void app_main(void){
    ESP_ERROR_CHECK(board_lcd_init());
    board_lcd_fill(0);
    /* selftest removed: the cart itself is now the test pattern */
    lua_State *L=host_open();
    static uint16_t *scaled;
    scaled=(uint16_t*)heap_caps_malloc(OUT*OUT*2, MALLOC_CAP_DMA);
    if(!scaled){ printf("no DMA buffer\n"); return; }

    auto present=[&](void){
        for(int y=0;y<OUT;y++){ const uint8_t*s=&fb[(y>>1)*128]; uint16_t*d=&scaled[y*OUT];
            for(int x=0;x<OUT;x++) d[x]=pal565[s[x>>1]&15]; }
        board_lcd_blit(OX,OY,OUT,OUT,scaled);
    };

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
}
#endif
