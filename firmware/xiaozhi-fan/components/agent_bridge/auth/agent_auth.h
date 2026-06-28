/**
 * AgentBridge 安全模块 — TLS + 设备认证 + 密钥轮换
 *
 * 三层安全:
 *   Layer 1: TLS 加密通道 (mbedTLS, 内置)
 *   Layer 2: 设备身份认证 (HMAC-SHA256 JWT)
 *   Layer 3: 密钥自动轮换 (24h 周期)
 *
 * 用法:
 *   agent_auth_t *auth = agent_auth_init(&cfg);
 *   agent_auth_get_token(auth, token_buf, 256);  // 获取 JWT
 *   agent_auth_rotate_key(auth);                  // 手动轮换
 *   agent_auth_verify(token);                     // 服务端验证
 */
#ifndef AGENT_AUTH_H
#define AGENT_AUTH_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 配置 */
typedef struct {
    char device_id[32];     /* 设备唯一 ID, 如 "esp32_001" */
    char secret_key[64];    /* 预共享密钥 (出厂烧录) */
    uint32_t token_ttl_sec; /* Token 有效期 (秒), 0=24h 默认 */
    const char *nvs_namespace; /* NVS 存储命名空间, NULL=默认 */
} agent_auth_cfg_t;

typedef struct agent_auth agent_auth_t;

/* ===== 设备端 API ===== */

/** 初始化 (密钥存储在 NVS, 首次自动生成) */
agent_auth_t *agent_auth_init(const agent_auth_cfg_t *cfg);

/** 生成 JWT Token (HMAC-SHA256) */
int agent_auth_get_token(agent_auth_t *auth, char *buf, size_t len);

/** 手动触发密钥轮换 */
int agent_auth_rotate_key(agent_auth_t *auth);

/** 获取设备 ID */
const char *agent_auth_device_id(agent_auth_t *auth);

/** 获取 token 剩余有效时间 (秒) */
int agent_auth_token_remaining(agent_auth_t *auth);

/** 反初始化 */
void agent_auth_deinit(agent_auth_t *auth);

/* ===== 服务端验证 API (PC/Cloud 侧) ===== */

/** 验证 JWT Token, 返回 device_id (失败返回 NULL) */
const char *agent_auth_verify(const char *token, const char *secret_key,
                               char *device_id_out, size_t len);

/** 构建认证请求头 "Authorization: Bearer <token>\r\n" */
int agent_auth_build_header(agent_auth_t *auth, char *buf, size_t len);

/** 注入认证 token 到 JSON 消息中 */
int agent_auth_inject_token(agent_auth_t *auth, const char *json_in,
                             char *json_out, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* AGENT_AUTH_H */
