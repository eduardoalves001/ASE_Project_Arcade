# --- GAME BUILD ---
idf_component_register(SRCS "arcade_main.c" "arcade_state.c" "games.c" "display_arcade.c" "storage_sd.c" "network_mqtt.c"
                    INCLUDE_DIRS "."
                    REQUIRES st7735_driver esp_wifi nvs_flash esp_event esp_netif mqtt fatfs sdmmc vfs esp_adc esp_timer driver esp_hw_support)

# --- DIAGNOSTIC BUILD --- swap the SRCS above to "diagnostic.c" "driver_dht20.c" to re-run the bring-up diagnostic.

target_add_binary_data(${COMPONENT_LIB} "mqtt_broker_ca.pem" TEXT)
