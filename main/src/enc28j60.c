#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lwip/ip4_addr.h"
#include "enc28j60.h"


static const char *TAG = "ENC28J60";
static spi_device_handle_t spi;

// MII 인터페이스 레지스터 (Bank 2)
#define MIREGADR  0x14
#define MIWRL     0x16
#define MIWRH     0x17

// MII 명령/데이터 레지스터 (Bank 2)
#define MICMD         0x12
#define MIRDL         0x18
#define MIRDH         0x19
// MII 상태 레지스터 (Bank 3, MAC 레지스터 - dummy byte 필요)
#define MISTAT        0x0A
#define MICMD_MIIRD   0x01
#define MISTAT_BUSY   0x01

// PHY 레지스터 주소
#define PHCON2        0x10
#define PHCON2_HDLDIS 0x0100
#define PHSTAT1       0x01
#define PHSTAT1_LLSTAT 0x0004  // bit2: Latching Link Status

static uint16_t next_pkt_ptr = RXSTART_INIT;
static SemaphoreHandle_t s_rx_sem = NULL;

static volatile uint32_t s_isr_count = 0;

static void IRAM_ATTR enc28j60_isr_handler(void *arg)
{
    s_isr_count++;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(s_rx_sem, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}
;
static SemaphoreHandle_t s_spi_mutex = NULL;
static esp_err_t         s_init_result = ESP_FAIL;

// ─── Bank 선택 ─────────────────────────────────
static void enc28j60_set_bank(uint8_t bank)
{
    enc28j60_bfc(ECON1, 0x03);
    enc28j60_bfs(ECON1, bank & 0x03);
}

// ─── MAC 레지스터 읽기 ────────────────────────────
// 이 칩은 MAC 레지스터 읽기 시 dummy byte 없이 data가 rx[1]에 바로 옴
static uint8_t enc28j60_rcr_mac(uint8_t addr)
{
    uint8_t tx[3] = { ENC_RCR | addr, 0x00, 0x00 };
    uint8_t rx[3] = { 0x00, 0x00, 0x00 };

    spi_transaction_t t = {
        .length    = 24,   // 3바이트
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    spi_device_polling_transmit(spi, &t);
    return rx[2];  // rx[1]=dummy, rx[2]=실제 데이터
}

// ─── PHY 레지스터 쓰기 (MII, 데이터시트 Section 3.3.3) ──────────
static void enc28j60_phy_write(uint8_t addr, uint16_t data)
{
    enc28j60_set_bank(2);
    enc28j60_wcr(MIREGADR, addr);
    enc28j60_wcr(MIWRL, data & 0xFF);
    enc28j60_wcr(MIWRH, data >> 8);
    vTaskDelay(pdMS_TO_TICKS(15));  // MII 완료 최소 10.24µs, 여유있게 15ms
}
/*
static void enc28j60_phy_write(uint8_t addr, uint16_t data)
{
    enc28j60_set_bank(2);
    enc28j60_wcr(MIREGADR, addr);
    enc28j60_wcr(MIWRL, data & 0xFF);
    enc28j60_wcr(MIWRH, data >> 8);   // MIWRH 쓰기가 MII write 시작
    vTaskDelay(pdMS_TO_TICKS(1));
    enc28j60_set_bank(3);
    while (enc28j60_rcr_mac(MISTAT) & 0x01) {  // BUSY bit 대기
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

*/
// ─── PHY 레지스터 읽기 (MII, 데이터시트 Section 3.3.3) ──────────
static uint16_t enc28j60_phy_read(uint8_t addr)
{
    enc28j60_set_bank(2);
    enc28j60_wcr(MIREGADR, addr);
    enc28j60_wcr(MICMD, 0x01);
    uint8_t miregadr_rb = enc28j60_rcr_mac(MIREGADR);
    uint8_t micmd_rb    = enc28j60_rcr_mac(MICMD);
    ESP_LOGD(TAG, "[MII] MIREGADR=0x%02X(exp:0x%02X) MICMD=0x%02X(exp:0x01)",
             miregadr_rb, addr, micmd_rb);
    vTaskDelay(pdMS_TO_TICKS(15));  // MISTAT 체크 대신 고정 대기
    enc28j60_set_bank(2);           // MIRDL/MIRDH는 Bank2
    enc28j60_wcr(MICMD, 0x00);
    uint16_t lo = enc28j60_rcr_mac(MIRDL);
    uint16_t hi = enc28j60_rcr_mac(MIRDH);
    return (hi << 8) | lo;
}
/*
static uint16_t enc28j60_phy_read(uint8_t addr)
{
    enc28j60_set_bank(2);
    enc28j60_wcr(MIREGADR, addr);
    enc28j60_wcr(MICMD, 0x01);         // MIIRD bit set
    vTaskDelay(pdMS_TO_TICKS(1));
    enc28j60_set_bank(3);
    while (enc28j60_rcr_mac(MISTAT) & 0x01) {  // BUSY bit 대기
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    enc28j60_set_bank(2);
    enc28j60_wcr(MICMD, 0x00);         // MIIRD clear
    uint16_t lo = enc28j60_rcr_mac(MIRDL);
    uint16_t hi = enc28j60_rcr_mac(MIRDH);
    return (hi << 8) | lo;
}
*/

// ─── 버퍼 읽기 (RBM) ──────────────────────────
static void enc28j60_rbm(uint8_t *buf, uint16_t len)
{
    // opcode(1) + data(len) 단일 트랜잭션 → CS 유지
    uint8_t *tx = calloc(len + 1, 1);
    uint8_t *rx = calloc(len + 1, 1);

    tx[0] = ENC_RBM;

    spi_transaction_t t = {
        .length    = (len + 1) * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    spi_device_polling_transmit(spi, &t);
    memcpy(buf, rx + 1, len);  // rx[0]=garbage(opcode echo)

    free(tx);
    free(rx);
}

// ─── 버퍼 쓰기 (WBM) ──────────────────────────
static void enc28j60_wbm(const uint8_t *buf, uint16_t len)
{
    uint8_t *tx = malloc(len + 1);

    tx[0] = ENC_WBM;
    memcpy(tx + 1, buf, len);

    spi_transaction_t t = {
        .length    = (len + 1) * 8,
        .tx_buffer = tx,
    };
    spi_device_polling_transmit(spi, &t);
    free(tx);
}

// ─── SPI + HW 초기화 ──────────────────────────
esp_err_t enc28j60_init(void)
{
    // ── RST 토글 ───────────────────────
    gpio_reset_pin(ENC_RST_PIN);
    gpio_set_direction(ENC_RST_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(ENC_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(1));
    gpio_set_level(ENC_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(10));  // 클럭 안정화 대기

    // ── SPI 버스 초기화 ────────────────
    spi_bus_config_t bus_cfg = {
        .mosi_io_num   = ENC_MOSI_PIN,
        .miso_io_num   = ENC_MISO_PIN,
        .sclk_io_num   = ENC_SCK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev_cfg = {
        //.clock_speed_hz = 1 * 1000 * 1000,
        .clock_speed_hz = 8 * 1000 * 1000,
        .mode           = 0,
        .spics_io_num   = ENC_CS_PIN,
        .queue_size     = 1,
        .cs_ena_posttrans = 2,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev_cfg, &spi));
    gpio_set_pull_mode(ENC_MISO_PIN, GPIO_PULLUP_ONLY);  // ← 추가
    s_spi_mutex = xSemaphoreCreateMutex();
    if (!s_spi_mutex) return ESP_ERR_NO_MEM;

    // ── ESTAT.CLKRDY 대기 ──────────────
    // MAC/MII/PHY 레지스터 접근 전 oscillator 안정화 필요
    {
        uint32_t t = 0;
        while (!(enc28j60_rcr(ESTAT) & ESTAT_CLKRDY)) {
            if (++t > 100) { ESP_LOGE(TAG, "CLKRDY timeout"); return ESP_FAIL; }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        ESP_LOGI(TAG, "CLKRDY ready (%lums)", (unsigned long)t);
    }

    // ── EREVID 검증 ────────────────────
    enc28j60_set_bank(3);
    uint8_t rev = enc28j60_rcr(EREVID);
    ESP_LOGI(TAG, "EREVID = 0x%02X (기대값: 0x06)", rev);
    if (rev != 0x06) {
        ESP_LOGE(TAG, "SPI 통신 실패");
        return ESP_FAIL;
    }

    // ─────────────────────────────────────────────
    // HW 초기화: RX/TX 버퍼 → MAC → RX 활성화
    // ─────────────────────────────────────────────

    // ── Bank 0: RX/TX 버퍼 경계 설정 (ETH 레지스터, WCR 사용) ──
    enc28j60_set_bank(0);
    next_pkt_ptr = RXSTART_INIT;
    enc28j60_wcr(ERXSTL,   RXSTART_INIT & 0xFF);
    enc28j60_wcr(ERXSTH,   RXSTART_INIT >> 8);
    enc28j60_wcr(ERXNDL,   RXSTOP_INIT  & 0xFF);
    enc28j60_wcr(ERXNDH,   RXSTOP_INIT  >> 8);
    enc28j60_wcr(ERXRDPTL, RXSTOP_INIT & 0xFF);   // 에라타: 홀수 필수
    enc28j60_wcr(ERXRDPTH, RXSTOP_INIT >> 8);
    enc28j60_wcr(ETXSTL,   TXSTART_INIT & 0xFF);
    enc28j60_wcr(ETXSTH,   TXSTART_INIT >> 8);

    // ── Bank 2: MAC 설정 (WCR: MAC 레지스터 쓰기는 WCR만 동작) ──
    enc28j60_set_bank(2);
    
    uint8_t econ1_check = enc28j60_rcr(ECON1);
    ESP_LOGD(TAG, "[DBG] set_bank(2) 후 ECON1=0x%02X (bits[1:0] 기대값: 0x02)", econ1_check);

    enc28j60_wcr(MACON1, 0xAB);
    uint8_t tx3[3] = {ENC_RCR | MACON1, 0, 0};
    uint8_t rx3[3] = {0};
    spi_transaction_t t3 = { .length=24, .tx_buffer=tx3, .rx_buffer=rx3 };
    spi_device_polling_transmit(spi, &t3);
    ESP_LOGD(TAG, "[DBG] MACON1 write 0xAB → rx=[%02X,%02X,%02X]", rx3[0], rx3[1], rx3[2]);
    // ▲▲▲ 진단 코드 끝 ▲▲▲
    enc28j60_wcr(MACON3, 0x00);   // 클리어 먼저
    enc28j60_wcr(MACON1,  MACON1_MARXEN);
     {
        uint8_t tx[3] = {ENC_RCR | MACON1, 0, 0};
        uint8_t rx[3] = {0};
        spi_transaction_t t = {.length=24, .tx_buffer=tx, .rx_buffer=rx};
        spi_device_polling_transmit(spi, &t);
        ESP_LOGD(TAG, "[raw] MACON1 → [%02X,%02X,%02X]", rx[0],rx[1],rx[2]);
    }

    enc28j60_wcr(MACON3,  MACON3_PADCFG0 | MACON3_TXCRCEN | MACON3_FRMLNEN);
    {
        uint8_t tx[3] = {ENC_RCR | MACON3, 0, 0};
        uint8_t rx[3] = {0};
        spi_transaction_t t = {.length=24, .tx_buffer=tx, .rx_buffer=rx};
        spi_device_polling_transmit(spi, &t);
        ESP_LOGD(TAG, "[raw] MACON3 → [%02X,%02X,%02X]", rx[0],rx[1],rx[2]);
    }
    enc28j60_wcr(MABBIPG, 0x12);
    enc28j60_wcr(MAIPGL,  0xC2);
    enc28j60_wcr(MAIPGH,  0x0C);
    enc28j60_wcr(MAMXFLL, 0xEE);
    enc28j60_wcr(MAMXFLH, 0x05);

    // 쓰기 확인 (rcr_mac: rx[1]=data, 이 칩은 dummy byte 없음)
    uint8_t m1 = enc28j60_rcr_mac(MACON1);
    uint8_t m3 = enc28j60_rcr_mac(MACON3);
    ESP_LOGD(TAG, "MAC 쓰기 확인: MACON1=0x%02X(expect 0x01) MACON3=0x%02X(expect 0x32)", m1, m3);

    // ── Bank 3: MAC 주소 설정 (eFuse 기반) ──
    // MAADR1(0x04)=chip_mac[0](MSB) ... MAADR6(0x01)=chip_mac[5](LSB)
    uint8_t chip_mac[6];
    ESP_ERROR_CHECK(esp_efuse_mac_get_default(chip_mac));
    chip_mac[0] |= 0x02;
    chip_mac[0] &= ~0x01;
    enc28j60_set_bank(3);
    enc28j60_wcr(MAADR1, chip_mac[0]);
    enc28j60_wcr(MAADR2, chip_mac[1]);
    enc28j60_wcr(MAADR3, chip_mac[2]);
    enc28j60_wcr(MAADR4, chip_mac[3]);
    enc28j60_wcr(MAADR5, chip_mac[4]);
    enc28j60_wcr(MAADR6, chip_mac[5]);

    // ── PHY 초기화 (데이터시트 Section 6.6) ──────────
    enc28j60_phy_write(PHCON2, 0x0100);  // HDLDIS=1: Half-Duplex Loopback Disable
    uint16_t phid1   = enc28j60_phy_read(0x02);
    uint16_t phstat1 = enc28j60_phy_read(0x01);
    ESP_LOGD(TAG, "PHID1=0x%04X(expect 0x0083) PHSTAT1=0x%04X", phid1, phstat1);

    // ── RX 활성화 ──────────────────────
    enc28j60_bfs(ECON1, ECON1_RXEN);

    // RX 활성화 후 인터럽트 설정
   enc28j60_bfs(ECON1, ECON1_RXEN);

   // EIE: PKTIE(패킷 수신) + INTIE(INT 핀 출력) 활성화
   enc28j60_bfs(EIE, EIE_PKTIE | EIE_INTIE);

   s_rx_sem = xSemaphoreCreateBinary();

   gpio_config_t io_cfg = {
    .pin_bit_mask = (1ULL << ENC_INT_PIN),   // GPIO35
    .mode         = GPIO_MODE_INPUT,
    .pull_up_en   = GPIO_PULLUP_DISABLE,       // INT는 active-low, idle=HIGH
    .intr_type    = GPIO_INTR_NEGEDGE,        // LOW로 떨어지는 순간
   };
   gpio_config(&io_cfg);
   gpio_install_isr_service(0);
   gpio_isr_handler_add(ENC_INT_PIN, enc28j60_isr_handler, NULL);


    ESP_LOGI(TAG, "HW 초기화 완료 (MAC: %02X:%02X:%02X:%02X:%02X:%02X)",
             chip_mac[0], chip_mac[1], chip_mac[2],
             chip_mac[3], chip_mac[4], chip_mac[5]);
    return ESP_OK;
}

// ─── 패킷 송신 ────────────────────────────────
esp_err_t enc28j60_send_packet(const uint8_t *buf, uint16_t len)
{
    if (xSemaphoreTake(s_spi_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) { //  100 -> 1000 변경
        ESP_LOGE(TAG, "TX mutex timeout");
        return ESP_ERR_TIMEOUT;
    }

    // TXRTS 클리어 대기 (이전 전송 완료 확인)
    uint32_t timeout = 1000;
    while ((enc28j60_rcr(ECON1) & ECON1_TXRTS) && timeout--) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (timeout == 0) {
        ESP_LOGE(TAG, "TX timeout");
        xSemaphoreGive(s_spi_mutex);
        return ESP_ERR_TIMEOUT;
    }

    enc28j60_set_bank(0);

    // EWRPT = TXSTART (쓰기 포인터 설정)
    enc28j60_wcr(EWRPTL, TXSTART_INIT & 0xFF);
    enc28j60_wcr(EWRPTH, TXSTART_INIT >> 8);

    // Per-packet control byte: 0x00 = MACON3 설정 그대로 사용
    uint8_t ctrl = 0x00;
    enc28j60_wbm(&ctrl, 1);

    // 패킷 데이터 기록
    enc28j60_wbm(buf, len);

    // ETXND = TXSTART + 1(ctrl byte) + len - 1
    uint16_t tx_end = TXSTART_INIT + len;
    enc28j60_wcr(ETXNDL, tx_end & 0xFF);
    enc28j60_wcr(ETXNDH, tx_end >> 8);

    // TXRTS SET → 전송 시작
    enc28j60_bfs(ECON1, ECON1_TXRTS);

    ESP_LOGD(TAG, "TX %d bytes", len);
    xSemaphoreGive(s_spi_mutex);
    return ESP_OK;
}

// ─── 패킷 수신 ────────────────────────────────
// 반환값: 수신 길이 (0 = 수신 없음, <0 = 오류)
int enc28j60_recv_packet(uint8_t *buf, uint16_t max_len)
{
    if (xSemaphoreTake(s_spi_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) { //100 -> 1000 수정
        return 0;
    }

    enc28j60_set_bank(1);
    uint8_t pkt_cnt = enc28j60_rcr(EPKTCNT);
    if (pkt_cnt == 0) {
        xSemaphoreGive(s_spi_mutex);
        return 0;
    }

    enc28j60_set_bank(0);

    // ERDPT = next_pkt_ptr (읽기 포인터 설정)
    enc28j60_wcr(ERDPTL, next_pkt_ptr & 0xFF);
    enc28j60_wcr(ERDPTH, next_pkt_ptr >> 8);

    // RSV(Receive Status Vector) 구조
    // [0~1]: Next Packet Pointer (Little-Endian)
    // [2~3]: Byte Count
    // [4]  : Status bits (bit0 = Received OK)
    // [5]  : 상위 Status bits
    uint8_t rsv[6];
    enc28j60_rbm(rsv, 6);

    uint16_t next_ptr   = rsv[0] | (rsv[1] << 8);
    uint16_t pkt_len    = rsv[2] | (rsv[3] << 8);
    uint8_t  rx_ok      = rsv[4] & 0x80;  // bit7 = Received OK

    if (!rx_ok || pkt_len > max_len) {
        ESP_LOGW(TAG, "RX 오류: ok=%d len=%d", rx_ok, pkt_len);
        goto update_ptr;
    }

    // 실제 데이터 읽기 (CRC 4바이트 제외)
    uint16_t data_len = pkt_len - 4;
    enc28j60_rbm(buf, data_len);
    ESP_LOGD(TAG, "RX %d bytes", data_len);
    ESP_LOGD(TAG, "[RSV] rsv=[%02X %02X %02X %02X %02X %02X] next=0x%04X len=%d ok=%d",
            rsv[0], rsv[1], rsv[2], rsv[3], rsv[4], rsv[5],
         next_ptr, pkt_len, rx_ok);

update_ptr:
    // ERXRDPT 업데이트: 실리콘 에라타 → 홀수값 필수
    uint16_t erxrdpt;
    if (next_ptr == RXSTART_INIT) {
        erxrdpt = RXSTOP_INIT;          // 홀수 (0x19FF)
    } else {
        erxrdpt = next_ptr - 1;         // next_ptr은 항상 짝수 → -1 = 홀수
    }
    enc28j60_wcr(ERXRDPTL, erxrdpt & 0xFF);
    enc28j60_wcr(ERXRDPTH, erxrdpt >> 8);

    next_pkt_ptr = next_ptr;

    // PKTDEC: EPKTCNT 감소
    enc28j60_bfs(ECON2, ECON2_PKTDEC);

    xSemaphoreGive(s_spi_mutex);
    return (!rx_ok || pkt_len > max_len) ? -1 : (int)(pkt_len - 4);
}

// ─── Read Control Register ────────────────────
uint8_t enc28j60_rcr(uint8_t addr)
{
    uint8_t tx[2] = { ENC_RCR | addr, 0x00 };
    uint8_t rx[2] = { 0x00, 0x00 };

    spi_transaction_t t = {
        .length    = 16,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    spi_device_polling_transmit(spi, &t);
    return rx[1];
}

// ─── Write Control Register ───────────────────
void enc28j60_wcr(uint8_t addr, uint8_t data)
{
    uint8_t tx[2] = { ENC_WCR | addr, data };

    spi_transaction_t t = {
        .length    = 16,
        .tx_buffer = tx,
    };
    spi_device_polling_transmit(spi, &t);
}

// ─── Bit Field Set ────────────────────────────
void enc28j60_bfs(uint8_t addr, uint8_t mask)
{
    uint8_t tx[2] = { ENC_BFS | addr, mask };

    spi_transaction_t t = {
        .length    = 16,
        .tx_buffer = tx,
    };
    spi_device_polling_transmit(spi, &t);
}

// ─── Bit Field Clear ──────────────────────────
void enc28j60_bfc(uint8_t addr, uint8_t mask)
{
    uint8_t tx[2] = { ENC_BFC | addr, mask };

    spi_transaction_t t = {
        .length    = 16,
        .tx_buffer = tx,
    };
    spi_device_polling_transmit(spi, &t);
}

// ─── netif 드라이버 구조체 ─────────────────────
// base가 반드시 첫 번째 멤버여야 함
// → esp_netif_attach() 내부에서 base*로 캐스팅하기 때문
typedef struct {
    esp_netif_driver_base_t base;
} enc28j60_netif_driver_t;

static esp_netif_t      *s_netif   = NULL;
static TaskHandle_t      s_rx_task = NULL;


// ─── TX 콜백: lwIP → enc28j60 ─────────────────
// lwIP이 패킷 송신 시 esp_netif가 이 함수를 호출
static esp_err_t enc28j60_netif_transmit(void *h, void *buffer, size_t len)
{
    ESP_LOGD(TAG, "netif_transmit len=%d", len);
    return enc28j60_send_packet((uint8_t *)buffer, (uint16_t)len);
}

// ─── RX 버퍼 해제 콜백 ────────────────────────
// esp_netif_receive() 처리 완료 후 호출됨
static void enc28j60_free_rx_buffer(void *h, void *buffer)
{
    free(buffer);
}

// ─── DHCP 재시작 워치독 ────────────────────────
static void dhcp_watchdog_task(void *arg)
{
    esp_netif_t *netif = (esp_netif_t *)arg;
    vTaskDelay(pdMS_TO_TICKS(10000));  // 10초 대기
    
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(netif, &ip_info);
    if (ip_info.ip.addr == 0) {
        ESP_LOGW(TAG, "DHCP 10초 실패 → 재시작");
        esp_restart();  // 소프트 리셋
    } else {
        vTaskDelete(NULL);
    }
}
/* 이전 워치독 함수
static void dhcp_watchdog_task(void *arg)
{
    esp_netif_t *netif = (esp_netif_t *)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        esp_netif_ip_info_t ip_info;
        esp_netif_get_ip_info(netif, &ip_info);
        if (ip_info.ip.addr == 0) {
            ESP_LOGW(TAG, "DHCP watchdog: DHCP 재시작");
            esp_netif_dhcpc_stop(netif);
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_netif_dhcpc_start(netif);
        } else {
            vTaskDelete(NULL);
        }
    }
}
*/
// ─── post_attach 콜백 ─────────────────────────
// esp_netif_attach() 호출 시 esp_netif가 이 함수를 호출
// → TX/RX 함수 포인터를 netif에 등록하는 시점
static esp_err_t enc28j60_post_attach(esp_netif_t *netif, void *args)
{
    enc28j60_netif_driver_t *driver = (enc28j60_netif_driver_t *)args;
    driver->base.netif = netif;  // netif 역참조 저장

    esp_netif_driver_ifconfig_t driver_ifconfig = {
        .handle                = driver,
        .transmit              = enc28j60_netif_transmit,
        .driver_free_rx_buffer = enc28j60_free_rx_buffer,
    };
    ESP_ERROR_CHECK(esp_netif_set_driver_config(netif, &driver_ifconfig));
    return ESP_OK;
}

// ─── RX 폴링 태스크 ───────────────────────────
// ① enc28j60_init() 을 이 태스크(CPU1)에서 호출 → SPI ISR이 CPU1에 등록
// ② init 완료 후 세마포어 신호 → enc28j60_netif_init() 재개
// ③ 이후 5ms 주기 폴링 루프
static void enc28j60_rx_task(void *arg)
{
    SemaphoreHandle_t init_done = (SemaphoreHandle_t)arg;

    s_init_result = enc28j60_init();
    xSemaphoreGive(init_done);

    if (s_init_result != ESP_OK) {
        ESP_LOGE(TAG, "enc28j60_init 실패: %s", esp_err_to_name(s_init_result));
        vTaskDelete(NULL);
        return;
    }

    static uint8_t rx_buf[1518];
    TickType_t diag_tick = xTaskGetTickCount();

    while (1) {
    xSemaphoreTake(s_rx_sem, pdMS_TO_TICKS(10));

    // recv_packet() 내부에서 mutex + EPKTCNT 처리
    // EPKTCNT > 0인 동안 소진
    while (1) {
        int len = enc28j60_recv_packet(rx_buf, sizeof(rx_buf));
        if (len <= 0) break;

        uint8_t *pkt = malloc(len);
        if (pkt) {
            memcpy(pkt, rx_buf, len);
           // ESP_LOGI(TAG, "→ esp_netif_receive len=%d", len);
           esp_netif_receive(s_netif, pkt, len, pkt);  // NULL → pkt
           //esp_netif_receive(s_netif, pkt, len, NULL);
            // ESP_LOGI(TAG, "← esp_netif_receive done");
        }
    }

    // 10초 진단 (기존 diag 코드 그대로)
    if ((xTaskGetTickCount() - diag_tick) >= pdMS_TO_TICKS(10000)) {
        diag_tick = xTaskGetTickCount();
        if (xSemaphoreTake(s_spi_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                uint8_t econ1 = enc28j60_rcr(ECON1);
                enc28j60_set_bank(1);
                uint8_t erxfcon = enc28j60_rcr(0x18);
                uint8_t epktcnt = enc28j60_rcr(EPKTCNT);
                ESP_LOGI(TAG, "[diag] ECON1=0x%02X ERXFCON=0x%02X EPKTCNT=%d",
                         econ1, erxfcon, epktcnt);

                enc28j60_set_bank(2);
                uint8_t e1_check  = enc28j60_rcr(ECON1);
                uint8_t macon1_2b = enc28j60_rcr(MACON1);
                ESP_LOGD(TAG, "[diag] ECON1=0x%02X(bank bits 기대: 0x02) MACON1_2B=0x%02X",
                         e1_check, macon1_2b);

                uint8_t t3[3], r3[3];
                spi_transaction_t st = { .length=24, .tx_buffer=t3, .rx_buffer=r3 };
                t3[0] = ENC_RCR | MACON1; t3[1] = 0; t3[2] = 0;
                memset(r3, 0, 3); spi_device_polling_transmit(spi, &st);
                ESP_LOGD(TAG, "[diag] MACON1(0x00) 3-byte=[0x%02X,0x%02X,0x%02X](expect 0x01)",
                         r3[0], r3[1], r3[2]);
                t3[0] = ENC_RCR | MACON3; t3[1] = 0; t3[2] = 0;
                memset(r3, 0, 3); spi_device_polling_transmit(spi, &st);
                ESP_LOGD(TAG, "[diag] MACON3(0x02) 3-byte=[0x%02X,0x%02X,0x%02X](expect 0x32)",
                         r3[0], r3[1], r3[2]);

                uint16_t phid1  = enc28j60_phy_read(0x02);
                uint16_t phstat = enc28j60_phy_read(PHSTAT1);
                xSemaphoreGive(s_spi_mutex);
                ESP_LOGD(TAG, "[diag] PHID1=0x%04X PHSTAT1=0x%04X Link:%s",
                         phid1, phstat, (phstat & PHSTAT1_LLSTAT) ? "UP" : "DOWN");
                ESP_LOGI(TAG, "[diag] ISR count=%lu", (unsigned long)s_isr_count);
                ESP_LOGI(TAG, "[diag] free heap=%lu", (unsigned long)esp_get_free_heap_size());
            }
    }
    }
}
/* ---- 폴링 방식 RX 테스크 
static void enc28j60_rx_task(void *arg)
{
    SemaphoreHandle_t init_done = (SemaphoreHandle_t)arg;

    s_init_result = enc28j60_init();
    xSemaphoreGive(init_done);

    if (s_init_result != ESP_OK) {
        ESP_LOGE(TAG, "enc28j60_init 실패: %s", esp_err_to_name(s_init_result));
        vTaskDelete(NULL);
        return;
    }

    static uint8_t rx_buf[1518];
    TickType_t diag_tick = xTaskGetTickCount();
    while (1) {
        int len = enc28j60_recv_packet(rx_buf, sizeof(rx_buf));
        if (len > 0) {
            uint8_t *pkt = malloc(len);
            if (pkt) {
                memcpy(pkt, rx_buf, len);
                esp_netif_receive(s_netif, pkt, len, NULL);
            }
        }

        // 10초마다 링크·수신 상태 진단
        if ((xTaskGetTickCount() - diag_tick) >= pdMS_TO_TICKS(10000)) {
            diag_tick = xTaskGetTickCount();
            if (xSemaphoreTake(s_spi_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                // ── ETH 레지스터 확인 (enc28j60_rcr, 2-byte) ──
                // ECON1: 현재 뱅크 + RXEN(bit2) 상태 확인
                uint8_t econ1 = enc28j60_rcr(ECON1);
                // ERXFCON (Bank1, 0x18): 수신 필터 설정, 기본값=0xA1
                enc28j60_set_bank(1);
                uint8_t erxfcon = enc28j60_rcr(0x18);
                uint8_t epktcnt = enc28j60_rcr(EPKTCNT);
                ESP_LOGI(TAG, "[diag] ECON1=0x%02X ERXFCON=0x%02X EPKTCNT=%d",
                         econ1, erxfcon, epktcnt);

                // ── MAC 레지스터 읽기 (3-byte) ──
                //MACON1(Bank2, 0x00) → init에서 0x01로 씀
                //MACON3(Bank2, 0x02) → init에서 0x32로 씀
                enc28j60_set_bank(2);
                // ▼▼▼ 이것만 추가 ▼▼▼
                uint8_t e1_check = enc28j60_rcr(ECON1);
                uint8_t macon1_2b = enc28j60_rcr(MACON1);  // 2바이트 읽기 (ETH 방식)
                ESP_LOGI(TAG, "[diag] ECON1=0x%02X(bank bits 기대: 0x02) MACON1_2B=0x%02X", e1_check, macon1_2b);
                // ▲▲▲

                uint8_t t3[3], r3[3];
                spi_transaction_t st = { .length=24, .tx_buffer=t3, .rx_buffer=r3 };
                t3[0] = ENC_RCR | MACON1; t3[1] = 0; t3[2] = 0;
                memset(r3, 0, 3); spi_device_polling_transmit(spi, &st);
                ESP_LOGI(TAG, "[diag] MACON1(0x00) 3-byte=[0x%02X,0x%02X,0x%02X](expect 0x01)",
                         r3[0], r3[1], r3[2]);
                t3[0] = ENC_RCR | MACON3; t3[1] = 0; t3[2] = 0;
                memset(r3, 0, 3); spi_device_polling_transmit(spi, &st);
                ESP_LOGI(TAG, "[diag] MACON3(0x02) 3-byte=[0x%02X,0x%02X,0x%02X](expect 0x32)",
                         r3[0], r3[1], r3[2]);

                uint16_t phid1  = enc28j60_phy_read(0x02);  // PHID1: 정상=0x0083
                uint16_t phstat = enc28j60_phy_read(PHSTAT1);
                xSemaphoreGive(s_spi_mutex);
                ESP_LOGI(TAG, "[diag] PHID1=0x%04X PHSTAT1=0x%04X Link:%s",
                         phid1, phstat, (phstat & PHSTAT1_LLSTAT) ? "UP" : "DOWN");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(3));
    }
}
*/

esp_netif_t *enc28j60_get_netif(void)
{
    return s_netif;
}

// ─── netif 초기화 진입점 ──────────────────────
esp_err_t enc28j60_netif_init(esp_netif_t **out_netif)
{
    // ── Ethernet 스택 기반 netif 생성 ─────
    esp_netif_inherent_config_t base_cfg = ESP_NETIF_INHERENT_DEFAULT_ETH();
    esp_netif_config_t cfg = {
        .base   = &base_cfg,
        .driver = NULL,
        .stack  = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    s_netif = esp_netif_new(&cfg);
    if (!s_netif) return ESP_FAIL;

    // ── 드라이버 생성 및 바인딩 ───────────
    enc28j60_netif_driver_t *driver = calloc(1, sizeof(enc28j60_netif_driver_t));
    driver->base.post_attach = enc28j60_post_attach;
    ESP_ERROR_CHECK(esp_netif_attach(s_netif, driver));

    // ── MAC 주소 설정 (ESP32 eFuse 기반) ──
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac));
    mac[0] |= 0x02;
    mac[0] &= ~0x01;
    esp_err_t err = esp_netif_set_mac(s_netif, mac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MAC 설정 실패: %s", esp_err_to_name(err));
        return err;
    }

    // ── CPU1에서 SPI 초기화 후 RX 폴링 시작 ──
    // enc28j60_init()을 CPU1 태스크에서 실행해야 SPI ISR이 CPU1에 등록됨.
    // 이후 enc28j60_rx(CPU1)의 SPI 접근이 크로스코어 조율 없이 동작.
    SemaphoreHandle_t init_done = xSemaphoreCreateBinary();
    if (!init_done) return ESP_ERR_NO_MEM;

    xTaskCreatePinnedToCore(enc28j60_rx_task, "enc28j60_rx", 8192,
                            init_done, 10, &s_rx_task, 1);

    xSemaphoreTake(init_done, portMAX_DELAY);
    vSemaphoreDelete(init_done);

    if (s_init_result != ESP_OK) {
        ESP_LOGE(TAG, "ENC28J60 HW 초기화 실패");
        return s_init_result;
    }

    /*  정적 ip 할당
    // ── 정적 IP 설정 ──────────────────────────────
    esp_netif_dhcpc_stop(s_netif);

    esp_netif_ip_info_t ip_info = {};
    IP4_ADDR(&ip_info.ip,      192, 168, 45, 200);
    IP4_ADDR(&ip_info.gw,      192, 168, 45,   1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255,   0);
    esp_netif_set_ip_info(s_netif, &ip_info);
    ESP_LOGI(TAG, "정적 IP 설정: 192.168.45.200");
    // ─────────────────────────────────────────────
        */

        // ── 링크 UP 대기 (PHSTAT1 bit2 = LLSTAT) ──
{
    uint16_t phstat = 0;
    int retry = 0;
    ESP_LOGI(TAG, "PHY 링크 UP 대기 중...");
    while (!(phstat & PHSTAT1_LLSTAT) && retry < 50) {
        if (xSemaphoreTake(s_spi_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            phstat = enc28j60_phy_read(PHSTAT1);
            xSemaphoreGive(s_spi_mutex);
        }
        if (!(phstat & PHSTAT1_LLSTAT)) vTaskDelay(pdMS_TO_TICKS(100));
        retry++;
    }
    ESP_LOGD(TAG, "PHY 링크 %s (PHSTAT1=0x%04X, %dms)",
             (phstat & PHSTAT1_LLSTAT) ? "UP" : "TIMEOUT",
             phstat, retry * 100);
}

    // ── 링크 UP 이벤트 ────────────────────
   esp_netif_action_start(s_netif, NULL, 0, NULL);
   esp_netif_action_connected(s_netif, NULL, 0, NULL);
   esp_netif_dhcpc_stop(s_netif);                          // ← start 바로 다음
   vTaskDelay(pdMS_TO_TICKS(2000));
   esp_netif_dhcpc_start(s_netif);

    xTaskCreate(dhcp_watchdog_task, "dhcp_wdog", 2048, s_netif, 5, NULL);

    if (out_netif) *out_netif = s_netif;
    ESP_LOGI(TAG, "netif 등록 완료");
    return ESP_OK;
}