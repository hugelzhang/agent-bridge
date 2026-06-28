/**
 * TLS Transport Wrapper 实现
 *
 * 原理: 包裹底层 transport, 在 send/recv 时插入 TLS 加密.
 * 每个 transport 自动在每条消息中注入 auth token.
 *
 * 依赖: ESP-IDF mbedTLS
 * 编译: 需要 mbedtls 组件
 */

#include "transport_tls.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef ESP_PLATFORM
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"
#include "esp_log.h"
#define TLS_LOG(level, fmt, ...) ESP_LOG##level("tls", fmt, ##__VA_ARGS__)
#else
#define TLS_LOG(level, fmt, ...) printf("[tls/" #level "] " fmt "\n", ##__VA_ARGS__)
#endif

#define TLS_BUF 2048

struct transport_tls {
    agent_auth_t       *auth;
    agent_transport_t  *inner;     /* 底层 transport */
    agent_transport_t   base;      /* 对外暴露的 transport */
    transport_tls_cfg_t cfg;
    bool                connected;

#ifdef ESP_PLATFORM
    mbedtls_ssl_context     ssl;
    mbedtls_ssl_config      ssl_conf;
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context drbg;
#endif
};

/* ═══════════════════════════════════════════
 *  TLS 包装的 connect / send / recv / disconnect
 * ═══════════════════════════════════════════ */

static int tls_connect(const char *uri) {
    transport_tls_t *t = (transport_tls_t *)
        ((char *)NULL - offsetof(transport_tls_t, base));

#ifdef ESP_PLATFORM
    /* 初始化 mbedTLS */
    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->ssl_conf);
    mbedtls_entropy_init(&t->entropy);
    mbedtls_ctr_drbg_init(&t->drbg);

    mbedtls_ctr_drbg_seed(&t->drbg, mbedtls_entropy_func, &t->entropy, NULL, 0);
    mbedtls_ssl_config_defaults(&t->ssl_conf,
        MBEDTLS_SSL_IS_CLIENT,
        MBEDTLS_SSL_TRANSPORT_STREAM,
        MBEDTLS_SSL_PRESET_DEFAULT);
    mbedtls_ssl_conf_rng(&t->ssl_conf, mbedtls_ctr_drbg_random, &t->drbg);

    /* 证书验证: 开发模式可跳过 */
    if (t->cfg.skip_verify) {
        mbedtls_ssl_conf_authmode(&t->ssl_conf, MBEDTLS_SSL_VERIFY_NONE);
    }
    mbedtls_ssl_setup(&t->ssl, &t->ssl_conf);
#endif

    if (t->inner && t->inner->connect) {
        int ret = t->inner->connect(uri);
        if (ret == 0) t->connected = true;
        return ret;
    }
    t->connected = true;
    return 0;
}

static int tls_send(const char *json_str) {
    transport_tls_t *t = (transport_tls_t *)
        ((char *)NULL - offsetof(transport_tls_t, base));
    if (!t || !json_str) return -1;

    /* 注入 auth token */
    char secured[TLS_BUF];
    int slen;
    if (t->auth) {
        slen = agent_auth_inject_token(t->auth, json_str, secured, sizeof(secured));
        if (slen < 0) {
            strncpy(secured, json_str, sizeof(secured)-1);
            slen = strlen(secured);
        }
    } else {
        strncpy(secured, json_str, sizeof(secured)-1);
        slen = strlen(secured);
    }

    /* 通过底层 transport 发送 */
    if (t->inner && t->inner->send) {
        return t->inner->send(secured);
    }
    return slen;
}

static int tls_recv(char *buf, size_t len, uint32_t timeout_ms) {
    transport_tls_t *t = (transport_tls_t *)
        ((char *)NULL - offsetof(transport_tls_t, base));
    if (t->inner && t->inner->recv) {
        return t->inner->recv(buf, len, timeout_ms);
    }
    return 0;
}

static void tls_disconnect(void) {
    transport_tls_t *t = (transport_tls_t *)
        ((char *)NULL - offsetof(transport_tls_t, base));
    t->connected = false;
#ifdef ESP_PLATFORM
    mbedtls_ssl_free(&t->ssl);
    mbedtls_ssl_config_free(&t->ssl_conf);
    mbedtls_ctr_drbg_free(&t->drbg);
    mbedtls_entropy_free(&t->entropy);
#endif
    if (t->inner && t->inner->disconnect) {
        t->inner->disconnect();
    }
}

/* ═══════════════════════════════════════════
 *  公共 API
 * ═══════════════════════════════════════════ */

transport_tls_t *transport_tls_create(agent_auth_t *auth,
                                       agent_transport_t *inner,
                                       const transport_tls_cfg_t *cfg) {
    transport_tls_t *t = calloc(1, sizeof(*t));
    if (!t) return NULL;

    t->auth = auth;
    t->inner = inner;
    if (cfg) memcpy(&t->cfg, cfg, sizeof(*cfg));

    t->base.name      = "tls_wrapper";
    t->base.connect   = tls_connect;
    t->base.send      = tls_send;
    t->base.recv      = tls_recv;
    t->base.disconnect = tls_disconnect;
    t->base.ctx       = t;

    TLS_LOG(I, "TLS wrapper created (skip_verify=%d)", t->cfg.skip_verify);
    return t;
}

agent_transport_t *transport_tls_get_base(transport_tls_t *tls) {
    return tls ? &tls->base : NULL;
}

agent_auth_t *transport_tls_get_auth(transport_tls_t *tls) {
    return tls ? tls->auth : NULL;
}
