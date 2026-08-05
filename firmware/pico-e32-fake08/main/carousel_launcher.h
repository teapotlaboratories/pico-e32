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
/* Dev "screenshot over serial" (P4 only — needs board_lcd_framebuffer): deflate the live DPI framebuffer and
 * stream it framed as `FB FB FB FB 'S' 'H' 'T' 'Z' w(2) h(2) clen(4)` + clen zlib bytes over the USB-JTAG
 * console, for a camera-free host-side PNG. Host: tools/fb_menu_shot.py. (The S3's GRAM panel can't be read
 * back cleanly, so it has no board_lcd_framebuffer and FB_DUMP is not built there — use the camera.) */
void carousel_fb_dump(void);
#endif
