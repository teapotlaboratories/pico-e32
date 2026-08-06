/* wifi_manager — one esp_wifi STA front-end over both backends (see wifi_manager.h): native esp_wifi on the S3,
 * esp_wifi_remote -> esp-hosted -> ESP32-C6 on the P4. Backend chosen by IDF_TARGET in this component's CMake. */
#include "wifi_manager.h"

#include <string.h>
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"   /* esp_get_free_heap_size — the up/down logs double as teardown evidence */
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#if CONFIG_IDF_TARGET_ESP32P4
#include "esp_hosted.h"   /* P4: bring the C6 SDIO link up HERE, not in esp-hosted's boot constructor — see below */
#endif

static const char *TAG = "wifi";
#define NVS_NS   "wifi"
#define KEY_SSID "ssid"
#define KEY_PASS "pass"

#define BIT_GOT_IP  BIT0
#define BIT_FAILED  BIT1
#define MAX_RETRY   3
#define RETRY_DELAY_US (10 * 1000 * 1000)   /* steady-state reconnect probe: slow enough to be free */

static bool               s_inited = false;
static esp_netif_t       *s_netif  = NULL;
static EventGroupHandle_t s_events = NULL;
static SemaphoreHandle_t  s_lock   = NULL;   /* serialises scan/connect (boot autoconnect vs the menu) */
static int                s_retry  = 0;
static bool               s_connecting = false;   /* true only while wifi_mgr_connect() is waiting */
static bool               s_keepalive  = false;   /* true once a link came up: keep it up from then on */
static esp_timer_handle_t s_retry_timer = NULL;
static bool               s_handlers = false;    /* event handlers registered once, even across a failed init */
static esp_event_handler_instance_t s_h_wifi = NULL, s_h_ip = NULL;   /* kept so teardown can unregister them */
static int                s_refs     = 0;        /* live wifi_mgr_acquire() references; 0 => radio torn down */
static volatile bool      s_initing  = false;    /* a bring-up is in flight (see wifi_mgr_bringup) */
static portMUX_TYPE       s_init_mux = portMUX_INITIALIZER_UNLOCKED;   /* statically init'd: usable before init */

static void retry_timer_cb(void *arg) { (void)arg; esp_wifi_connect(); }

/* Disconnects mean two different things, so they get two different policies:
 *   - during a wifi_mgr_connect() call: retry a bounded number of times, then report failure, so the menu
 *     gets an answer instead of hanging forever on a wrong password or an absent AP;
 *   - after a link was actually established: keep retrying indefinitely on a slow timer. An AP reboot or a
 *     walk out of range must not leave the handheld permanently offline — which is exactly what a shared
 *     bounded counter used to do, since it only reset on GOT_IP. */
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_connecting) {
            if (s_retry < MAX_RETRY) { s_retry++; esp_wifi_connect(); }
            else                     { xEventGroupSetBits(s_events, BIT_FAILED); }
        } else if (s_keepalive && s_retry_timer) {
            esp_timer_start_once(s_retry_timer, RETRY_DELAY_US);   /* INVALID_STATE = already armed; fine */
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry     = 0;
        s_keepalive = true;      /* from here on, a drop is something to recover from, not to give up on */
        xEventGroupSetBits(s_events, BIT_GOT_IP);
    }
}

static esp_err_t wifi_mgr_init_once(void) {
#if CONFIG_IDF_TARGET_ESP32P4
    /* Bring up the esp-hosted SDIO link to the C6 HERE — on demand, from whoever acquired the radio — and never
     * from esp-hosted's own boot constructor. That constructor (ESP_ERROR_CHECK(esp_hosted_init()) at
     * __attribute__((constructor)) time) stalls the C6 handshake in the full firmware: it runs before app_main,
     * racing the other C++ global constructors, and the board never reaches app_main. It is disabled at build
     * time from the project CMakeLists; see the P4 board doc. Verified that this pairs cleanly with the
     * esp_hosted_deinit() in teardown — repeated up/down cycles re-init fine and don't leak. */
    esp_err_t he = esp_hosted_init();
    if (he != ESP_OK && he != ESP_ERR_INVALID_STATE) { ESP_LOGE(TAG, "esp_hosted_init -> %s", esp_err_to_name(he)); return he; }
    /* esp_hosted_init() only starts the transport tasks; this does the blocking C6 handshake (reset via GPIO54,
     * SDIO card init, identify slave) so the link is actually up before esp_wifi_init runs against it. */
    he = esp_hosted_connect_to_slave();
    if (he != ESP_OK) { ESP_LOGE(TAG, "esp_hosted_connect_to_slave -> %s", esp_err_to_name(he)); return he; }
#endif

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    ESP_ERROR_CHECK(esp_netif_init());
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;   /* a shared loop may already exist */
    /* Guard the one-shot allocations: an earlier call that failed *after* this point leaves s_inited false,
     * so a retry would otherwise create a second netif and register the handlers twice. */
    if (!s_netif) s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));   /* we own persistence via NVS ourselves */

    if (!s_events) s_events = xEventGroupCreate();
    if (!s_lock)   s_lock   = xSemaphoreCreateMutex();
    if (!s_retry_timer) {
        const esp_timer_create_args_t targs = { .callback = retry_timer_cb, .name = "wifi_retry" };
        esp_timer_create(&targs, &s_retry_timer);   /* NULL on failure -> we simply don't auto-retry */
    }
    if (!s_handlers) {
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, &s_h_wifi));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, &s_h_ip));
        s_handlers = true;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_inited = true;
    ESP_LOGI(TAG, "radio up (free heap %u)", (unsigned)esp_get_free_heap_size());
    return ESP_OK;
}

