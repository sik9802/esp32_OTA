#pragma once

#include "esp_err.h"
#include "esp_log.h" 
#include "driver/uart.h"
#include "driver/gpio.h"

#define MODBUS_UART_NUM     UART_NUM_2
#define MODBUS_TX_PIN       GPIO_NUM_17
#define MODBUS_RX_PIN       GPIO_NUM_16
#define MODBUS_BAUD_RATE    9600//19200 9600
#define MODBUS_RX_BUF_SIZE  256


esp_err_t uart_modbus_init(void);
esp_err_t modbus_send_fc03(uint8_t slave_addr, uint16_t reg_addr, uint16_t reg_count);
esp_err_t modbus_send_fc04(uint8_t slave_addr, uint16_t reg_addr, uint16_t reg_count);
esp_err_t modbus_recv_fc04(uint8_t *data_out, uint16_t *data_len);
esp_err_t modbus_send_fc06(uint8_t slave_addr, uint16_t reg_addr, uint16_t value);
esp_err_t modbus_recv_fc06(uint8_t *data_out, uint16_t *data_len);
esp_err_t modbus_send_fc16(uint8_t slave_addr, uint16_t reg_addr, uint16_t reg_count, uint16_t *values);
esp_err_t modbus_recv_fc16(uint8_t *data_out, uint16_t *data_len);

typedef void (*modbus_scan_cb_t)(uint8_t id, bool found, void *user_data);
void modbus_scan_slaves_with_cb(uint8_t start, uint8_t end,
                                  uint16_t timeout_ms,
                                  modbus_scan_cb_t cb,
                                  void *user_data);


typedef struct {
    float temperature;
    float humidity;
} sensor_data_t;

esp_err_t modbus_read_sensor(sensor_data_t *sensor);
void modbus_scan_slaves(void);

