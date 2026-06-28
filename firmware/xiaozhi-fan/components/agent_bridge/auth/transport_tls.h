/**
 * TLS Transport Wrapper — 为任意 transport 加 TLS 加密通道
 *
 * 用法: 包裹一个已有的 plain transport, 升级为安全版本
 *   transport_tls_t *tls = transport_tls_create(auth, base_transport);
 *   agent_bridge_set_transport(bridge, transport_tls_get_base(tls), uri);
 *
 * 自动处理:
 *   - TLS 握手 (mbedTLS)
 *   - 证书验证
 *   - 消息加密/解密
 *   - 会话恢复
 */
#ifndef AGENT_TRANSPORT_TLS_H
#define AGENT_TRANSPORT_TLS_H

#include "../agent_bridge.h"
#include "agent_auth.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct transport_tls transport_tls_t;

/** TLS 配置 */
typedef struct {
    const char *ca_cert_pem;     /* CA 证书 (PEM), NULL=不验证服务器 */
    const char *client_cert_pem; /* 客户端证书 (PEM), NULL=不需要 */
    const char *client_key_pem;  /* 客户端私钥 (PEM) */
    bool skip_verify;            /* 跳过证书验证 (仅开发环境!) */
} transport_tls_cfg_t;

/**
 * 创建 TLS wrapper
 * @param auth   认证模块 (用于注入 token)
 * @param inner  被包裹的底层 transport
 * @param cfg    TLS 配置
 */
transport_tls_t *transport_tls_create(agent_auth_t *auth,
                                       agent_transport_t *inner,
                                       const transport_tls_cfg_t *cfg);

/** 获取 TLS wrapper 的 transport 接口 */
agent_transport_t *transport_tls_get_base(transport_tls_t *tls);

/** 获取 TLS wrapper 的 auth 模块 */
agent_auth_t *transport_tls_get_auth(transport_tls_t *tls);

#ifdef __cplusplus
}
#endif

#endif /* AGENT_TRANSPORT_TLS_H */
