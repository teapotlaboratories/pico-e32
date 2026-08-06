/* wifi_manager — a tiny STA-mode WiFi front-end for the launcher.
 *
 * Scope: acquire/release the radio, then scan / connect / status / persist, in blocking calls suited to a modal
 * menu (no async callbacks leak out). The radio is off by default and only up while someone holds it — see the
 * power-policy block below.
 *
 * One implementation over the esp_wifi API on both boards; only the backend differs (chosen in this
 * component's CMakeLists by IDF_TARGET): the ESP32-S3 drives its native radio, while the radio-less ESP32-P4
 * uses esp_wifi_remote to forward the same calls over esp-hosted (SDIO) to its ESP32-C6 companion. The launcher
 * still gates the WiFi menu on BOARD_HAS_WIFI (defined by a board once its WiFi path is proven).
 *
 * Credentials persist in NVS (namespace "wifi"); wifi_mgr_autoconnect() rejoins them once the radio is up. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Protocol maxima, not arbitrary sizes: an 802.11 SSID is up to 32 bytes (and is NOT required to be
 * NUL-terminated on the wire), and a WPA/WPA2 passphrase is at most 63 characters. Buffers here are
 * MAXLEN+1 so they can always hold a C string; esp_wifi's own wifi_config_t fields are exactly 32 / 64. */
#define WIFI_SSID_MAXLEN 32
#define WIFI_PASS_MAXLEN 63

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

/* ---- Power policy: the radio is OFF unless something is actively using it (WC-5) ----------------------------
 * Nothing brings WiFi up at boot. A caller that needs the network acquires it, does its work, and releases it;
 * when the last reference goes the stack is torn down completely — esp_wifi deinit'd, the STA netif destroyed,
 * and on the P4 the esp-hosted link to the ESP32-C6 dropped so the companion radio stops drawing power and its
 * priority-23 tasks go away. This costs a bring-up each time it is needed (~2.3 s on the P4, ~0.15 s on the S3)
 * and buys back boot time, battery, and CPU that belongs to the game.
 *
 * Acquire/release are reference-counted so overlapping users compose (e.g. an OTA check while the WiFi settings
 * screen is open). Every acquire MUST be matched by exactly one release. */

/* Bring the radio up (if it isn't already) and take a reference. Blocking: on the P4 this includes the C6
 * handshake. ESP_OK once the stack is up — that means "radio ready", NOT "connected"; use wifi_mgr_autoconnect()
 * or wifi_mgr_connect() after this. On failure no reference is held. */
esp_err_t wifi_mgr_acquire(void);

/* Drop a reference. When the last one goes, the radio is fully torn down. */
void wifi_mgr_release(void);

/* Force the radio down NOW, whatever the refcount — used when launching a cart: a game never needs the network
 * and must not share the CPU (or, on the P4, the SDIO bus) with it. Safe to call when already down. */
void wifi_mgr_shutdown(void);

/* Is the radio currently up? (Up != connected — see wifi_mgr_status for that.) */
bool wifi_mgr_is_up(void);

/* Blocking active scan. Fills up to `max` APs, de-duplicated by SSID, strongest first (hidden/blank SSIDs
 * dropped). Returns the count written (>= 0), or < 0 on error. Requires a held wifi_mgr_acquire(). */
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
 * Call after wifi_mgr_acquire(); this is how a download/OTA gets onto the saved network. */
esp_err_t wifi_mgr_autoconnect(int timeout_ms);

/* Drop the association but keep the radio up (the caller still holds its reference). Also stands the
 * auto-reconnect keepalive down, so it stays disconnected until told otherwise. */
void wifi_mgr_disconnect(void);

#ifdef __cplusplus
}
#endif
