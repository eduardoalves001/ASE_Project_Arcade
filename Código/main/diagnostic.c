/*
 * Temporary hardware diagnostic for ESP-Arcade.
 * Prints the potentiometer % and button state over serial every 300 ms,
 * mirrors the button onto the LED, probes the DHT20/SD, and draws colour
 * bars on the TFT. Built instead of the game by main/CMakeLists.txt while
 * bring-up is in progress; revert SRCS to restore the game.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "esp_adc/adc_oneshot.h"

#include "board_pins.h"
#include "driver_dht20.h"
#include "st7735.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"

static const char *TAG = "DIAG";

void app_main(void)
{
    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "        ESP-Arcade  HARDWARE DIAGNOSTIC");
    ESP_LOGI(TAG, "==================================================");

    /* ---- LED output ---- */
    gpio_config_t led = { .pin_bit_mask = 1ULL << LED_GPIO, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&led);

    /* ---- Button (single) on GPIO23, input + pull-up ---- */
    gpio_config_t btn = { .pin_bit_mask = 1ULL << BUTTON_A_GPIO, .mode = GPIO_MODE_INPUT, .pull_up_en = 1 };
    gpio_config(&btn);

    /* ---- Potentiometer on ADC1 ---- */
    adc_oneshot_unit_handle_t adc = NULL;
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = ADC_UNIT_1 };
    adc_oneshot_new_unit(&ucfg, &adc);
    adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    adc_oneshot_config_channel(adc, ADC_CHANNEL, &ccfg);

    /* ---- DHT20 probe (expected absent for now) ---- */
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dht;
    dht20_init(&bus, &dht, DHT20_ADDR, I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO, I2C_MASTER_FREQ_HZ);
    float t = 0, h = 0;
    esp_err_t derr = dht20_read_safe(dht, &t, &h);
    ESP_LOGI(TAG, "DHT20 .......... %s", derr == ESP_OK ? "OK" : "not connected (ok, sensor still missing)");

    /* ---- I2C bus scan: list every address that ACKs ---- */
    int i2c_found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(bus, addr, 50) == ESP_OK) {
            ESP_LOGI(TAG, "  I2C device @ 0x%02X%s", addr, addr == DHT20_ADDR ? "  <- DHT20" : "");
            i2c_found++;
        }
    }
    ESP_LOGI(TAG, "I2C scan ....... %d device(s) on GPIO6/7", i2c_found);

    /* ---- Shared SPI bus ---- */
    spi_bus_config_t bcfg = {
        .mosi_io_num = PIN_MOSI, .miso_io_num = PIN_MISO, .sclk_io_num = PIN_CLK,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 4000,
    };
    esp_err_t serr = spi_bus_initialize(SPI2_HOST, &bcfg, SPI_DMA_CH_AUTO);
    ESP_LOGI(TAG, "SPI bus ........ %s", esp_err_to_name(serr));

    /* ---- SD card ---- */
    esp_vfs_fat_sdmmc_mount_config_t mcfg = { .format_if_mount_failed = false, .max_files = 3, .allocation_unit_size = 16 * 1024 };
    sdmmc_card_t *card = NULL;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    sdspi_device_config_t scfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    scfg.gpio_cs = PIN_SD_CS;
    scfg.host_id = host.slot;
    esp_err_t sderr = esp_vfs_fat_sdspi_mount("/sdcard", &host, &scfg, &mcfg, &card);
    ESP_LOGI(TAG, "SD card ........ %s", sderr == ESP_OK ? "MOUNTED" : esp_err_to_name(sderr));

    /* ---- TFT: colour bars so a working screen is obvious ---- */
    st7735_config_t tft = {
        .mosi_io_num = PIN_MOSI, .sclk_io_num = PIN_CLK, .cs_io_num = PIN_CS,
        .dc_io_num = PIN_DC, .rst_io_num = PIN_RST, .bl_io_num = PIN_BL,
        .host_id = SPI2_HOST, .skip_bus_init = true,
    };
    st7735_init(&tft);
    st7735_fill_screen(ST7735_BLACK);
    st7735_fill_rect(0,  0, 160, 16, ST7735_RED);
    st7735_fill_rect(0, 16, 160, 16, ST7735_GREEN);
    st7735_fill_rect(0, 32, 160, 16, ST7735_BLUE);
    st7735_draw_string(6, 54, "DIAG: turn pot,",  ST7735_WHITE, ST7735_BLACK, 1);
    st7735_draw_string(6, 66, "press the button", ST7735_WHITE, ST7735_BLACK, 1);

    ESP_LOGI(TAG, "--------------------------------------------------");
    ESP_LOGI(TAG, "Now TURN the pot and PRESS the button.");
    ESP_LOGI(TAG, "The LED should light while the button is pressed.");
    ESP_LOGI(TAG, "--------------------------------------------------");

    while (1) {
        static int pot_min = 4095, pot_max = 0;
        int raw = 0;
        adc_oneshot_read(adc, ADC_CHANNEL, &raw);
        int pct = (raw * 100) / 4095;
        if (raw < pot_min) pot_min = raw;
        if (raw > pot_max) pot_max = raw;
        int pressed = (gpio_get_level(BUTTON_A_GPIO) == 0);   /* active low */

        gpio_set_level(LED_GPIO, pressed);                    /* LED mirrors the button */

        ESP_LOGI(TAG, "POT %3d%%  [seen min=%3d%%  max=%3d%%]   BUTTON=%s   LED=%s",
                 pct, (pot_min * 100) / 4095, (pot_max * 100) / 4095,
                 pressed ? "PRESSED " : "released", pressed ? "ON" : "off");

        /* live pot bar along the bottom of the TFT */
        st7735_fill_rect(0, 78, 160, 2, ST7735_BLACK);
        st7735_fill_rect(0, 78, (pct * 160) / 100, 2, ST7735_YELLOW);

        vTaskDelay(pdMS_TO_TICKS(300));
    }
}
