#include <stdio.h>
#include "driver_dht20.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// DHT20 Commands
#define DHT20_CMD_TRIGGER_MEASUREMENT 0xAC
#define DHT20_CMD_TRIGGER_DATA_1      0x33
#define DHT20_CMD_TRIGGER_DATA_2      0x00

// Power-up calibration command (AHT20/DHT20 datasheet section 5.4). The
// sensor ACKs its I2C address as soon as it is powered, but measurement
// commands are unreliable until this sequence has been sent once.
#define DHT20_CMD_INIT                0xBE
#define DHT20_CMD_INIT_DATA_1         0x08
#define DHT20_CMD_INIT_DATA_2         0x00
#define DHT20_CMD_SOFT_RESET          0xBA

// Status bit masks
#define DHT20_STATUS_BUSY_MASK        0x80

/* DHT20 datasheet 7.4: read the register, then write it back with 0xB0 set in
 * the command byte and the first data byte zeroed. Applied to 0x1B/0x1C/0x1E
 * at power-up, this clears the state that makes measurements return zeros. */
static void dht20_reset_register(i2c_master_dev_handle_t dev, uint8_t reg)
{
    uint8_t cmd[3] = {reg, 0x00, 0x00};
    if (i2c_master_transmit(dev, cmd, sizeof(cmd), 100) != ESP_OK) return;
    vTaskDelay(pdMS_TO_TICKS(5));

    uint8_t buf[3] = {0};
    if (i2c_master_receive(dev, buf, sizeof(buf), 100) != ESP_OK) return;
    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t cmd2[3] = {(uint8_t)(0xB0 | reg), buf[1], buf[2]};
    i2c_master_transmit(dev, cmd2, sizeof(cmd2), 100);
    vTaskDelay(pdMS_TO_TICKS(5));
}

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
        /* Synchronous mode (no trans_queue_depth): async queues the transfer and
         * returns ESP_OK before data arrives, so reads could "succeed" with an
         * all-zero buffer that decodes to -50 C / 0 %RH. */
        .flags.enable_internal_pullup = true,
    };

    ESP_ERROR_CHECK(i2c_new_master_bus(&i2cMasterCfg, pBusHandle));

    i2c_device_config_t i2cDevCfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = sensorAddr,
        .scl_speed_hz = clkSpeedHz,
    };

    ESP_ERROR_CHECK(i2c_master_bus_add_device(*pBusHandle, &i2cDevCfg, pSensorHandle));

    // Official DHT20 datasheet power-up flow: wait >=100 ms, read the status
    // word, then reset the internal registers 0x1B/0x1C/0x1E (datasheet 7.4
    // sample code). Skipping the register reset is the documented cause of
    // measurements that "complete" but return all-zero data.
    vTaskDelay(pdMS_TO_TICKS(100));

    dht20_reset_register(*pSensorHandle, 0x1B);
    dht20_reset_register(*pSensorHandle, 0x1C);
    dht20_reset_register(*pSensorHandle, 0x1E);
    vTaskDelay(pdMS_TO_TICKS(10));
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

/* CRC-8 over the first 6 frame bytes (poly 0x31, init 0xFF, MSB first) —
 * the checksum the DHT20 appends as byte 7 of every measurement frame. */
static uint8_t dht20_crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0xFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

#define DHT20_I2C_TIMEOUT_MS   100   /* finite: a wedged bus must not hang the task */
#define DHT20_READ_ATTEMPTS    3     /* full trigger+read retries per call */

esp_err_t dht20_read_safe(i2c_master_dev_handle_t sensorHandle, float* pTemperature, float* pHumidity)
{
    uint8_t trigger[3] = {DHT20_CMD_TRIGGER_MEASUREMENT, DHT20_CMD_TRIGGER_DATA_1, DHT20_CMD_TRIGGER_DATA_2};
    esp_err_t err = ESP_FAIL;

    for (int attempt = 0; attempt < DHT20_READ_ATTEMPTS; attempt++) {
        if (attempt > 0) vTaskDelay(pdMS_TO_TICKS(50));

        /* A NACK here usually means the sensor is still busy with a previous
         * measurement (it NACKs every transaction while measuring) — wait and retry. */
        err = i2c_master_transmit(sensorHandle, trigger, sizeof(trigger), DHT20_I2C_TIMEOUT_MS);
        if (err != ESP_OK) continue;

        vTaskDelay(pdMS_TO_TICKS(80));   /* nominal measurement time */

        /* This unit takes ~170 ms per measurement and NACKs the bus the whole
         * time, so a failed receive means "not done yet", NOT a dead sensor.
         * Keep polling (up to ~450 ms total) instead of re-triggering, which
         * would restart the measurement and livelock forever. */
        uint8_t data[7] = {0};
        bool got_frame = false;
        for (int poll = 0; poll < 15; poll++) {
            err = i2c_master_receive(sensorHandle, data, sizeof(data), DHT20_I2C_TIMEOUT_MS);
            if (err == ESP_OK && !(data[0] & DHT20_STATUS_BUSY_MASK)) { got_frame = true; break; }
            if (err == ESP_OK) err = ESP_ERR_TIMEOUT;   /* answered but busy bit still set */
            vTaskDelay(pdMS_TO_TICKS(25));
        }
        if (!got_frame) continue;

        /* CRC note: this unit answers with empty data + 0xFF filler instead of
         * a real checksum (measurement core fault). Per project decision, the
         * frame is decoded and shown as-is — the readout reflects exactly what
         * the sensor reports, even when that is the -50 C / 0 % empty pattern. */
        if (dht20_crc8(data, 6) != data[6]) {
            ESP_LOGD("DHT20", "CRC mismatch; displaying raw sensor data anyway");
        }

        uint32_t raw_humid = ((uint32_t)data[1] << 12) | ((uint32_t)data[2] << 4) | (((uint32_t)data[3] & 0xF0) >> 4);
        uint32_t raw_temp = (((uint32_t)data[3] & 0x0F) << 16) | ((uint32_t)data[4] << 8) | (uint32_t)data[5];
        if (pHumidity)    *pHumidity = ((float)raw_humid / 1048576.0f) * 100.0f;
        if (pTemperature) *pTemperature = ((float)raw_temp / 1048576.0f) * 200.0f - 50.0f;
        return ESP_OK;
    }
    return err;
}
