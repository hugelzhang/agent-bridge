/**
 * Dify HTTP Transport — 让 ESP32 通过 HTTP Server 暴露设备给 Dify/任意 Agent
 *
 * 用法:
 *   1. transport_dify_http_init(&http, bridge, 8080);
 *   2. agent_bridge_set_transport(bridge, &http.base, "http://0.0.0.0:8080");
 *
 * Agent 端:
 *   GET  http://<esp32-ip>:8080/tools     → MCP 工具列表
 *   POST http://<esp32-ip>:8080/call      → 执行工具调用
 *     Body: {"name":"fan.set_power","arguments":{"power":true}}
 *     Response: {"content":[{"type":"text","text":"..."}],"isError":false}
 *
 * 平台依赖: ESP-IDF HTTP Server (esp_http_server.h)
 */

#ifndef AGENT_TRANSPORT_DIFY_HTTP_H
#define AGENT_TRANSPORT_DIFY_HTTP_H

#include "../agent_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct transport_dify_http transport_dify_http_t;

/** 创建 HTTP transport */
transport_dify_http_t *transport_dify_http_create(agent_bridge_t *bridge,
                                                   uint16_t port);

/** 启动 HTTP server (阻塞直到 stop) */
int transport_dify_http_start(transport_dify_http_t *http);

/** 停止 HTTP server */
void transport_dify_http_stop(transport_dify_http_t *http);

/** 获取内嵌的 agent_transport_t 指针, 传给 agent_bridge_set_transport */
agent_transport_t *transport_dify_http_get_base(transport_dify_http_t *http);

#ifdef __cplusplus
}
#endif

#endif /* AGENT_TRANSPORT_DIFY_HTTP_H */
