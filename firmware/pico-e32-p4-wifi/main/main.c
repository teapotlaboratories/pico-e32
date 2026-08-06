/* ESP32-P4 + ESP32-C6 WiFi bring-up probe.
 *
 * The P4 has no radio; WiFi is delegated over SDIO (esp-hosted) to the on-board ESP32-C6 running esp-hosted
 * slave firmware. esp_wifi_remote makes the standard esp_wifi_* calls transparently RPC to the C6. This probe
 * does the minimum to answer one question: does the C6 respond? — bring up STA and run one scan, logging the
 * transport sync + AP count. Guition JC4880P443C SDIO pins: CLK18 CMD19 D0-3=14/15/16/17, C6 reset GPIO54
 * (map from GustavoH-Smart/esp32p4 README_WIFI + the CNX P4+C6 writeup; identical to esp_hosted's P4 defaults).
 * Slave-firmware flashing route (esp-hosted host-performs-slave-OTA over the same SDIO link) from the
 * espressif/esp-hosted `host_performs_slave_ota` example. */
#include <string.h>
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"          /* provided by esp_wifi_remote — API-identical, forwarded to the C6 */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "p4wifi";

void app_main(void) {
    ESP_LOGI(TAG, "== P4+C6 WiFi probe ==");

    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) { nvs_flash_erase(); nvs_flash_init(); }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    e = esp_wifi_init(&cfg);
    ESP_LOGI(TAG, "esp_wifi_init -> %s", esp_err_to_name(e));   /* fails/hangs here if the C6 isn't talking */
    if (e != ESP_OK) { ESP_LOGE(TAG, "no C6 transport — is the C6 flashed with esp-hosted slave fw?"); return; }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "STA up — scanning...");

    if (esp_wifi_scan_start(NULL, true) != ESP_OK) { ESP_LOGE(TAG, "scan_start failed"); return; }
    uint16_t n = 0; esp_wifi_scan_get_ap_num(&n);
    ESP_LOGI(TAG, "scan found %u APs", n);
    wifi_ap_record_t recs[12]; uint16_t got = 12;
    esp_wifi_scan_get_ap_records(&got, recs);
    for (int i = 0; i < got; i++)
        ESP_LOGI(TAG, "  [%d] %-32s rssi=%d ch=%d", i, (char *)recs[i].ssid, recs[i].rssi, recs[i].primary);
    ESP_LOGI(TAG, "== scan OK: C6 link is UP ==");

    /* connect test — proves the full RPC path (not just scan) survives the host/slave version gap. TEMP creds. */
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, "Tukang Ketoprak", sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, "hermanudin", sizeof wc.sta.password);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_LOGI(TAG, "connecting to '%s'...", wc.sta.ssid);
    esp_wifi_connect();
    for (int t = 0; t < 20; t++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            esp_netif_ip_info_t ip;
            if (nif && esp_netif_get_ip_info(nif, &ip) == ESP_OK && ip.ip.addr) {
                ESP_LOGI(TAG, "== CONNECTED: %s  ip=" IPSTR "  rssi=%d ==", ap.ssid, IP2STR(&ip.ip), ap.rssi);
                return;
            }
        }
    }
    ESP_LOGW(TAG, "== connect did not get an IP in 20s ==");
}
