#include "config_portal.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>

static const char *TAG = "PORTAL";

// ── NVS 헬퍼 ──────────────────────────────────

static esp_err_t nvs_open_rw(nvs_handle_t *h)
{
    return nvs_open(NVS_NAMESPACE, NVS_READWRITE, h);
}

// ── config_load ────────────────────────────────
esp_err_t config_load(app_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (ret != ESP_OK) {
        // NVS에 아직 값 없음 → 기본값 사용
        ESP_LOGW(TAG, "NVS 없음 → 기본값 사용");
        strlcpy(cfg->mqtt_broker, DEFAULT_BROKER, sizeof(cfg->mqtt_broker));
        strlcpy(cfg->mqtt_user,   DEFAULT_USER,   sizeof(cfg->mqtt_user));
        strlcpy(cfg->mqtt_pass,   DEFAULT_PASS,   sizeof(cfg->mqtt_pass));
        cfg->poll_ms = DEFAULT_POLL_MS;
        return ESP_OK;
    }

    size_t len;

    len = sizeof(cfg->mqtt_broker);
    if (nvs_get_str(h, NVS_KEY_BROKER, cfg->mqtt_broker, &len) != ESP_OK
        || strlen(cfg->mqtt_broker) == 0)  // ← 추가
        strlcpy(cfg->mqtt_broker, DEFAULT_BROKER, sizeof(cfg->mqtt_broker));

    len = sizeof(cfg->mqtt_user);
    if (nvs_get_str(h, NVS_KEY_USER, cfg->mqtt_user, &len) != ESP_OK
        || strlen(cfg->mqtt_user) == 0)    // ← 추가
        strlcpy(cfg->mqtt_user, DEFAULT_USER, sizeof(cfg->mqtt_user));

    len = sizeof(cfg->mqtt_pass);
    if (nvs_get_str(h, NVS_KEY_PASS, cfg->mqtt_pass, &len) != ESP_OK
        || strlen(cfg->mqtt_pass) == 0)    // ← 추가
        strlcpy(cfg->mqtt_pass, DEFAULT_PASS, sizeof(cfg->mqtt_pass));

    if (nvs_get_u32(h, NVS_KEY_POLL_MS, &cfg->poll_ms) != ESP_OK)
        cfg->poll_ms = DEFAULT_POLL_MS;

    nvs_close(h);
    ESP_LOGI(TAG, "설정 로드 완료: broker=%s user=%s poll=%ldms",
             cfg->mqtt_broker, cfg->mqtt_user, cfg->poll_ms);
    return ESP_OK;
}

// ── portal_mode 플래그 ─────────────────────────

bool config_portal_check_flag(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
        return false;

    uint8_t flag = 0;
    nvs_get_u8(h, NVS_KEY_PORTAL, &flag);
    nvs_close(h);
    return (flag == 1);
}

