#include "uart_modbus.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "MODBUS";

esp_err_t uart_modbus_init(void)
{
    ESP_LOGI(TAG, "UART2 RS-485 초기화 시작");

    // ── ① 파라미터 설정 ─────────────────────────────────
    // uart_param_config() 내부 동작:
    //   UART_CLKDIV_REG  → 80MHz / 9600 = 8333 기록 (Baud Rate)
    //   UART_CONF0_REG   → BIT_NUM=3(8비트), PARITY_EN=0, STOP_BIT=1
    uart_config_t uart_config = {
        .baud_rate  = MODBUS_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,       // UART_CONF0_REG UART_BIT_NUM = 3
        .parity     = UART_PARITY_DISABLE,    // UART_CONF0_REG UART_PARITY_EN = 0
        .stop_bits  = UART_STOP_BITS_1,       // UART_CONF0_REG UART_STOP_BIT_NUM = 1
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,          // APB_CLK 80MHz 선택
    };
    ESP_ERROR_CHECK(uart_param_config(MODBUS_UART_NUM, &uart_config)); // 
    ESP_LOGI(TAG, "Baud Rate %d 설정 완료", MODBUS_BAUD_RATE);

    // ── ② 핀 연결 ───────────────────────────────────────
    // uart_set_pin() 내부 동작:
    //   IO_MUX_GPIO17_REG MCU_SEL = 4  → U2TXD 기능
    //   IO_MUX_GPIO16_REG MCU_SEL = 4  → U2RXD 기능
    //   RTS/CTS = UART_PIN_NO_CHANGE   → RS-485 모듈이 자동 처리
    ESP_ERROR_CHECK(uart_set_pin(
        MODBUS_UART_NUM,
        MODBUS_TX_PIN,
        MODBUS_RX_PIN,
        UART_PIN_NO_CHANGE,   // RTS: DE/RE 자동 처리 모듈 사용 → 불필요
        UART_PIN_NO_CHANGE    // CTS: 미사용
    ));
    ESP_LOGI(TAG, "TX=GPIO%d RX=GPIO%d 핀 연결 완료",
             MODBUS_TX_PIN, MODBUS_RX_PIN);

    // ── ③ 드라이버 설치 ─────────────────────────────────
    // TRM 19.3.3 UART RAM:
    //   3개 UART가 1024×8bit RAM 공유
    //   UART2 기본 Rx/Tx FIFO = 128바이트씩
    //   여기서 256바이트로 Rx 버퍼 확장
    // Tx 버퍼 = 0 → 송신 시 블로킹 방식 (Modbus 요청은 짧으므로 적합)
    ESP_ERROR_CHECK(uart_driver_install(
        MODBUS_UART_NUM,
        MODBUS_RX_BUF_SIZE,  // Rx 버퍼 256바이트
        0,                   // Tx 버퍼 0 → 블로킹 송신
        0, NULL, 0           // 이벤트 큐 미사용 
    ));
    ESP_LOGI(TAG, "UART 드라이버 설치 완료 (Rx 버퍼: %d bytes)",
             MODBUS_RX_BUF_SIZE);

    // ── ④ RS-485 Half-Duplex 모드 설정 ──────────────────
    // uart_set_mode() 내부 동작:
    //   UART_RS485_CONF_REG Bit[0] UART_RS485_EN      = 1
    //   UART_RS485_CONF_REG Bit[3] UART_DL1_EN        = 1 (STOP 딜레이)
    //   UART_RS485_CONF_REG Bit[4] UART_RS485TX_RX_EN = 0 (송신 중 수신 차단)
    ESP_ERROR_CHECK(uart_set_mode(
        MODBUS_UART_NUM,
        UART_MODE_RS485_HALF_DUPLEX
    ));
    ESP_LOGI(TAG, "RS-485 Half-Duplex 모드 설정 완료");

    ESP_LOGI(TAG, "UART2 초기화 완료");
    return ESP_OK;

}



