/** transport_tcp_raw.h — 最简 TCP 透传 | 端口 9000 | 平台: 通用 POSIX socket */
#ifndef AGENT_TRANSPORT_TCP_RAW_H
#define AGENT_TRANSPORT_TCP_RAW_H
#include "../agent_bridge.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct transport_tcp_raw transport_tcp_raw_t;
transport_tcp_raw_t *transport_tcp_raw_create(agent_bridge_t *bridge, uint16_t port);
void transport_tcp_raw_start(transport_tcp_raw_t *tcp);
void transport_tcp_raw_stop(transport_tcp_raw_t *tcp);
agent_transport_t *transport_tcp_raw_get_base(transport_tcp_raw_t *tcp);
#ifdef __cplusplus
}
#endif
#endif
