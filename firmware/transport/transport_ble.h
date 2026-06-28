/** transport_ble.h — BLE GATT 传输 | 手机直连配网+控制 | ESP32 BLE */
#ifndef AGENT_TRANSPORT_BLE_H
#define AGENT_TRANSPORT_BLE_H
#include "../agent_bridge.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct transport_ble transport_ble_t;
transport_ble_t *transport_ble_create(agent_bridge_t *bridge, const char *device_name);
void transport_ble_start(transport_ble_t *ble);
void transport_ble_stop(transport_ble_t *ble);
agent_transport_t *transport_ble_get_base(transport_ble_t *ble);
#ifdef __cplusplus
}
#endif
#endif
