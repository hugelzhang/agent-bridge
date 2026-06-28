/** transport_espnow.h — ESP-NOW 自组网 (无路由器 WiFi 直连) | ESP32 专有 */
#ifndef AGENT_TRANSPORT_ESPNOW_H
#define AGENT_TRANSPORT_ESPNOW_H
#include "../agent_bridge.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct transport_espnow transport_espnow_t;
transport_espnow_t *transport_espnow_create(agent_bridge_t *bridge, const uint8_t *pmk);
void transport_espnow_start(transport_espnow_t *espnow);
void transport_espnow_stop(transport_espnow_t *espnow);
void transport_espnow_add_peer(const uint8_t *mac);
agent_transport_t *transport_espnow_get_base(transport_espnow_t *espnow);
#ifdef __cplusplus
}
#endif
#endif
