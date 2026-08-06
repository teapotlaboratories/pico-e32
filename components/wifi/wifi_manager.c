/* wifi_manager — one esp_wifi STA front-end over both backends (see wifi_manager.h): native esp_wifi on the S3,
 * esp_wifi_remote -> esp-hosted -> ESP32-C6 on the P4. Backend chosen by IDF_TARGET in this component's CMake. */
#include "wifi_manager.h"

#include <string.h>
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
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

static bool               s_inited = false;
static esp_netif_t       *s_netif  = NULL;
static EventGroupHandle_t s_events = NULL;
static SemaphoreHandle_t  s_lock   = NULL;   /* serialises scan/connect (boot autoconnect vs the menu) */
static int                s_retry  = 0;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry < MAX_RETRY) { s_retry++; esp_wifi_connect(); }
        else                     { xEventGroupSetBits(s_events, BIT_FAILED); }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry = 0;
        xEventGroupSetBits(s_events, BIT_GOT_IP);
    }
}

esp_err_t wifi_mgr_init(void) {
    if (s_inited) return ESP_OK;

#if CONFIG_IDF_TARGET_ESP32P4
    /* Bring up the esp-hosted SDIO link to the C6 HERE, from app_main, rather than from esp-hosted's own boot
     * constructor. That constructor (ESP_ERROR_CHECK(esp_hosted_init()) at __attribute__((constructor)) time)
     * stalls the C6 handshake in the full firmware — it runs before app_main, racing the other C++ global
     * constructors, and the board never reaches app_main. Deferring it to here (after board bring-up) makes it
     * reliable; a minimal app is unaffected either way. esp_hosted_init() is idempotent. */
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
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));   /* we own persistence via NVS ourselves */

    s_events = xEventGroupCreate();
    s_lock   = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_inited = true;
    ESP_LOGI(TAG, "STA up");
    return ESP_OK;
}

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
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    if (pass) strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    /* leave threshold.authmode at 0 (OPEN) so it accepts whatever the AP offers (open or WPA/WPA2/WPA3) */
    if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK) {
        ret = ESP_FAIL;
    } else {
        esp_wifi_disconnect();
        xEventGroupClearBits(s_events, BIT_GOT_IP | BIT_FAILED);
        s_retry = 0;
        ret = esp_wifi_connect();
        if (ret == ESP_OK) {
            EventBits_t bits = xEventGroupWaitBits(s_events, BIT_GOT_IP | BIT_FAILED,
                                                   pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
            if (bits & BIT_GOT_IP) {
                ret = ESP_OK;
            } else {
                esp_wifi_disconnect();        /* give up cleanly on timeout/fail */
                ret = (bits & BIT_FAILED) ? ESP_FAIL : ESP_ERR_TIMEOUT;
            }
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
    if (s_inited) esp_wifi_disconnect();
}

esp_err_t wifi_mgr_autoconnect(int timeout_ms) {
    char ssid[WIFI_SSID_MAXLEN + 1] = { 0 }, pass[WIFI_PASS_MAXLEN + 1] = { 0 };
    if (!wifi_mgr_load(ssid, pass)) return ESP_ERR_NOT_FOUND;
    ESP_LOGI(TAG, "autoconnect -> %s", ssid);
    return wifi_mgr_connect(ssid, pass, timeout_ms);
}

void wifi_mgr_disconnect(void) {
    if (s_inited) esp_wifi_disconnect();
}
