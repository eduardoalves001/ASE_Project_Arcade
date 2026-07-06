#ifndef SENSORS_H
#define SENSORS_H

/* Ambient telemetry task (DHT20 on I2C).
 * Reads temperature/humidity every 2 s into g_state.temp / g_state.hum and
 * flags availability in g_state.env_ok. Display/dashboard only — it has NO
 * effect on gameplay, the LED or any other component. The console runs
 * normally when the sensor is absent (the readout is simply hidden). */
void sensor_task(void *pvParameters);

#endif /* SENSORS_H */
