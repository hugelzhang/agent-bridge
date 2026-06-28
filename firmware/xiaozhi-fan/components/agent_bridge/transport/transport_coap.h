/** transport_coap.h — CoAP (Constrained Application Protocol) 轻量 UDP 传输 | RFC 7252 | 端口 5683 */
#ifndef AGENT_TRANSPORT_COAP_H
#define AGENT_TRANSPORT_COAP_H
#include "../agent_bridge.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct transport_coap transport_coap_t;
transport_coap_t *transport_coap_create(agent_bridge_t *bridge, uint16_t port);
void transport_coap_start(transport_coap_t *coap);
void transport_coap_stop(transport_coap_t *coap);
agent_transport_t *transport_coap_get_base(transport_coap_t *coap);
#ifdef __cplusplus
}
#endif
#endif
