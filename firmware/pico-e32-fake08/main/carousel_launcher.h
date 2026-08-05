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
/* Dev "screenshot over serial" (both boards): capture board_lcd_framebuffer and stream it framed over the
 * console for a camera-free host-side PNG. The transport differs by console:
 *   - P4 (USB-JTAG): zlib-compressed `FB FB FB FB 'S' 'H' 'T' 'Z' w(2) h(2) clen(4)` + clen bytes (the bulk
 *     endpoint stalls on big raw writes, so it compresses first). Host: tools/fb_menu_shot.py.
 *   - S3 (UART): raw `FB FB FB FB 'S' 'H' 'O' 'T' w(2) h(2)` + w*h*2 RGB565 (its GRAM panel can't be read
 *     back, so board_lcd_framebuffer serves a per-blit shadow buffer). Host: tools/fb_menu_shot_raw.py.
 * See carousel_launcher.cpp for the two gotchas (console LF->CRLF; watchdog backtrace injection). */
void carousel_fb_dump(void);
#endif
