/* ota_manager — see ota_manager.h for the safety ordering this file implements. */
#include "ota_manager.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
/* SHA-256 via PSA Crypto, not mbedtls/sha256.h: IDF v6.0.2 ships mbedtls 4.1, where the legacy low-level hash
 * API moved to mbedtls/private/ (TF-PSA-Crypto split) and is no longer public. PSA is initialised for us during
 * system init (components/mbedtls/port/esp_psa_crypto_init.c), so there is no psa_crypto_init() call here. */
#include "psa/crypto.h"

static const char *TAG = "ota";

#define CHUNK 4096            /* one flash write per read; small enough to keep the UI responsive */
#define MANIFEST_MAXLEN 1024  /* a manifest is ~200 bytes; this is a sanity bound, not a target */

/* TLS policy. Shipped builds validate against the IDF certificate bundle (public CAs, nothing embedded).
 * -D OTA_INSECURE=1 disables verification for a self-signed bench server — it is deliberately noisy and
 * deliberately not the default, so it cannot become the shipped behaviour by accident. */
static void ota_apply_tls(esp_http_client_config_t *cfg) {
#if defined(OTA_INSECURE) && OTA_INSECURE
    cfg->skip_cert_common_name_check = true;
    cfg->crt_bundle_attach = NULL;
    ESP_LOGW(TAG, "OTA_INSECURE build: TLS certificate NOT verified — bench testing only, never ship this");
#else
    cfg->crt_bundle_attach = esp_crt_bundle_attach;
#endif
}

void ota_current_version(char *buf, size_t len) {
    if (!buf || !len) return;
    const esp_app_desc_t *d = esp_app_get_description();
    snprintf(buf, len, "%s", d ? d->version : "?");
}

/* --- manifest ------------------------------------------------------------------------------------------- */

/* Pull "key":"value" / "key":number out of a flat JSON object. A hand-rolled scan rather than a JSON parser:
 * the manifest is a flat object we define ourselves, and this keeps the component free of a parser dependency
 * for ~30 lines. It is strict about the key being a real key (quoted, followed by a colon). */
static bool json_str(const char *js, const char *key, char *out, size_t out_len) {
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(js, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p) return false;
    while (*p && (*p == ':' || *p == ' ' || *p == '\t')) p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_len) out[i++] = *p++;
    out[i] = '\0';
    return *p == '"';           /* a truncated value is a rejected manifest, not a silent short string */
}

static bool json_num(const char *js, const char *key, size_t *out) {
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(js, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p < '0' || *p > '9') return false;
    size_t v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10 + (size_t)(*p++ - '0');
    *out = v;
    return true;
}

/* Open `c` and follow redirects, returning the final status code (<0 on transport error).
 *
 * esp_http_client_open() + fetch_headers() does NOT follow 3xx — that lives in esp_http_client_perform(),
 * which we cannot use because the body has to be streamed into flash a chunk at a time. Real hosting
 * redirects constantly (GitHub releases hand off to objects.githubusercontent.com; most CDNs and bare-domain
 * rules do the same), so without this the first non-bench endpoint fails as a bare "not found". */
#define MAX_REDIRECTS 4
static int open_following_redirects(esp_http_client_handle_t c) {
    for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
        esp_err_t err = esp_http_client_open(c, 0);
        if (err != ESP_OK) { ESP_LOGE(TAG, "open: %s", esp_err_to_name(err)); return -1; }
        esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        if (status != 301 && status != 302 && status != 303 && status != 307 && status != 308) return status;
        if (hop == MAX_REDIRECTS) { ESP_LOGE(TAG, "too many redirects (%d)", hop); return status; }
        esp_http_client_set_redirection(c);      /* adopts the Location header */
        esp_http_client_close(c);
        ESP_LOGI(TAG, "following redirect (%d), hop %d", status, hop + 1);
    }
    return -1;
}