static void portal_clear_flag(void)
{
    nvs_handle_t h;
    if (nvs_open_rw(&h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_PORTAL, 0);
    nvs_commit(h);
    nvs_close(h);
}

// ── 원격 트리거 ────────────────────────────────

void config_portal_request(void *arg)  // ← 시그니처 변경
{
    ESP_LOGI(TAG, "Config Portal 진입");
    config_portal_start();
    vTaskDelete(NULL);  // ← 태스크 종료
}

// ── HTML 페이지 ────────────────────────────────

static const char *PORTAL_HTML =
"<!DOCTYPE html><html><head>"
"<meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP32 Config</title>"
"<style>"
"body{font-family:sans-serif;background:#1a1a2e;color:#eee;display:flex;"
"justify-content:center;align-items:center;min-height:100vh;margin:0}"
".card{background:#16213e;border-radius:12px;padding:32px;width:340px;"
"box-shadow:0 4px 24px #0005}"
"h2{margin:0 0 24px;text-align:center;color:#e94560}"
"label{display:block;margin:12px 0 4px;font-size:.85rem;color:#aaa}"
"input{width:100%;box-sizing:border-box;padding:10px;border-radius:6px;"
"border:1px solid #0f3460;background:#0f3460;color:#fff;font-size:1rem}"
".section{margin-bottom:16px}"
".section-title{"
"font-size:.8rem;color:#e94560;text-transform:uppercase;"
"letter-spacing:1px;margin-bottom:8px;padding-bottom:6px;"
"border-bottom:1px solid #0f3460}"
".accordion-header{"
"display:flex;justify-content:space-between;align-items:center;"
"cursor:pointer;padding:10px 0;color:#aaa;font-size:.9rem;"
"border-top:1px solid #0f3460;margin-top:8px}"
".accordion-header:hover{color:#eee}"
".accordion-body{display:none}"
".accordion-body.open{display:block}"
".chevron{transition:transform .2s}"
".chevron.open{transform:rotate(180deg)}"
"button{margin-top:24px;width:100%;padding:12px;border:none;border-radius:6px;"
"background:#e94560;color:#fff;font-size:1rem;cursor:pointer}"
"button:hover{background:#c73652}"
"</style></head><body>"
"<div class='card'>"
"<h2>⚙ ESP32 Config</h2>"
"<form method='POST' action='/save'>"

"<div class='section'>"
"<div class='section-title'>📊 센서 설정</div>"
"<label>Polling 주기 (ms)</label>"
"<input name='poll' type='number' min='500' max='60000' placeholder='변경하지 않으면 비워두세요'>"
"</div>"

"<div class='section'>"
"<div class='accordion-header' onclick='toggleAdv()'>"
"<span>🔧 브로커 설정 (고급)</span>"
"<span class='chevron' id='chev'>▾</span>"
"</div>"
"<div class='accordion-body' id='adv-body'>"
"<label>MQTT Broker URI</label>"
"<input name='broker' placeholder='변경하지 않으면 비워두세요'>"
"<label>Username</label>"
"<input name='user' placeholder='변경하지 않으면 비워두세요'>"
"<label>Password</label>"
"<input name='pass' type='password' placeholder='변경하지 않으면 비워두세요'>"
"</div>"
"</div>"

"<button type='submit'>저장 &amp; 재시작</button>"
"</form>"
"</div>"
"<script>"
"function toggleAdv(){"
"const b=document.getElementById('adv-body');"
"const c=document.getElementById('chev');"
"b.classList.toggle('open');"
"c.classList.toggle('open');}"
"</script>"
"</body></html>";

static const char *SAVED_HTML =
"<!DOCTYPE html><html><head><meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>저장 완료</title>"
"<style>body{font-family:sans-serif;background:#1a1a2e;color:#eee;"
"display:flex;justify-content:center;align-items:center;min-height:100vh}"
".card{background:#16213e;border-radius:12px;padding:32px;text-align:center}"
"h2{color:#0f6}p{color:#aaa}</style></head><body>"
"<div class='card'><h2>✅ 저장 완료</h2>"
"<p>ESP32가 3초 후 재시작됩니다.</p></div></body></html>";


// ── URL 디코딩 ─────────────────────────────────
static void url_decode(char *str) {
    char *src = str, *dst = str;
    while (*src) {
        if (*src == '%' && *(src+1) && *(src+2)) {
            char hex[3] = {*(src+1), *(src+2), 0};
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

// ── HTTP 핸들러 ────────────────────────────────

static esp_err_t get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t post_handler(httpd_req_t *req)
{
    char buf[512] = {0};
    int  ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;

    // URL 디코딩 없이 단순 파싱 (값에 특수문자 없다고 가정)
    char broker[128] = {0}, user[64] = {0}, pass[64] = {0};
    uint32_t poll_ms = DEFAULT_POLL_MS;

    // sscanf로 form-urlencoded 파싱
    // 형식: broker=xxx&user=xxx&pass=xxx&poll=1000
    httpd_query_key_value(buf, "broker", broker, sizeof(broker));
    httpd_query_key_value(buf, "user",   user,   sizeof(user));
    httpd_query_key_value(buf, "pass",   pass,   sizeof(pass));
    url_decode(broker);  // ← 추가
    url_decode(user);    // ← 추가
    url_decode(pass);    // ← 추가
    

    char poll_str[16] = {0};
    if (httpd_query_key_value(buf, "poll", poll_str, sizeof(poll_str)) == ESP_OK) {
        poll_ms = (uint32_t)atoi(poll_str);
        if (poll_ms < 500)   poll_ms = 500;
        if (poll_ms > 60000) poll_ms = 60000;
    }

    ESP_LOGI(TAG, "수신: broker=%s user=%s poll=%ld", broker, user, poll_ms);

    // NVS 저장
    nvs_handle_t h;
    if (nvs_open_rw(&h) == ESP_OK) {
        nvs_set_str(h, NVS_KEY_BROKER, broker);
        nvs_set_str(h, NVS_KEY_USER,   user);
        nvs_set_str(h, NVS_KEY_PASS,   pass);
        nvs_set_u32(h, NVS_KEY_POLL_MS, poll_ms);
        nvs_set_u8(h,  NVS_KEY_PORTAL,  0);   // 플래그 클리어
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "NVS 저장 완료");
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, SAVED_HTML, HTTPD_RESP_USE_STRLEN);

    // 3초 후 재시작
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
    return ESP_OK;
}

// ── AP 모드 + 웹서버 시작 ──────────────────────

void config_portal_start(void)
{
    ESP_LOGI(TAG, "=== Config Portal 시작 ===");
    portal_clear_flag();

    // HTTP 서버 시작 (ENC28J60 유선 IP 그대로 사용)
    httpd_handle_t server = NULL;
    httpd_config_t hcfg   = HTTPD_DEFAULT_CONFIG();

    ESP_ERROR_CHECK(httpd_start(&server, &hcfg));

    httpd_uri_t uri_get = {
        .uri     = "/",
        .method  = HTTP_GET,
        .handler = get_handler,
    };
    httpd_uri_t uri_post = {
        .uri     = "/save",
        .method  = HTTP_POST,
        .handler = post_handler,
    };
    httpd_register_uri_handler(server, &uri_get);
    httpd_register_uri_handler(server, &uri_post);

    ESP_LOGI(TAG, "Config Portal 대기 중... http://[ENC28J60_IP] 접속");
    // post_handler 내부에서 esp_restart() 호출
}