// ── CRC16 계산 ─────────────────────────────────────────
// Modbus RTU 표준 CRC16 (다항식 0xA001)
static uint16_t modbus_crc16(const uint8_t *data, uint16_t length)
{
    uint16_t crc = 0xFFFF;  // 초기값

    for (uint16_t i = 0; i < length; i++) {
        crc ^= data[i];                    // XOR
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001; // LSB가 1이면 다항식 XOR
            } else {
                crc >>= 1;                 // LSB가 0이면 시프트만
            }
        }
    }
    return crc;
}

// ── FC03 요청 송신 ──────────────────────────────────────
esp_err_t modbus_send_fc03(uint8_t slave_addr, uint16_t reg_addr, uint16_t reg_count)
{
    // 프레임 구성 (CRC 제외 6바이트)
    uint8_t frame[8];
    frame[0] = slave_addr;          // 슬레이브 주소
    frame[1] = 0x03;                // FC03
    frame[2] = (reg_addr >> 8) & 0xFF;  // 시작 주소 상위
    frame[3] = reg_addr & 0xFF;         // 시작 주소 하위
    frame[4] = (reg_count >> 8) & 0xFF; // 수량 상위
    frame[5] = reg_count & 0xFF;        // 수량 하위

    // CRC 계산 후 추가 (리틀 엔디안: Low 먼저)
    uint16_t crc = modbus_crc16(frame, 6);
    frame[6] = crc & 0xFF;          // CRC Low
    frame[7] = (crc >> 8) & 0xFF;  // CRC High

    // 송신 로그
    ESP_LOGI(TAG, "FC03 송신: [%02X][%02X][%02X][%02X][%02X][%02X][%02X][%02X]",
             frame[0], frame[1], frame[2], frame[3],
             frame[4], frame[5], frame[6], frame[7]);

    // UART2로 송신
    // uart_write_bytes() 내부:
    //   데이터 → Tx_FIFO_REG (0x3FF6E000) 에 순서대로 기록
    //   RS-485 Half-Duplex → 송신 완료 후 자동 수신 모드 전환
    int bytes_sent = uart_write_bytes(MODBUS_UART_NUM,
                                       (const char *)frame, 8);
    if (bytes_sent != 8) {
        ESP_LOGE(TAG, "송신 실패: %d/8 bytes", bytes_sent);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "FC03 송신 완료 (%d bytes)", bytes_sent);
    return ESP_OK;
}

esp_err_t modbus_send_fc04(uint8_t slave_addr, uint16_t reg_addr, uint16_t reg_count)
{
    uint8_t frame[8];
    frame[0] = slave_addr;
    frame[1] = 0x04;                    // FC04로 변경
    frame[2] = (reg_addr >> 8) & 0xFF;
    frame[3] = reg_addr & 0xFF;
    frame[4] = (reg_count >> 8) & 0xFF;
    frame[5] = reg_count & 0xFF;

    uint16_t crc = modbus_crc16(frame, 6);
    frame[6] = crc & 0xFF;
    frame[7] = (crc >> 8) & 0xFF;

    ESP_LOGI(TAG, "FC04 송신: [%02X][%02X][%02X][%02X][%02X][%02X][%02X][%02X]",
             frame[0], frame[1], frame[2], frame[3],
             frame[4], frame[5], frame[6], frame[7]);

    int bytes_sent = uart_write_bytes(MODBUS_UART_NUM,
                                       (const char *)frame, 8);
    if (bytes_sent != 8) {
        ESP_LOGE(TAG, "송신 실패: %d/8 bytes", bytes_sent);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "FC04 송신 완료 (%d bytes)", bytes_sent);  // 추가
    
    return ESP_OK;
}

