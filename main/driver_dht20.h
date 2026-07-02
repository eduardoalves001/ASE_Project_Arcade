#ifndef __driver_dht20_H__
#define __driver_dht20_H__

#include "driver/i2c_master.h"

#define DHT20_ADDR                  0x38     // Default I2C address for DHT20
#define DHT20_SCL_DFLT_FREQ_HZ      100000   // Typical I2C frequency (100kHz)

/**
 * @brief Initialize the DHT20 sensor
 * 
 * @param[out] pBusHandle Pointer to return the I2C bus handle
 * @param[out] pSensorHandle Pointer to return the I2C device handle
 * @param[in] sensorAddr I2C address of the sensor (usually 0x38)
 * @param[in] sdaPin SDA GPIO pin
 * @param[in] sclPin SCL GPIO pin
 * @param[in] clkSpeedHz I2C clock speed in Hz
 */
void dht20_init(i2c_master_bus_handle_t* pBusHandle,
                i2c_master_dev_handle_t* pSensorHandle,
                uint8_t sensorAddr, int sdaPin, int sclPin, uint32_t clkSpeedHz);

/**
 * @brief Free resources used by the DHT20 sensor
 * 
 * @param[in] busHandle I2C bus handle
 * @param[in] sensorHandle I2C device handle
 */
void dht20_free(i2c_master_bus_handle_t busHandle,
                i2c_master_dev_handle_t sensorHandle);

/**
 * @brief Trigger a measurement and read temperature and humidity (blocking)
 * This function triggers a measurement, waits for it to complete (~80ms), and reads the results.
 * 
 * @param[in] sensorHandle I2C device handle
 * @param[out] pTemperature Pointer to store temperature (Celsius)
 * @param[out] pHumidity Pointer to store humidity (%)
 */
void dht20_read_data_after_wait(i2c_master_dev_handle_t sensorHandle, float* pTemperature, float* pHumidity);

// Lower level functions for finer control if needed, matching TC74 style

/**
 * @brief Trigger a new measurement
 * 
 * @param[in] sensorHandle I2C device handle
 */
void dht20_trigger_measurement(i2c_master_dev_handle_t sensorHandle);

/**
 * @brief Check if the measurement is ready
 * Checks the status byte (bit 7)
 * 
 * @param[in] sensorHandle I2C device handle
 * @return true if ready, false otherwise
 */
bool dht20_is_ready(i2c_master_dev_handle_t sensorHandle);

/**
 * @brief Read data from the sensor without triggering a new measurement.
 * Assumes measurement has been triggered and is ready.
 * 
 * @param[in] sensorHandle I2C device handle
 * @param[out] pTemperature Pointer to store temperature (Celsius)
 * @param[out] pHumidity Pointer to store humidity (%)
 */
void dht20_read_data(i2c_master_dev_handle_t sensorHandle, float* pTemperature, float* pHumidity);

#endif // __driver_dht20_H__
