/**
 * Web Console Transport — 浏览器直接控制设备
 *
 * ESP32 内嵌 Web 服务器, 浏览器打开 http://<esp32-ip> 即可:
 *   - 设备卡片 (开关/滑块/传感器)
 *   - 实时状态更新 (SSE)
 *   - 深色主题 UI
 *
 * 端口: 默认 80
 * 依赖: ESP-IDF HTTP Server
 */
#ifndef AGENT_TRANSPORT_WEBCONSOLE_H
#define AGENT_TRANSPORT_WEBCONSOLE_H
#include "../agent_bridge.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct transport_webconsole transport_webconsole_t;
transport_webconsole_t *transport_webconsole_create(agent_bridge_t *bridge, uint16_t port);
void transport_webconsole_start(transport_webconsole_t *wc);
void transport_webconsole_stop(transport_webconsole_t *wc);
agent_transport_t *transport_webconsole_get_base(transport_webconsole_t *wc);
#ifdef __cplusplus
}
#endif
#endif
