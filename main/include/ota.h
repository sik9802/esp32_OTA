#pragma once
#include "mqtt_client.h"

void ota_task(void *pvParameter);  // ota_start → ota_task로 변경
void ota_set_client(esp_mqtt_client_handle_t client);