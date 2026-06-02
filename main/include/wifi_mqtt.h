#ifndef WIFI_MQTT_H
#define WIFI_MQTT_H

#include "esp_err.h"
#include "esp_log.h"
#include "config_portal.h"

esp_err_t wifi_mqtt_init_with_config(const app_config_t *cfg);
esp_err_t mqtt_start(const app_config_t *cfg);       // ← 추가
void      mqtt_publish_sensor(float temperature, float humidity);
void wait_for_ip(void);
#endif