/* Idempotent AND thread-safe: two callers can want the radio at once (the WiFi screen and, later, an OTA or
 * download). Without this gate both would sail past the s_inited check and run the whole bring-up twice (two
 * netifs, a second esp_wifi_init). The loser simply waits for the winner and reports the same result. */
static esp_err_t wifi_mgr_bringup(void) {
    if (s_inited) return ESP_OK;

    bool mine = false;
    portENTER_CRITICAL(&s_init_mux);
    if (!s_inited && !s_initing) { s_initing = true; mine = true; }
    portEXIT_CRITICAL(&s_init_mux);

    if (!mine) {                                   /* another task is mid-bring-up — wait it out */
        while (s_initing) vTaskDelay(pdMS_TO_TICKS(50));
        return s_inited ? ESP_OK : ESP_FAIL;
    }

    esp_err_t err = wifi_mgr_init_once();
    s_initing = false;
    return err;
}

/* Full teardown — the whole point of the on-demand model (WC-5). Not a "stop": the stack is deinitialised and
 * the netif destroyed, and on the P4 the esp-hosted link is dropped so the C6 stops drawing power and its
 * priority-23 tasks disappear. Everything here must leave the module able to bring up again from scratch, so
 * every flag guarding a one-shot allocation is reset. The long-lived primitives (mutex, event group, timer) are
 * deliberately NOT destroyed — they are small, and keeping them removes a whole class of use-after-free race
 * with a concurrent scan/connect. */
static void wifi_mgr_teardown(void) {
    if (!s_inited) return;

    xSemaphoreTake(s_lock, portMAX_DELAY);   /* never tear down under a live scan/connect */

    s_keepalive  = false;                    /* stop the reconnector before the handlers go away */
    s_connecting = false;
    if (s_retry_timer) esp_timer_stop(s_retry_timer);

    esp_wifi_disconnect();
    esp_wifi_stop();

    if (s_handlers) {                        /* unregister BEFORE deinit so no event lands on a dead stack */
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_h_wifi);
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_h_ip);
        s_h_wifi = s_h_ip = NULL;
        s_handlers = false;
    }

    esp_wifi_deinit();

    if (s_netif) { esp_netif_destroy_default_wifi(s_netif); s_netif = NULL; }

#if CONFIG_IDF_TARGET_ESP32P4
    /* Drop the SDIO link to the C6. This is what actually powers the companion radio down and removes the
     * esp-hosted tasks; without it the P4 keeps a whole second chip awake for nothing. */
    esp_hosted_deinit();
#endif

    s_inited = false;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "radio down (free heap %u)", (unsigned)esp_get_free_heap_size());
}

esp_err_t wifi_mgr_acquire(void) {
    portENTER_CRITICAL(&s_init_mux);
    s_refs++;
    portEXIT_CRITICAL(&s_init_mux);

    esp_err_t err = wifi_mgr_bringup();
    if (err != ESP_OK) {                     /* failed acquires hold no reference */
        portENTER_CRITICAL(&s_init_mux);
        if (s_refs > 0) s_refs--;
        portEXIT_CRITICAL(&s_init_mux);
    }
    return err;
}

void wifi_mgr_release(void) {
    bool last = false;
    portENTER_CRITICAL(&s_init_mux);
    if (s_refs > 0 && --s_refs == 0) last = true;
    portEXIT_CRITICAL(&s_init_mux);
    if (last) wifi_mgr_teardown();
}

void wifi_mgr_shutdown(void) {
    portENTER_CRITICAL(&s_init_mux);
    s_refs = 0;
    portEXIT_CRITICAL(&s_init_mux);
    wifi_mgr_teardown();
}

bool wifi_mgr_is_up(void) { return s_inited; }

int wifi_mgr_scan(wifi_ap_t *out, int max) {
    if (!s_inited || !out || max <= 0) return -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    int count = -1;
    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {           /* blocking */
        uint16_t n = 0;
        esp_wifi_scan_get_ap_num(&n);
        wifi_ap_record_t *recs = n ? (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * n) : NULL;
        if (n == 0) {
            count = 0;
        } else if (recs) {
            esp_wifi_scan_get_ap_records(&n, recs);           /* strongest-first already */
            count = 0;
            for (uint16_t i = 0; i < n && count < max; i++) {
                const char *ssid = (const char *)recs[i].ssid;
                if (ssid[0] == '\0') continue;                /* hidden / blank */
                bool dup = false;                             /* keep only the strongest of each SSID */
                for (int j = 0; j < count; j++)
                    if (strncmp(out[j].ssid, ssid, WIFI_SSID_MAXLEN) == 0) { dup = true; break; }
                if (dup) continue;
                strlcpy(out[count].ssid, ssid, sizeof(out[count].ssid));
                out[count].rssi = recs[i].rssi;
                out[count].open = (recs[i].authmode == WIFI_AUTH_OPEN);
                count++;
            }
        }
        free(recs);
        esp_wifi_clear_ap_list();
    }
    xSemaphoreGive(s_lock);
    return count;
}

