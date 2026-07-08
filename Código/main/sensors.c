#include "sensors.h"
#include "arcade_state.h"
#include "driver_dht20.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "Sensor";

#define DHT20_MISS_LIMIT   3      /* consecutive failures before hiding the readout */
#define DHT20_PERIOD_MS    2000   /* healthy polling period */
#define DHT20_RETRY_MS     5000   /* slower probing while the sensor is absent */

void sensor_task(void *pvParameters)
{
    i2c_master_dev_handle_t *sensor = (i2c_master_dev_handle_t *)pvParameters;
    int fails = 0;

    while (1) {
        float t = 0.0f, h = 0.0f;
        esp_err_t err = dht20_read_safe(*sensor, &t, &h);

        if (err == ESP_OK) {
            bool was_ok = false;
            if (xSemaphoreTake(state_mutex, portMAX_DELAY)) {
                was_ok        = g_state.env_ok;
                g_state.temp  = t;
                g_state.hum   = h;
                g_state.env_ok = true;
                xSemaphoreGive(state_mutex);
            }
            if (!was_ok) ESP_LOGI(TAG, "DHT20 online: %.1f C, %.0f %%RH", t, h);
            fails = 0;
        } else if (fails < DHT20_MISS_LIMIT && ++fails == DHT20_MISS_LIMIT) {
            if (xSemaphoreTake(state_mutex, portMAX_DELAY)) {
                g_state.env_ok = false;
                xSemaphoreGive(state_mutex);
            }
            ESP_LOGW(TAG, "DHT20 not responding; ambient readout hidden (games unaffected).");
        }

        vTaskDelay(pdMS_TO_TICKS(fails >= DHT20_MISS_LIMIT ? DHT20_RETRY_MS : DHT20_PERIOD_MS));
    }
}
