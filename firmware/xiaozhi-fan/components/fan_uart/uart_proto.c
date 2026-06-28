/**
 * UART 协议驱动 — MC32F7073 下位机通讯
 * 独立组件编译, 避免链接顺序影响 AFE
 */
#include "uart_proto.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"

static const char *TAG = "uart";

#define UART_PORT           UART_NUM_1
#define UART_TX_PIN         11
#define UART_RX_PIN         12
#define UART_BUF_SIZE       256
#define TASK_STACK          2048

typedef enum { RX_IDLE, RX_CMD, RX_LEN, RX_DATA } rx_state_t;

static rx_state_t      rx_st = RX_IDLE;
static uint8_t         rx_buf[UPROTO_MAX_FRAME_LEN], rx_pos, rx_cmd, rx_dlen;

static slave_status_t  g_status = {0};
static portMUX_TYPE    g_lock = portMUX_INITIALIZER_UNLOCKED;

static uart_ack_cb_t    cb_ack = NULL;
static uart_nack_cb_t   cb_nack = NULL;
static uart_status_cb_t cb_st = NULL;

static SemaphoreHandle_t ack_sem = NULL;
static SemaphoreHandle_t tx_lock = NULL;
static bool hw_ready = false;

static bool send_frame(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    if (!hw_ready || len > UPROTO_MAX_DATA) return false;
    xSemaphoreTake(tx_lock, portMAX_DELAY);
    uint8_t buf[UPROTO_MAX_FRAME_LEN];
    buf[0] = UPROTO_SOF; buf[1] = cmd; buf[2] = len;
    if (len) memcpy(&buf[3], data, len);
    buf[3 + len] = UPROTO_EOF;
    int n = uart_tx_chars(UART_PORT, (const char *)buf, 4 + len);
    xSemaphoreGive(tx_lock);
    return (n == 4 + len);
}

static void rx_reset(void) { rx_st = RX_IDLE; rx_pos = 0; }

static void rx_byte(uint8_t b)
{
    switch (rx_st) {
    case RX_IDLE:
        if (b == UPROTO_SOF) { rx_st = RX_CMD; rx_pos = 0; rx_buf[rx_pos++] = b; }
        break;
    case RX_CMD: rx_cmd = b; rx_buf[rx_pos++] = b; rx_st = RX_LEN; break;
    case RX_LEN:
        rx_dlen = b; rx_buf[rx_pos++] = b;
        rx_st = (rx_dlen > UPROTO_MAX_DATA) ? (rx_reset(), RX_IDLE) : RX_DATA;
        break;
    case RX_DATA:
        rx_buf[rx_pos++] = b;
        if (rx_pos >= 3 + rx_dlen + 1) {
            if (b == UPROTO_EOF) {
                uint8_t *d = &rx_buf[1];
                switch (d[0]) {
                case CMD_ACK:
                    if (d[1] >= 2) { if (cb_ack) cb_ack(d[2], d[3]); xSemaphoreGive(ack_sem); }
                    break;
                case CMD_NACK:
                    if (d[1] >= 2) { if (cb_nack) cb_nack(d[2], d[3]); xSemaphoreGive(ack_sem); }
                    break;
                case CMD_STATUS_REPORT:
                    if (d[1] >= 4) {
                        taskENTER_CRITICAL(&g_lock);
                        g_status.chg = d[2]; g_status.light = d[3];
                        g_status.angle = d[4]; g_status.fan = d[5];
                        taskEXIT_CRITICAL(&g_lock);
                        if (cb_st) cb_st(&g_status);
                    }
                    break;
                }
            }
            rx_reset();
        }
        break;
    }
}

static void rx_task(void *arg)
{
    uint8_t b;
    while (1) {
        if (uart_read_bytes(UART_PORT, &b, 1, pdMS_TO_TICKS(100)) > 0) rx_byte(b);
    }
}

void uart_proto_init(void)
{
    const uart_config_t cfg = {
        .baud_rate = 9600, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(UART_PORT, &cfg);
    uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    gpio_set_pull_mode(UART_RX_PIN, GPIO_PULLUP_ONLY);
    uart_driver_install(UART_PORT, UART_BUF_SIZE, UART_BUF_SIZE, 0, NULL,
                        ESP_INTR_FLAG_LEVEL1 | ESP_INTR_FLAG_LOWMED);

    tx_lock = xSemaphoreCreateMutex();
    ack_sem = xSemaphoreCreateBinary();
    xTaskCreate(rx_task, "uart_rx", TASK_STACK, NULL, 1, NULL);
    hw_ready = true;
    ESP_LOGI(TAG, "Init GPIO%d(TX) GPIO%d(RX) @9600", UART_TX_PIN, UART_RX_PIN);
}

void uart_proto_set_ack_callback(uart_ack_cb_t cb)       { cb_ack = cb; }
void uart_proto_set_nack_callback(uart_nack_cb_t cb)     { cb_nack = cb; }
void uart_proto_set_status_callback(uart_status_cb_t cb) { cb_st = cb; }
void uart_proto_set_error_callback(uart_error_cb_t cb)   { (void)cb; }

static void queue_cmd(uint8_t cmd, uint8_t data) { send_frame(cmd, &data, 1); }

void uart_proto_set_fan(uint8_t s)    { if (s > PROTO_FAN_MAX) s = PROTO_FAN_MAX; queue_cmd(CMD_SET_FAN, s); }
void uart_proto_set_light(light_mode_t m)  { queue_cmd(CMD_SET_LIGHT, (uint8_t)m); }
void uart_proto_set_angle(angle_mode_t m)  { queue_cmd(CMD_SET_ANGLE, (uint8_t)m); }
void uart_proto_shutdown(void)        { send_frame(CMD_SHUTDOWN, NULL, 0); }
void uart_proto_query_status(void)    { send_frame(CMD_STATUS_QUERY, NULL, 0); }
const slave_status_t *uart_proto_get_status(void) { return &g_status; }