esp_err_t wifi_mgr_connect(const char *ssid, const char *pass, int timeout_ms) {
    if (!s_inited || !ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t ret;
    wifi_config_t wc = { 0 };
    /* memcpy, not strlcpy: an SSID may be exactly 32 bytes and is not NUL-terminated in wifi_config_t, so
     * strlcpy's mandatory terminator would silently drop the 32nd character and the join would just fail.
     * wc is zero-initialised, so a shorter SSID is already padded. Same reasoning caps the passphrase at its
     * protocol maximum (63) rather than the field size (64). */
    memcpy(wc.sta.ssid, ssid, strnlen(ssid, sizeof(wc.sta.ssid)));
    if (pass) memcpy(wc.sta.password, pass, strnlen(pass, sizeof(wc.sta.password) - 1));
    /* leave threshold.authmode at 0 (OPEN) so it accepts whatever the AP offers (open or WPA/WPA2/WPA3) */
    if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK) {
        ret = ESP_FAIL;
    } else {
        /* Stand down the steady-state reconnector for the duration: this attempt owns the radio, and a failed
         * *explicit* connect should report failure rather than quietly retry the new credentials forever. */
        s_keepalive = false;
        if (s_retry_timer) esp_timer_stop(s_retry_timer);
        esp_wifi_disconnect();
        xEventGroupClearBits(s_events, BIT_GOT_IP | BIT_FAILED);
        s_retry      = 0;
        s_connecting = true;
        ret = esp_wifi_connect();
        if (ret == ESP_OK) {
            EventBits_t bits = xEventGroupWaitBits(s_events, BIT_GOT_IP | BIT_FAILED,
                                                   pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
            s_connecting = false;             /* modal attempt over; a later drop is the keepalive's business */
            if (bits & BIT_GOT_IP) {
                ret = ESP_OK;                 /* GOT_IP already armed s_keepalive */
            } else {
                esp_wifi_disconnect();        /* give up cleanly on timeout/fail */
                ret = (bits & BIT_FAILED) ? ESP_FAIL : ESP_ERR_TIMEOUT;
            }
        } else {
            s_connecting = false;
        }
    }
    xSemaphoreGive(s_lock);
    return ret;
}

void wifi_mgr_status(wifi_status_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!s_inited) return;

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return;   /* not associated */

    esp_netif_ip_info_t ip;
    if (s_netif && esp_netif_get_ip_info(s_netif, &ip) == ESP_OK && ip.ip.addr != 0) {
        out->connected = true;
        strlcpy(out->ssid, (const char *)ap.ssid, sizeof(out->ssid));
        out->rssi = ap.rssi;
        snprintf(out->ip, sizeof(out->ip), IPSTR, IP2STR(&ip.ip));
    }
}

esp_err_t wifi_mgr_save(const char *ssid, const char *pass) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_str(h, KEY_SSID, ssid ? ssid : "");
    nvs_set_str(h, KEY_PASS, pass ? pass : "");
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

bool wifi_mgr_load(char *ssid, char *pass) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t sl = WIFI_SSID_MAXLEN + 1, pl = WIFI_PASS_MAXLEN + 1;
    bool ok = (nvs_get_str(h, KEY_SSID, ssid, &sl) == ESP_OK) && ssid[0] != '\0';
    if (ok && nvs_get_str(h, KEY_PASS, pass, &pl) != ESP_OK) pass[0] = '\0';
    nvs_close(h);
    return ok;
}

void wifi_mgr_forget(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, KEY_SSID);
    nvs_erase_key(h, KEY_PASS);
    nvs_commit(h);
    nvs_close(h);
    wifi_mgr_disconnect();   /* also stands the keepalive down, so "forget" doesn't silently reconnect */
}

esp_err_t wifi_mgr_autoconnect(int timeout_ms) {
    char ssid[WIFI_SSID_MAXLEN + 1] = { 0 }, pass[WIFI_PASS_MAXLEN + 1] = { 0 };
    if (!wifi_mgr_load(ssid, pass)) return ESP_ERR_NOT_FOUND;
    ESP_LOGI(TAG, "autoconnect -> %s", ssid);
    return wifi_mgr_connect(ssid, pass, timeout_ms);
}

void wifi_mgr_disconnect(void) {
    if (!s_inited) return;
    s_keepalive = false;                                  /* an explicit disconnect must STAY disconnected */
    if (s_retry_timer) esp_timer_stop(s_retry_timer);
    esp_wifi_disconnect();
}
