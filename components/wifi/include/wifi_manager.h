/* wifi_manager — a tiny STA-mode WiFi front-end for the launcher.
 *
 * Scope: scan / connect / status / persist, in blocking calls suited to a modal menu (no async callbacks leak
 * out). One implementation over the esp_wifi API on both boards; only the backend differs (chosen in this
 * component's CMakeLists by IDF_TARGET): the ESP32-S3 drives its native radio, while the radio-less ESP32-P4
 * uses esp_wifi_remote to forward the same calls over esp-hosted (SDIO) to its ESP32-C6 companion. The launcher
 * still gates the WiFi menu on BOARD_HAS_WIFI (defined by a board once its WiFi path is proven).
 *
 * Credentials persist in NVS (namespace "wifi"); wifi_mgr_autoconnect() reconnects to them at boot. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_SSID_MAXLEN 32
#define WIFI_PASS_MAXLEN 64

typedef struct {
    char    ssid[WIFI_SSID_MAXLEN + 1];
    int8_t  rssi;      /* dBm (negative; closer to 0 = stronger) */
    bool    open;      /* true = no password (WIFI_AUTH_OPEN) */
} wifi_ap_t;

typedef struct {
    bool    connected;
    char    ssid[WIFI_SSID_MAXLEN + 1];   /* "" if not connected */
    char    ip[16];                       /* "a.b.c.d", or "" */
    int8_t  rssi;
} wifi_status_t;

/* Bring up NVS + netif + default event loop + esp_wifi in STA mode. Idempotent (safe to call more than once).
 * ESP_OK on success (on the P4 this also brings up the esp-hosted SDIO link to the C6). */
esp_err_t wifi_mgr_init(void);

/* Blocking active scan. Fills up to `max` APs, de-duplicated by SSID, strongest first (hidden/blank SSIDs
 * dropped). Returns the count written (>= 0), or < 0 on error. wifi_mgr_init() must have run. */
int wifi_mgr_scan(wifi_ap_t *out, int max);

/* Connect to ssid/pass (pass "" for an open network) and block up to timeout_ms for an IP. Returns ESP_OK once
 * an IP is acquired, else ESP_FAIL/timeout. Does NOT persist — call wifi_mgr_save() after a success to keep it. */
esp_err_t wifi_mgr_connect(const char *ssid, const char *pass, int timeout_ms);

/* Current link status; never blocks. */
void wifi_mgr_status(wifi_status_t *out);

/* Persist / load / clear the saved credentials (NVS namespace "wifi"). wifi_mgr_load returns true if present;
 * ssid/pass buffers must hold WIFI_SSID_MAXLEN+1 / WIFI_PASS_MAXLEN+1 bytes. */
esp_err_t wifi_mgr_save(const char *ssid, const char *pass);
bool      wifi_mgr_load(char *ssid, char *pass);
void      wifi_mgr_forget(void);

/* If saved credentials exist, connect to them (blocking up to timeout_ms). No-op (ESP_ERR_NOT_FOUND) if none.
 * Call once at boot after wifi_mgr_init(). */
esp_err_t wifi_mgr_autoconnect(int timeout_ms);

/* Disconnect (stays initialised, so a later connect is fast). */
void wifi_mgr_disconnect(void);

#ifdef __cplusplus
}
#endif
