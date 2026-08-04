#pragma once
#include <string>

class Host;

/* Native cover-art carousel launcher (LAUNCHER build). Draws a 3-up carousel of decoded .p8.png cover art
 * on the panel via board_lcd_blit, navigates the SD folder tree with the touch deck, and returns the
 * absolute path of the cart the user chose to launch (or "" if they exited without choosing — the caller
 * then falls back to the text browser / flash cart). Assumes board_lcd_init has run; it inits touch itself.
 * `host` supplies listcarts/listdirs/setCartDirectory over the mounted SD; start_dir is the SD mount point
 * (the navigation floor — the user can't ascend above it). */
std::string carousel_launcher_run(Host *host, const std::string &start_dir);

#ifdef FB_DUMP
/* Dev screenshot: deflate the full-res RGB565 framebuffer with miniz and stream it framed as
 * `FB FB FB FB 'S' 'H' 'T' 'Z' w(2) h(2) complen(4)` + complen zlib bytes. Compression keeps the blob far
 * under the P4 USB-Serial-JTAG bulk-transfer stall floor (a raw 768 KB write dies at a random point).
 * Host side: tools/fb_shot.py. Used by carousel_fb_dump() and main.cpp's FB_DUMP loop. */
void fb_dump_compressed(const unsigned short *fb, int w, int h);
#endif
