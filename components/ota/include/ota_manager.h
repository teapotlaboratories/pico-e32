/* ota_manager — firmware update over WiFi, for the launcher's SYSTEM UPDATE screen (WC-4a).
 *
 * Blocking calls suited to a modal screen, same shape as wifi_manager. The caller owns the radio: acquire it
 * (wifi_mgr_acquire) before ota_check/ota_apply and release after. This component never touches the radio, the
 * SD card, or the UI.
 *
 * SAFETY — this is firmware that replaces firmware, so the ordering matters and is deliberate:
 *   1. the image is streamed into the INACTIVE slot; the running one is never written;
 *   2. its SHA-256 is computed WHILE streaming and compared against the manifest BEFORE the boot slot moves —
 *      a truncated, corrupted or swapped image is rejected with the boot slot untouched;
 *   3. only then esp_ota_end() (which validates the image structure) and esp_ota_set_boot_partition();
 *   4. the new image boots as "pending verify" and must reach the launcher and call ota_mark_valid(); if it
 *      does not, the bootloader rolls back to the previous slot (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE).
 * An aborted or failed download therefore leaves a fully bootable device.
 *
 * Why not esp_https_ota: its esp_https_ota_finish() calls esp_ota_end() AND esp_ota_set_boot_partition() in one
 * step (IDF components/esp_https_ota/src/esp_https_ota.c), leaving nowhere to verify the manifest hash before
 * the switch. Verifying then reverting is worse than not switching. esp_http_client + esp_ota_ops gives the
 * ordering above, and hashes in the same pass rather than re-reading the partition. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_VERSION_MAXLEN 47
#define OTA_URL_MAXLEN    255

/* One release, as described by the manifest:
 *   {"target":"esp32s3","version":"...","build":"...","url":"https://...","sha256":"<64 hex>","size":<bytes>}
 *
 * `target` is required and must match the running chip. Two boards share one endpoint here, and without it an
 * S3 would happily download and flash a P4 image: the bootloader does reject a wrong chip ID, but only at the
 * next boot, so the board would burn the transfer, switch slots, fail to boot and roll back. Refusing up front
 * is a clear message instead of a scary reboot. */
typedef struct {
    char   target[16];                /* IDF target the image is built for, e.g. "esp32p4" */
    char   version[OTA_VERSION_MAXLEN + 1];
    char   build[32];                 /* ISO-8601 build stamp, display only */
    char   url[OTA_URL_MAXLEN + 1];
    char   sha256[65];                /* lowercase hex of the .bin */
    size_t size;                      /* bytes; must match exactly */
} ota_release_t;

/* The running firmware's version string (from esp_app_desc, i.e. the build's git describe). */
void ota_current_version(char *buf, size_t len);

/* Fetch + parse the manifest at `url`. ESP_OK means "parsed", NOT "newer" — ask ota_is_newer().
 * Requires a held wifi_mgr_acquire(). */
esp_err_t ota_check(const char *manifest_url, ota_release_t *out, int timeout_ms);

/* Does this release differ from what is running? Deliberately an inequality, not an ordering: version strings
 * here are git-describe output, which does not order meaningfully, and pretending otherwise would silently
 * refuse legitimate rebuilds and downgrades. */
bool ota_is_newer(const ota_release_t *r);

/* Progress callback; return false to cancel (the download aborts and the boot slot is left untouched). */
typedef bool (*ota_progress_fn)(size_t done, size_t total, void *user);

/* Download `r` into the inactive slot, verify, and point the bootloader at it. Does NOT reboot — the caller
 * decides when. ESP_ERR_INVALID_CRC = hash/size mismatch (nothing switched); ESP_ERR_INVALID_STATE = cancelled.
 * Requires a held wifi_mgr_acquire(). */
esp_err_t ota_apply(const ota_release_t *r, ota_progress_fn cb, void *user);

/* True when the running image is on trial after an update and has not yet been confirmed. */
bool ota_awaiting_verify(void);

/* Confirm the running image, cancelling the pending rollback. Call once the launcher is up — that is the
 * evidence the image is good. Harmless if no verify is pending. */
void ota_mark_valid(void);

#ifdef __cplusplus
}
#endif
