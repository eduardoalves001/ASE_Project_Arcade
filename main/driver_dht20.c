#include <stdio.h>
#include "driver_dht20.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// DHT20 Commands
#define DHT20_CMD_TRIGGER_MEASUREMENT 0xAC
#define DHT20_CMD_TRIGGER_DATA_1      0x33
#define DHT20_CMD_TRIGGER_DATA_2      0x00

// Status bit masks
#define DHT20_STATUS_BUSY_MASK        0x80

void dht20_init(i2c_master_bus_handle_t* pBusHandle,
                i2c_master_dev_handle_t* pSensorHandle,
                uint8_t sensorAddr, int sdaPin, int sclPin, uint32_t clkSpeedHz)
{
    i2c_master_bus_config_t i2cMasterCfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = sclPin,
        .sda_io_num = sdaPin,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    ESP_ERROR_CHECK(i2c_new_master_bus(&i2cMasterCfg, pBusHandle));

    i2c_device_config_t i2cDevCfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = sensorAddr,
        .scl_speed_hz = clkSpeedHz,
    };

    ESP_ERROR_CHECK(i2c_master_bus_add_device(*pBusHandle, &i2cDevCfg, pSensorHandle));
}

void dht20_free(i2c_master_bus_handle_t busHandle,
                i2c_master_dev_handle_t sensorHandle)
{
    ESP_ERROR_CHECK(i2c_master_bus_rm_device(sensorHandle));
    ESP_ERROR_CHECK(i2c_del_master_bus(busHandle));
}

void dht20_trigger_measurement(i2c_master_dev_handle_t sensorHandle)
{
    uint8_t buffer[3] = {DHT20_CMD_TRIGGER_MEASUREMENT, DHT20_CMD_TRIGGER_DATA_1, DHT20_CMD_TRIGGER_DATA_2};
    ESP_ERROR_CHECK(i2c_master_transmit(sensorHandle, buffer, sizeof(buffer), -1));
}

bool dht20_is_ready(i2c_master_dev_handle_t sensorHandle)
{
    uint8_t status;
    ESP_ERROR_CHECK(i2c_master_receive(sensorHandle, &status, 1, -1));
    return !(status & DHT20_STATUS_BUSY_MASK);
}

void dht20_read_data(i2c_master_dev_handle_t sensorHandle, float* pTemperature, float* pHumidity)
{
    uint8_t data[7];
    // Read 7 bytes: Status, H1, H2, H3/T1, T2, T3, CRC
    ESP_ERROR_CHECK(i2c_master_receive(sensorHandle, data, sizeof(data), -1));

    uint32_t raw_humid = ((uint32_t)data[1] << 12) | ((uint32_t)data[2] << 4) | (((uint32_t)data[3] & 0xF0) >> 4);
    uint32_t raw_temp = (((uint32_t)data[3] & 0x0F) << 16) | ((uint32_t)data[4] << 8) | (uint32_t)data[5];

    if (pHumidity) {
        *pHumidity = ((float)raw_humid / 1048576.0f) * 100.0f;
    }
    if (pTemperature) {
        *pTemperature = ((float)raw_temp / 1048576.0f) * 200.0f - 50.0f;
    }
}

void dht20_read_data_after_wait(i2c_master_dev_handle_t sensorHandle, float* pTemperature, float* pHumidity)
{
    dht20_trigger_measurement(sensorHandle);
    
    // Manual delay of 80ms as per datasheet
    vTaskDelay(pdMS_TO_TICKS(80));
    
    // Optional: could loop check dht20_is_ready logic here for robustness, but strict delay usually works.
    
    dht20_read_data(sensorHandle, pTemperature, pHumidity);
}
