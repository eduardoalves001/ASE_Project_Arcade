#include "sensors.h"
#include "arcade_state.h"
#include "driver_dht20.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void sensor_task(void *pvParameters)
{
    i2c_master_dev_handle_t *sensor = (i2c_master_dev_handle_t *)pvParameters;

    while (1) {
        float temperature = 0.0f;
        float humidity    = 0.0f;
        dht20_read_data_after_wait(*sensor, &temperature, &humidity);

        if (xSemaphoreTake(state_mutex, portMAX_DELAY)) {
            g_state.temp = temperature;
            g_state.hum  = humidity;
            xSemaphoreGive(state_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
