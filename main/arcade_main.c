#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "esp_adc/adc_oneshot.h"

#include "board_pins.h"
#include "arcade_state.h"
#include "storage_sd.h"
#include "sensors.h"
#include "games.h"
#include "display_arcade.h"
#include "network_mqtt.h"

#include "driver_dht20.h"
#include "st7735.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"

static const char *TAG = "Arcade";

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    uint32_t gpio_num = (uint32_t)arg;
    xQueueSendFromISR(button_evt_queue, &gpio_num, NULL);
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Booting ESP-Arcade...");

    /* shared state */
    memset(&g_state, 0, sizeof(g_state));
    g_state.screen       = SCREEN_MENU;
    g_state.current_game = GAME_SNAKE;
    state_mutex      = xSemaphoreCreateMutex();
    button_evt_queue = xQueueCreate(10, sizeof(uint32_t));

    /* status LED */
    gpio_config_t led_conf = {
        .intr_type    = GPIO_INTR_DISABLE,
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_down_en = 0,
        .pull_up_en   = 0,
    };
    gpio_config(&led_conf);

    /* three buttons: input, pull-up, falling-edge interrupt */
    gpio_config_t io_conf = {
        .intr_type    = GPIO_INTR_NEGEDGE,
        .pin_bit_mask = (1ULL << BUTTON_A_GPIO) | (1ULL << BUTTON_B_GPIO) | (1ULL << BUTTON_C_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = 1,
    };
    gpio_config(&io_conf);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BUTTON_A_GPIO, gpio_isr_handler, (void *)BUTTON_A_GPIO);
    gpio_isr_handler_add(BUTTON_B_GPIO, gpio_isr_handler, (void *)BUTTON_B_GPIO);
    gpio_isr_handler_add(BUTTON_C_GPIO, gpio_isr_handler, (void *)BUTTON_C_GPIO);

    /* potentiometer on ADC1 */
    adc_oneshot_unit_init_cfg_t adc1InitCfg = { .unit_id = ADC_UNIT_1 };
    adc_oneshot_new_unit(&adc1InitCfg, &adc1_handle);
    adc_oneshot_chan_cfg_t adcChanCfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL, &adcChanCfg);

    /* DHT20 on I2C */
    static i2c_master_bus_handle_t busHandle;
    static i2c_master_dev_handle_t dht20Handle;
    dht20_init(&busHandle, &dht20Handle, DHT20_ADDR, I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO, I2C_MASTER_FREQ_HZ);

    /* shared SPI2 bus (TFT + SD card) */
    spi_bus_config_t buscfg = {
        .mosi_io_num   = PIN_MOSI,
        .miso_io_num   = PIN_MISO,
        .sclk_io_num   = PIN_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    /* SD card over SPI */
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs  = PIN_SD_CS;
    slot_config.host_id  = host.slot;

    esp_err_t ret_sd = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_config, &mount_config, &card);
    if (ret_sd == ESP_OK) {
        ESP_LOGI(TAG, "SD card mounted.");
        load_high_scores();
    } else {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret_sd));
    }

    /* TFT display (shares the SPI bus -> skip_bus_init) */
    st7735_config_t tft_cfg = {
        .mosi_io_num  = PIN_MOSI,
        .sclk_io_num  = PIN_CLK,
        .cs_io_num    = PIN_CS,
        .dc_io_num    = PIN_DC,
        .rst_io_num   = PIN_RST,
        .bl_io_num    = PIN_BL,
        .host_id      = SPI2_HOST,
        .skip_bus_init = true,
    };
    st7735_init(&tft_cfg);
    st7735_fill_screen(ST7735_BLACK);
    st7735_draw_string(28, 34, "ESP-ARCADE", ST7735_YELLOW, ST7735_BLACK, 1);

    /* tasks */
    xTaskCreate(sensor_task,  "sensor_task",  4096, (void *)&dht20Handle, 5, NULL);
    xTaskCreate(game_task,    "game_task",    8192, NULL, 6, NULL);
    xTaskCreate(display_task, "display_task", 8192, NULL, 5, NULL);

    /* Wi-Fi + MQTT leaderboard (games keep running even with no network) */
    wifi_init_sta();
    xTaskCreate(net_task, "net_task", 8192, NULL, 4, &net_task_handle);

    ESP_LOGI(TAG, "ESP-Arcade initialized.");
}