esp_err_t modbus_recv_fc04(uint8_t *data_out, uint16_t *data_len)
{
    uint8_t rx_buf[32] = {0};

    int bytes_read = uart_read_bytes(MODBUS_UART_NUM,
                                      rx_buf,
                                      sizeof(rx_buf),
                                      pdMS_TO_TICKS(100));
    if (bytes_read <= 0) {
        ESP_LOGW(TAG, "수신 타임아웃");
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "FC04 수신 (%d bytes): [%02X][%02X][%02X][%02X][%02X][%02X][%02X]",
             bytes_read,
             rx_buf[0], rx_buf[1], rx_buf[2],
             rx_buf[3], rx_buf[4], rx_buf[5], rx_buf[6]);

    // CRC 검증
    uint16_t crc_received = (rx_buf[bytes_read-1] << 8) | rx_buf[bytes_read-2];
    uint16_t crc_calc     = modbus_crc16(rx_buf, bytes_read - 2);

    if (crc_received != crc_calc) {
        ESP_LOGE(TAG, "CRC 불일치: 수신=0x%04X 계산=0x%04X",
                 crc_received, crc_calc);
        return ESP_FAIL;
    }

    *data_len = rx_buf[2];
    memcpy(data_out, &rx_buf[3], *data_len);
    ESP_LOGI(TAG, "FC04 수신 완료 (%d bytes)", bytes_read);
    return ESP_OK;
}

esp_err_t modbus_send_fc06(uint8_t slave_addr, uint16_t reg_addr, uint16_t value)
{
    uint8_t frame[8];
    frame[0] = slave_addr;
    frame[1] = 0x06;                      // Function Code
    frame[2] = (reg_addr >> 8) & 0xFF;    // Reg Addr High
    frame[3] = reg_addr & 0xFF;           // Reg Addr Low
    frame[4] = (value >> 8) & 0xFF;       // Value High
    frame[5] = value & 0xFF;              // Value Low

    uint16_t crc = modbus_crc16(frame, 6);
    frame[6] = crc & 0xFF;                // CRC Low  ← Modbus는 Low 먼저
    frame[7] = (crc >> 8) & 0xFF;         // CRC High

    // RX FIFO 비우기 (이전 수신 잔여 데이터 제거)
    uart_flush_input(UART_NUM_2);

    int sent = uart_write_bytes(UART_NUM_2, frame, 8);
    if (sent != 8) {
        ESP_LOGE("MODBUS", "FC06 send failed: %d bytes", sent);
        return ESP_FAIL;
    }

    ESP_LOGI("MODBUS", "FC06 TX: [%02X %02X %02X %02X %02X %02X %02X %02X]",
             frame[0], frame[1], frame[2], frame[3],
             frame[4], frame[5], frame[6], frame[7]);
    return ESP_OK;
}

esp_err_t modbus_recv_fc06(uint8_t *data_out, uint16_t *data_len)
{
    // FC06 정상 응답은 Query Echo → 8바이트
    // 슬레이브 에러 응답은 5바이트 (ID + FC|0x80 + Exception Code + CRC2)
    vTaskDelay(pdMS_TO_TICKS(50));  // 슬레이브 처리 대기 (9600baud → 8byte ≈ 8ms, 여유분)

    int len = uart_read_bytes(UART_NUM_2, data_out, 16, pdMS_TO_TICKS(100));
    if (len <= 0) {
        ESP_LOGE("MODBUS", "FC06 recv timeout");
        return ESP_ERR_TIMEOUT;
    }
    *data_len = (uint16_t)len;

    // Echo 검증 (정상이면 첫 2바이트: slave_addr + 0x06)
    if (data_out[1] == 0x86) {  // 0x06 | 0x80 = Exception
        ESP_LOGE("MODBUS", "FC06 Exception Code: 0x%02X", data_out[2]);
        return ESP_FAIL;
    }

    ESP_LOGI("MODBUS", "FC06 RX (%d bytes): [%02X %02X %02X %02X %02X %02X %02X %02X]",
             len, data_out[0], data_out[1], data_out[2], data_out[3],
             data_out[4], data_out[5], data_out[6], data_out[7]);
    return ESP_OK;
}

esp_err_t modbus_read_sensor(sensor_data_t *sensor)
{
    uint8_t  data[16] = {0};
    uint16_t data_len = 0;

    ESP_ERROR_CHECK(modbus_send_fc04(0x01, 0x0001, 0x0002));

      
    uart_wait_tx_done(MODBUS_UART_NUM, pdMS_TO_TICKS(100));

    esp_err_t ret = modbus_recv_fc04(data, &data_len);
    if (ret != ESP_OK) return ret;

    sensor->temperature = ((data[0] << 8) | data[1]) / 10.0f;
    sensor->humidity    = ((data[2] << 8) | data[3]) / 10.0f;

    ESP_LOGI(TAG, "온도: %.1f℃  습도: %.1f%%",
             sensor->temperature, sensor->humidity);
    return ESP_OK;
}

