#ifndef ENC28J60_H
#define ENC28J60_H

#include "esp_err.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_netif.h"
#include "esp_efuse.h"
#include "esp_mac.h" 

// ── Opcode ──────────────────────────────
#define ENC_RCR  0x00   // Read Control Register
#define ENC_RBM  0x3A   // Read Buffer Memory
#define ENC_WCR  0x40   // Write Control Register
#define ENC_WBM  0x7A   // Write Buffer Memory
#define ENC_BFS  0x80   // Bit Field Set
#define ENC_BFC  0xA0   // Bit Field Clear
#define ENC_SRC  0xFF   // Soft Reset

// ── 핀 정의 ─────────────────────────────
#define ENC_MOSI_PIN  GPIO_NUM_13
#define ENC_MISO_PIN  GPIO_NUM_12
#define ENC_SCK_PIN   GPIO_NUM_14
#define ENC_CS_PIN    GPIO_NUM_15
#define ENC_RST_PIN   GPIO_NUM_4
#define ENC_INT_PIN   GPIO_NUM_35

// ── EIE 비트 정의 (현재 누락) ───────────
#define EIE_INTIE   0x80   // 글로벌 INT 핀 출력 활성화
#define EIE_PKTIE   0x40   // 패킷 수신 인터럽트 활성화
#define EIE_TXIE    0x08   // 송신 완료 인터럽트
#define EIE_RXERIE  0x01   // 수신 오류 인터럽트

// ── EIR 비트 정의 (현재 누락) ───────────
#define EIR_PKTIF   0x40   // 패킷 수신 플래그
#define EIR_RXERIF  0x01   // 수신 오류 플래그

// ── PHSTAT1 비트 정의 (현재 누락) ────────
#define PHSTAT1_LLSTAT  0x0004  // PHY 링크 상태
#define PHCON2   0x10

// ── 버퍼 주소 ───────────────────────────
#define RXSTART_INIT   0x0000
#define RXSTOP_INIT    0x19FF
#define TXSTART_INIT   0x1A00
#define TXSTOP_INIT    0x1FFF

// ── Bank 0 레지스터 ─────────────────────
#define ERDPTL   0x00
#define ERDPTH   0x01
#define EWRPTL   0x02
#define EWRPTH   0x03
#define ETXSTL   0x04
#define ETXSTH   0x05
#define ETXNDL   0x06
#define ETXNDH   0x07
#define ERXSTL   0x08
#define ERXSTH   0x09
#define ERXNDL   0x0A
#define ERXNDH   0x0B
#define ERXRDPTL 0x0C
#define ERXRDPTH 0x0D

// ── Bank 1 레지스터 ─────────────────────
#define EPKTCNT  0x19

// ── Bank 2 레지스터 (MAC) ────────────────
#define MACON1   0x00
#define MACON3   0x02
#define MACON4   0x03
#define MABBIPG  0x04
#define MAIPGL   0x06
#define MAIPGH   0x07
#define MAMXFLL  0x0A
#define MAMXFLH  0x0B

// ── Bank 3 레지스터 ─────────────────────
#define MAADR1   0x04
#define MAADR2   0x05
#define MAADR3   0x02
#define MAADR4   0x03
#define MAADR5   0x00
#define MAADR6   0x01
#define EREVID   0x12

// ── ECON / ESTAT / EIR / EIE ────────────
#define ECON1    0x1F
#define ECON2    0x1E
#define ESTAT    0x1D
#define EIR      0x1C
#define EIE      0x1B

// ── 비트 정의 ───────────────────────────
#define ECON1_RXEN      0x04
#define ECON1_TXRTS     0x08
#define ECON2_AUTOINC   0x80
#define ECON2_PKTDEC    0x40
#define ESTAT_CLKRDY    0x01
#define MACON1_MARXEN   0x01
#define MACON3_FULLDPX  0x01
#define MACON3_FRMLNEN  0x02
#define MACON3_TXCRCEN  0x10
#define MACON3_PADCFG0  0x20

// ── API ─────────────────────────────────
esp_err_t enc28j60_init(void);
uint8_t   enc28j60_rcr(uint8_t addr);
void      enc28j60_wcr(uint8_t addr, uint8_t data);
void      enc28j60_bfs(uint8_t addr, uint8_t mask);
void      enc28j60_bfc(uint8_t addr, uint8_t mask);
esp_err_t enc28j60_send_packet(const uint8_t *buf, uint16_t len);
int       enc28j60_recv_packet(uint8_t *buf, uint16_t max_len);
// enc28j60.h
esp_netif_t *enc28j60_get_netif(void);
// ── netif API ───────────────────────────
esp_err_t enc28j60_netif_init(esp_netif_t **out_netif);

#endif // ENC28J60_H