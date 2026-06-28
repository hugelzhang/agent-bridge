/** transport_modbus.h — Modbus RTU (RS485) | 工业 PLC/电表/变频器 | 9600-8-N-1 */
#ifndef AGENT_TRANSPORT_MODBUS_H
#define AGENT_TRANSPORT_MODBUS_H
#include "../agent_bridge.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct transport_modbus transport_modbus_t;
transport_modbus_t *transport_modbus_create(agent_bridge_t *bridge, int uart_num, int tx_pin, int rx_pin, int rts_pin, int baud);
void transport_modbus_start(transport_modbus_t *mod);
void transport_modbus_stop(transport_modbus_t *mod);
agent_transport_t *transport_modbus_get_base(transport_modbus_t *mod);
#ifdef __cplusplus
}
#endif
#endif
