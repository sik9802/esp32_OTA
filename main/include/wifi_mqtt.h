#ifndef WIFI_MQTT_H
#define WIFI_MQTT_H

#include "esp_err.h"
#include "esp_log.h"

esp_err_t wifi_mqtt_init(void);
esp_err_t mqtt_start(void);       // ← 추가
void      mqtt_publish_sensor(float temperature, float humidity);
void wait_for_ip(void);
#endif