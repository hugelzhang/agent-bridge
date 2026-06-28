/**
 * Modbus RTU Transport — 工业设备接入 (PLC/电表/变频器/传感器)
 *
 * 每个 AgentBridge device.name 映射为 Modbus 从站地址 (1-247).
 * device.caps 映射为 Modbus 功能码:
 *   ON_OFF   → FC 05 (写单线圈)
 *   LEVEL    → FC 06 (写单寄存器, 0-100 → 0-1000)
 *   SENSOR   → FC 03 (读保持寄存器)
 *
 * 帧格式: 地址(1B) + 功能码(1B) + 数据(N) + CRC16(2B)
 * 物理层: UART + RS485 (RTS 自动方向控制)
 *
 * 依赖: ESP-IDF driver/uart
 */
#include "transport_modbus.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"

static const char *TAG = "transport_modbus";
#define MODBUS_BUF 256

struct transport_modbus {
    agent_bridge_t *bridge; agent_transport_t base;
    int uart_num; int baud; bool running; TaskHandle_t task;
    uint8_t slave_addr;  /* 本设备 Modbus 地址 */
    uint16_t registers[128]; /* 保持寄存器 (0-127) */
};

/* CRC16-Modbus */
static uint16_t crc16(const uint8_t *data, int len) {
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
    return crc;
}

/**
 * 将 AgentBridge 设备能力映射到 Modbus 寄存器布局:
 *   寄存器 0 = 设备电源状态 (0=OFF, 1=ON)
 *   寄存器 1 = 设备等级 (0-1000)
 *   寄存器 2-3 = 传感器值 (温度×10, 湿度×10)
 */

static void modbus_task(void *arg) {
    transport_modbus_t *m = arg;
    uint8_t buf[MODBUS_BUF];
    ESP_LOGI(TAG, "Modbus polling on UART%d @%d", m->uart_num, m->baud);

    while (m->running) {
        int n = uart_read_bytes(m->uart_num, buf, MODBUS_BUF, pdMS_TO_TICKS(100));
        if (n < 8) continue;  /* 最小帧: addr+func+data(2)+crc(2) = 8 */

        /* 校验地址 */
        if (buf[0] != m->slave_addr) continue;

        /* CRC 校验 */
        uint16_t rcv_crc = buf[n-1] | (buf[n-2] << 8);
        if (crc16(buf, n-2) != rcv_crc) continue;

        uint8_t func = buf[1];

        if (func == 0x03 && n >= 8) {
            /* FC 03: 读保持寄存器 → 响应传感器数据 */
            uint16_t start = (buf[2] << 8) | buf[3];
            uint16_t count = (buf[4] << 8) | buf[5];
            if (start + count > 128) count = 128 - start;
            uint8_t resp[MODBUS_BUF];
            resp[0] = m->slave_addr; resp[1] = 0x03;
            resp[2] = count * 2;  /* 字节数 */
            for (int i = 0; i < count; i++) {
                resp[3 + i*2] = m->registers[start+i] >> 8;
                resp[4 + i*2] = m->registers[start+i] & 0xFF;
            }
            int rlen = 3 + count*2;
            uint16_t crc = crc16(resp, rlen);
            resp[rlen] = crc & 0xFF; resp[rlen+1] = crc >> 8;
            uart_write_bytes(m->uart_num, resp, rlen+2);
        }
        else if (func == 0x05 && n >= 8) {
            /* FC 05: 写单线圈 → 执行 on/off */
            uint16_t addr = (buf[2] << 8) | buf[3];
            uint16_t val = (buf[4] << 8) | buf[5];
            bool on = (val == 0xFF00);

            /* 查询 addr 对应的设备名并执行 */
            char dev_name[32];
            snprintf(dev_name, sizeof(dev_name), "modbus_%d", addr);
            char args[64];
            snprintf(args, sizeof(args), "{\"power\":%s}", on ? "true":"false");
            char result[256];
            agent_bridge_dispatch_tool(m->bridge, dev_name, args, result, sizeof(result));
            m->registers[addr] = on ? 1 : 0;

            /* 回显 */
            uart_write_bytes(m->uart_num, buf, n); /* 原样回显 */
        }
        else if (func == 0x06 && n >= 8) {
            /* FC 06: 写单寄存器 → set_level */
            uint16_t addr = (buf[2] << 8) | buf[3];
            uint16_t val = (buf[4] << 8) | buf[5];
            int pct = (val * 100) / 1000; if (pct>100) pct=100;

            char dev_name[32];
            snprintf(dev_name, sizeof(dev_name), "modbus_%d", addr);
            char args[64], result[256];
            snprintf(args, sizeof(args), "{\"level\":%d}", pct);
            agent_bridge_dispatch_tool(m->bridge, dev_name, args, result, sizeof(result));
            m->registers[addr] = val;

            uart_write_bytes(m->uart_num, buf, n);
        }
    }
    vTaskDelete(NULL);
}

transport_modbus_t *transport_modbus_create(agent_bridge_t *b, int uart_num, int tx, int rx, int rts, int baud) {
    transport_modbus_t *m = calloc(1, sizeof(*m));
    m->bridge = b; m->uart_num = uart_num; m->baud = baud ? baud : 9600; m->slave_addr = 1;
    uart_config_t cfg = {.baud_rate = m->baud, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE};
    uart_param_config(uart_num, &cfg);
    uart_set_pin(uart_num, tx, rx, rts, UART_PIN_NO_CHANGE);
    uart_driver_install(uart_num, MODBUS_BUF*2, 0, 0, NULL, 0);
    return m;
}
void transport_modbus_start(transport_modbus_t *m) { if (m && !m->running) { m->running = true; xTaskCreate(modbus_task, "modbus", 4096, m, 5, &m->task); } }
void transport_modbus_stop(transport_modbus_t *m) { if (m) { m->running = false; uart_driver_delete(m->uart_num); } }
agent_transport_t *transport_modbus_get_base(transport_modbus_t *m) { if (!m) return NULL; m->base.name="modbus_rtu"; m->base.ctx=m; return &m->base; }