esp_err_t ota_check(const char *manifest_url, ota_release_t *out, int timeout_ms) {
    if (!manifest_url || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);

    esp_http_client_config_t cfg = {
        .url = manifest_url,
        .timeout_ms = timeout_ms,
        .keep_alive_enable = false,
    };
    ota_apply_tls(&cfg);

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_FAIL;

    int status = open_following_redirects(c);
    if (status < 0) { esp_http_client_cleanup(c); return ESP_FAIL; }
    if (status != 200) {
        ESP_LOGE(TAG, "manifest HTTP %d", status);
        esp_http_client_close(c); esp_http_client_cleanup(c);
        return ESP_ERR_NOT_FOUND;
    }

    char body[MANIFEST_MAXLEN + 1];
    int n = esp_http_client_read_response(c, body, MANIFEST_MAXLEN);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (n <= 0) return ESP_ERR_INVALID_RESPONSE;
    body[n] = '\0';

    if (!json_str(body, "target",  out->target,  sizeof out->target)  ||
        !json_str(body, "version", out->version, sizeof out->version) ||
        !json_str(body, "url",     out->url,     sizeof out->url)     ||
        !json_str(body, "sha256",  out->sha256,  sizeof out->sha256)  ||
        !json_num(body, "size",    &out->size)) {
        ESP_LOGE(TAG, "manifest missing a required field");
        return ESP_ERR_INVALID_RESPONSE;
    }
    json_str(body, "build", out->build, sizeof out->build);   /* display only */

    if (strlen(out->sha256) != 64 || out->size == 0) {
        ESP_LOGE(TAG, "manifest sha256/size implausible");
        return ESP_ERR_INVALID_RESPONSE;
    }
    /* Wrong-chip images are refused here rather than at the next boot. */
    if (strcmp(out->target, CONFIG_IDF_TARGET) != 0) {
        ESP_LOGE(TAG, "manifest targets '%s' but this is '%s' — refusing", out->target, CONFIG_IDF_TARGET);
        return ESP_ERR_INVALID_VERSION;
    }
    ESP_LOGI(TAG, "manifest: %s for %s (%u bytes)", out->version, out->target, (unsigned)out->size);
    return ESP_OK;
}

bool ota_is_newer(const ota_release_t *r) {
    if (!r || !r->version[0]) return false;
    char cur[OTA_VERSION_MAXLEN + 1];
    ota_current_version(cur, sizeof cur);
    return strcmp(cur, r->version) != 0;
}

/* --- apply ---------------------------------------------------------------------------------------------- */

static void hex32(const unsigned char *in, char *out /* >=65 */) {
    static const char *H = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[i * 2] = H[in[i] >> 4]; out[i * 2 + 1] = H[in[i] & 0xf]; }
    out[64] = '\0';
}

