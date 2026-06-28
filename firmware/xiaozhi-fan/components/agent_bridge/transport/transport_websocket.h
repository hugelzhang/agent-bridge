/**
 * WebSocket Transport — 直连 Claude MCP / 任意 WebSocket Agent
 *
 * 协议: JSON-RPC 2.0 over WebSocket (MCP compatible)
 *
 * 交互流程:
 *   1. ESP32 连接 ws://agent-server:8080/ws
 *   2. 握手成功后, 自动发送 tools/list (注册所有设备)
 *   3. Server 下发 tools/call → 解析 → agent_bridge_dispatch_tool → 执行 → 返回结果
 *   4. 设备状态变化 → 主动推送 state_update 消息给 Server
 *   5. 断线自动重连 (指数退避)
 *
 * 用法:
 *   transport_websocket_t *ws = transport_websocket_create(bridge,
 *       "ws://192.168.1.100:8080/ws", "agent_esp32_001");
 *   transport_websocket_start(ws);
 *
 * 对接 Claude MCP Server (PC 端):
 *   python mcp_server.py  →  WebSocket Server on :8080
 *   ESP32 连接后, Claude 可以直接发现并调用设备 tools
 *
 * 平台依赖: ESP-IDF esp_websocket_client
 */

#ifndef AGENT_TRANSPORT_WEBSOCKET_H
#define AGENT_TRANSPORT_WEBSOCKET_H

#include "../agent_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct transport_websocket transport_websocket_t;

/**
 * 创建 WebSocket transport
 * @param bridge     AgentBridge 实例
 * @param ws_uri     WebSocket 服务地址, 如 "ws://192.168.1.100:8080/ws"
 * @param client_id  客户端标识
 * @param auto_reconnect  是否自动重连 (建议 true)
 */
transport_websocket_t *transport_websocket_create(agent_bridge_t *bridge,
                                                   const char *ws_uri,
                                                   const char *client_id,
                                                   bool auto_reconnect);

/** 启动连接 (异步, 在独立 FreeRTOS 任务中运行) */
void transport_websocket_start(transport_websocket_t *ws);

/** 停止连接 */
void transport_websocket_stop(transport_websocket_t *ws);

/** 获取内嵌 transport 指针 */
agent_transport_t *transport_websocket_get_base(transport_websocket_t *ws);

#ifdef __cplusplus
}
#endif

#endif /* AGENT_TRANSPORT_WEBSOCKET_H */
