/**
 * 传输层: 串口/UART (AT 指令风格 + JSON 透传)
 *
 * 适用场景:
 *   - ESP32 ↔ 上位机 (USB 串口)
 *   - MCU ↔ 4G 模组 (AT 透传)
 *   - MCU ↔ 另一个 MCU (TTL 串口)
 *
 * 协议:
 *   每帧以 '\n' 分隔, 内容为 JSON.
 *   上电后自动发送设备能力清单.
 */

#ifndef AGENT_TRANSPORT_SERIAL_H
#define AGENT_TRANSPORT_SERIAL_H

#include "../agent_bridge.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 串口传输层上下文 */
typedef struct {
    agent_transport_t base;         /* 基类 */

    /* ---- 平台适配: 实现以下函数 ---- */
    int  (*uart_init)(int baud);    /* 初始化串口 */
    int  (*uart_write)(const uint8_t *data, size_t len);  /* 发送字节 */
    int  (*uart_read)(uint8_t *buf, size_t len, uint32_t timeout_ms); /* 接收 */

    /* ---- 内部状态 ---- */
    char    rx_buf[512];
    size_t  rx_len;

    agent_cmd_cb_t  cmd_cb;
    void           *cmd_user_data;
    agent_bridge_t *bridge;
} transport_serial_t;

/**
 * 初始化串口传输层
 * @param ts        传输层对象
 * @param bridge    AgentBridge 实例 (用于获取设备列表)
 * @param baud      波特率 (如 115200)
 */
void transport_serial_init(transport_serial_t *ts,
                           agent_bridge_t *bridge,
                           int baud);

/**
 * 需要在串口接收中断或轮询中调用, 喂入收到的字节
 */
void transport_serial_feed(transport_serial_t *ts, uint8_t byte);

#ifdef __cplusplus
}
#endif

#endif /* AGENT_TRANSPORT_SERIAL_H */
