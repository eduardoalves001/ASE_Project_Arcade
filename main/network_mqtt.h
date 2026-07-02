#ifndef NETWORK_MQTT_H
#define NETWORK_MQTT_H

/* Brings up the Wi-Fi station and blocks until it gets an IP. */
void wifi_init_sta(void);

/* Publishes arcade/status over secure MQTT and handles arcade/command.
 * Notify this task (xTaskNotifyGive on net_task_handle) to force a publish. */
void net_task(void *pvParameters);

#endif /* NETWORK_MQTT_H */
