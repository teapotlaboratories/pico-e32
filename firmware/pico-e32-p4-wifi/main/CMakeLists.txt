idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_wifi_remote esp_netif esp_event nvs_flash esp_hosted
)