esp_err_t ota_apply(const ota_release_t *r, ota_progress_fn cb, void *user) {
    if (!r || !r->url[0] || r->size == 0) return ESP_ERR_INVALID_ARG;

    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
    if (!slot) { ESP_LOGE(TAG, "no inactive OTA slot"); return ESP_ERR_NOT_FOUND; }
    if (r->size > slot->size) {
        ESP_LOGE(TAG, "image %u > slot %u", (unsigned)r->size, (unsigned)slot->size);
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_LOGI(TAG, "target slot '%s' @0x%lx (%u KB)", slot->label, (unsigned long)slot->address,
             (unsigned)(slot->size / 1024));

    esp_http_client_config_t cfg = { .url = r->url, .timeout_ms = 15000, .keep_alive_enable = false };
    ota_apply_tls(&cfg);
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_FAIL;

    int status = open_following_redirects(c);
    if (status < 0) { esp_http_client_cleanup(c); return ESP_FAIL; }
    if (status != 200) {
        ESP_LOGE(TAG, "image HTTP %d", status);
        esp_http_client_close(c); esp_http_client_cleanup(c);
        return ESP_ERR_NOT_FOUND;
    }

    esp_ota_handle_t h = 0;
    esp_err_t err = esp_ota_begin(slot, r->size, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        esp_http_client_close(c); esp_http_client_cleanup(c);
        return err;
    }

    psa_hash_operation_t sha = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&sha, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        esp_ota_abort(h); esp_http_client_close(c); esp_http_client_cleanup(c);
        ESP_LOGE(TAG, "psa_hash_setup failed — refusing to flash an unverifiable image");
        return ESP_FAIL;
    }

    static char buf[CHUNK];      /* static: 4 KB does not belong on a UI task stack */
    size_t done = 0;
    bool cancelled = false;

    while (done < r->size) {
        int n = esp_http_client_read(c, buf, sizeof buf);
        if (n < 0) { err = ESP_ERR_INVALID_RESPONSE; break; }
        if (n == 0) break;                                  /* server closed early -> size check below catches it */
        if (done + (size_t)n > r->size) { err = ESP_ERR_INVALID_SIZE; break; }  /* longer than advertised */

        psa_hash_update(&sha, (const unsigned char *)buf, (size_t)n);
        err = esp_ota_write(h, buf, (size_t)n);
        if (err != ESP_OK) { ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err)); break; }
        done += (size_t)n;

        if (cb && !cb(done, r->size, user)) { cancelled = true; break; }
    }

    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    unsigned char digest[32];
    size_t digest_len = 0;
    psa_status_t hs = psa_hash_finish(&sha, digest, sizeof digest, &digest_len);
    if (hs != PSA_SUCCESS || digest_len != sizeof digest) {
        esp_ota_abort(h);
        ESP_LOGE(TAG, "psa_hash_finish failed (%d) — boot slot untouched", (int)hs);
        return ESP_FAIL;
    }

    if (cancelled)      { esp_ota_abort(h); ESP_LOGW(TAG, "cancelled at %u/%u bytes — boot slot untouched",
                                                     (unsigned)done, (unsigned)r->size); return ESP_ERR_INVALID_STATE; }
    if (err != ESP_OK)  { esp_ota_abort(h); ESP_LOGE(TAG, "download failed (%s) — boot slot untouched",
                                                    esp_err_to_name(err)); return err; }
    if (done != r->size) {
        esp_ota_abort(h);
        ESP_LOGE(TAG, "short image: %u of %u bytes — boot slot untouched", (unsigned)done, (unsigned)r->size);
        return ESP_ERR_INVALID_SIZE;
    }

    char got[65];
    hex32(digest, got);
    if (strcasecmp(got, r->sha256) != 0) {
        esp_ota_abort(h);
        ESP_LOGE(TAG, "SHA-256 mismatch — boot slot untouched");
        ESP_LOGE(TAG, "  manifest %s", r->sha256);
        ESP_LOGE(TAG, "  received %s", got);
        return ESP_ERR_INVALID_CRC;
    }

    /* Only now is it safe to finalise: bytes are exactly what the manifest promised. esp_ota_end() additionally
     * validates the image structure, and set_boot_partition arms the pending-verify boot. */
    err = esp_ota_end(h);
    if (err != ESP_OK) { ESP_LOGE(TAG, "esp_ota_end: %s — boot slot untouched", esp_err_to_name(err)); return err; }

    err = esp_ota_set_boot_partition(slot);
    if (err != ESP_OK) { ESP_LOGE(TAG, "set_boot_partition: %s", esp_err_to_name(err)); return err; }

    ESP_LOGI(TAG, "verified %u bytes, boot slot -> '%s'; reboot to apply", (unsigned)done, slot->label);
    return ESP_OK;
}

/* --- rollback ------------------------------------------------------------------------------------------- */

bool ota_awaiting_verify(void) {
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (!run || esp_ota_get_state_partition(run, &st) != ESP_OK) return false;
    return st == ESP_OTA_IMG_PENDING_VERIFY;
}

void ota_mark_valid(void) {
    if (!ota_awaiting_verify()) return;
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "new image confirmed good (%s) — rollback cancelled", esp_err_to_name(err));
}
