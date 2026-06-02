#pragma once

#include "esp_err.h"
#include <stdbool.h>

// NVS 네임스페이스 / 키 정의
#define NVS_NAMESPACE       "config"
#define NVS_KEY_PORTAL      "portal_mode"
#define NVS_KEY_BROKER      "mqtt_broker"
#define NVS_KEY_USER        "mqtt_user"
#define NVS_KEY_PASS        "mqtt_pass"
#define NVS_KEY_POLL_MS     "poll_ms"

// 기본값 (NVS에 값이 없을 때 사용)
#define DEFAULT_BROKER      "mqtts://604efa86dab4428c9f2f4de139f5ca0a.s1.eu.hivemq.cloud:8883"
#define DEFAULT_USER        "chunsik"
#define DEFAULT_PASS        "ESP32server"
#define DEFAULT_POLL_MS     1000

// Config 값 구조체 (런타임에 NVS에서 로드해 사용)
typedef struct {
    char    mqtt_broker[128];
    char    mqtt_user[64];
    char    mqtt_pass[64];
    uint32_t poll_ms;
} app_config_t;

// ── 공개 API ───────────────────────────────────

// 부팅 시 NVS에서 설정 로드 (없으면 기본값)
esp_err_t config_load(app_config_t *cfg);

// portal_mode 플래그 확인 (true → Portal 진입 필요)
bool config_portal_check_flag(void);

// MQTT 원격 트리거: NVS에 플래그 기록 후 재시작
void config_portal_request(void *arg);

// AP 모드 웹서버 실행 (설정 완료 시 내부에서 esp_restart 호출)
void config_portal_start(void);