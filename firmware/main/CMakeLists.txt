idf_component_register(
    SRCS "app_main.c"
    PRIV_REQUIRES esp-tls esp_wifi esp_event esp_netif nvs_flash esp_driver_i2c esp_driver_rmt
    INCLUDE_DIRS "."
    EMBED_TXTFILES "mosquitto.org.crt"
)
