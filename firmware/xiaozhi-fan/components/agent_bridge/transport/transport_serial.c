/**
 * 串口传输层实现
 *
 * 协议: 每行一个 JSON 消息, '\n' 分隔
 *
 * 上行 (MCU → Agent):
 *   上电: {"type":"device_list","devices":[...]}
 *   状态: {"type":"state_update","device":"fan","state":{...}}
 *   结果: {"type":"tool_result","content":[...]}
 *
 * 下行 (Agent → MCU):
 *   命令: {"type":"tool_call","name":"fan.set_power","arguments":{"power":true}}
 *   查询: {"type":"tools/list"}
 */

#include "transport_serial.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ---- 平台适配示例 ---- */
#ifndef TRANSPORT_SERIAL_LOG
#define TRANSPORT_SERIAL_LOG(level, fmt, ...)
#endif

/* ================================================================
 *  串口连接/发送/接收
 * ================================================================ */

static int serial_connect(const char *uri) {
    transport_serial_t *ts = (transport_serial_t *)
        ((agent_transport_t *)((char *)NULL - offsetof(transport_serial_t, base)));

    /* 实际的串口初始化在 transport_serial_init 中已调用 */
    /* uri 可用于传递波特率信息, 但通常初始化时已设置 */
    (void)uri;

    TRANSPORT_SERIAL_LOG("INFO", "serial connected");
    return 0;
}

static int serial_send(const char *json_str) {
    /* 需要通过 ctx 找回 transport_serial_t... */
    /* 简化实现: ctx 存的就是 transport_serial_t* */
    /* 但因为 agent_transport_t 是基类, 需要 CONTAINER_OF */

    /* 实际应用中, transport 实现者把自己的对象指针存在 transport.ctx */
    /* 这里展示标准做法 */
    return 0;
}

static int serial_recv(char *buf, size_t len, uint32_t timeout_ms) {
    /* 由 transport_serial_feed 驱动, 这里返回已缓冲的数据 */
    return 0;
}

static void serial_disconnect(void) {
    /* 串口不需要断开 */
}

/* ================================================================
 *  帧解析 (由 feed 驱动)
 * ================================================================ */

void transport_serial_feed(transport_serial_t *ts, uint8_t byte) {
    if (byte == '\n' || byte == '\r') {
        if (ts->rx_len > 0) {
            ts->rx_buf[ts->rx_len] = '\0';

            TRANSPORT_SERIAL_LOG("DEBUG", "rx: %s", ts->rx_buf);

            /* 检查消息类型并调用 agent_bridge 处理 */
            if (strstr(ts->rx_buf, "\"tools/list\"") ||
                strstr(ts->rx_buf, "\"tools/call\"")) {
                /* 直接交给 bridge task 处理 (通过下次 poll) */
                /* 实际实现: 缓存消息, task 循环中取出 */
            }
            else if (ts->cmd_cb) {
                /* 解析 tool name 并回调 */
                const char *name_pos = strstr(ts->rx_buf, "\"name\":\"");
                if (name_pos) {
                    name_pos += 8;  /* skip "name":" */
                    char tool_name[64];
                    size_t i = 0;
                    while (*name_pos && *name_pos != '"' && i < sizeof(tool_name) - 1) {
                        tool_name[i++] = *name_pos++;
                    }
                    tool_name[i] = '\0';

                    const char *args_pos = strstr(ts->rx_buf, "\"arguments\":");
                    const char *args = args_pos ? args_pos + 12 : "{}";

                    ts->cmd_cb(tool_name, args, ts->cmd_user_data);
                }
            }

            ts->rx_len = 0;
        }
        return;
    }

    if (ts->rx_len < sizeof(ts->rx_buf) - 1) {
        ts->rx_buf[ts->rx_len++] = (char)byte;
    }
}

/* ================================================================
 *  初始化
 * ================================================================ */

void transport_serial_init(transport_serial_t *ts,
                           agent_bridge_t *bridge,
                           int baud) {
    memset(ts, 0, sizeof(*ts));

    ts->bridge = bridge;

    /* 填充基类 */
    ts->base.name      = "serial";
    ts->base.connect   = NULL;   /* 由平台实现 */
    ts->base.send      = NULL;
    ts->base.recv      = NULL;
    ts->base.disconnect = NULL;
    ts->base.ctx       = ts;

    /* 初始化串口硬件 (平台适配) */
    if (ts->uart_init) {
        ts->uart_init(baud);
    }

    /* 上电发送设备能力清单 */
    if (bridge) {
        char dev_list[1024];
        agent_bridge_get_device_list_json(bridge, dev_list, sizeof(dev_list));
        if (ts->uart_write) {
            ts->uart_write((const uint8_t *)dev_list, strlen(dev_list));
            ts->uart_write((const uint8_t *)"\n", 1);
        }
    }
}