esp_err_t modbus_send_fc16(uint8_t slave_addr, uint16_t reg_addr, uint16_t reg_count, uint16_t *values)
{
    // 프레임 크기: 헤더7 + 데이터(reg_count×2) + CRC2
    uint8_t byte_count = reg_count * 2;
    uint8_t frame_len  = 7 + byte_count + 2;
    uint8_t frame[32]; // XY-MD02 최대 레지스터 수 감안

    frame[0] = slave_addr;
    frame[1] = 0x10;                       // FC16
    frame[2] = (reg_addr >> 8) & 0xFF;
    frame[3] = reg_addr & 0xFF;
    frame[4] = (reg_count >> 8) & 0xFF;
    frame[5] = reg_count & 0xFF;
    frame[6] = byte_count;                 // 바이트수 필드

    // 데이터 채우기
    for (int i = 0; i < reg_count; i++) {
        frame[7 + i*2]     = (values[i] >> 8) & 0xFF;  // High
        frame[7 + i*2 + 1] = values[i] & 0xFF;         // Low
    }

    uint16_t crc = modbus_crc16(frame, 7 + byte_count);
    frame[7 + byte_count]     = crc & 0xFF;
    frame[7 + byte_count + 1] = (crc >> 8) & 0xFF;

    uart_flush_input(UART_NUM_2);

    int sent = uart_write_bytes(UART_NUM_2, frame, frame_len);
    if (sent != frame_len) {
        ESP_LOGE("MODBUS", "FC16 send failed: %d bytes", sent);
        return ESP_FAIL;
    }

    ESP_LOGI("MODBUS", "FC16 TX (%d bytes):", frame_len);
    ESP_LOG_BUFFER_HEX("MODBUS", frame, frame_len);
    return ESP_OK;
}

esp_err_t modbus_recv_fc16(uint8_t *data_out, uint16_t *data_len)
{
    // 정상 응답 6바이트 고정
    vTaskDelay(pdMS_TO_TICKS(50));

    int len = uart_read_bytes(UART_NUM_2, data_out, 16, pdMS_TO_TICKS(100));
    if (len <= 0) {
        ESP_LOGE("MODBUS", "FC16 recv timeout");
        return ESP_ERR_TIMEOUT;
    }
    *data_len = (uint16_t)len;

    if (data_out[1] == 0x90) {  // 0x10 | 0x80 = Exception
        ESP_LOGE("MODBUS", "FC16 Exception: 0x%02X", data_out[2]);
        return ESP_FAIL;
    }

    ESP_LOGI("MODBUS", "FC16 RX (%d bytes):", len);
    ESP_LOG_BUFFER_HEX("MODBUS", data_out, len);
    return ESP_OK;
}

void modbus_scan_slaves(void)
{
    ESP_LOGI(TAG, "슬레이브 스캔 시작 (1~247)...");

    for (uint8_t id = 1; id <= 247; id++) {
        modbus_send_fc04(id, 0x0001, 0x0001);
        uart_wait_tx_done(MODBUS_UART_NUM, pdMS_TO_TICKS(50));

        uint8_t  rx_buf[32] = {0};
        int bytes_read = uart_read_bytes(MODBUS_UART_NUM,
                                          rx_buf,
                                          sizeof(rx_buf),
                                          pdMS_TO_TICKS(100));
        if (bytes_read > 0) {
            ESP_LOGI(TAG, "슬레이브 발견! ID: %d (0x%02X)", id, id);
            ESP_LOGI(TAG, "응답: [%02X][%02X][%02X][%02X][%02X][%02X][%02X]",
                     rx_buf[0], rx_buf[1], rx_buf[2],
                     rx_buf[3], rx_buf[4], rx_buf[5], rx_buf[6]);
            break;  // 찾으면 중단
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}