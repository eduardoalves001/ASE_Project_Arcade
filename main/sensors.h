#ifndef SENSORS_H
#define SENSORS_H

/* Reads the DHT20 once per second and stores temp/humidity in g_state.
 * The temperature drives "hard mode" (see HARD_TEMP_C). */
void sensor_task(void *pvParameters);

#endif /* SENSORS_H */
