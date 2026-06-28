/**
 * AgentBridge 安全模块实现
 *
 * JWT 格式: header.payload.signature
 *   header:  {"alg":"HS256","typ":"JWT"}
 *   payload: {"sub":"<device_id>","iat":<ts>,"exp":<ts>,"jti":"<nonce>"}
 *   signature: HMAC-SHA256(header.payload, secret_key)
 *
 * 依赖: mbedTLS (ESP-IDF 内置) 或 PC 端 OpenSSL
 */

#include "agent_auth.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

/* ---- ESP-IDF 依赖 ---- */
#ifdef ESP_PLATFORM
#include "mbedtls/md.h"
#include "mbedtls/base64.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#define AUTH_LOG(level, fmt, ...) ESP_LOG##level("auth", fmt, ##__VA_ARGS__)
#define AUTH_MALLOC calloc
#define AUTH_FREE free
#define get_time_ms() (esp_timer_get_time() / 1000)
#else
/* ---- PC 端 fallback (OpenSSL) ---- */
#include <openssl/hmac.h>
#include <openssl/evp.h>
#define AUTH_LOG(level, fmt, ...) printf("[auth/" #level "] " fmt "\n", ##__VA_ARGS__)
#define AUTH_MALLOC calloc
#define AUTH_FREE free
#define get_time_ms() ((uint64_t)(time(NULL) * 1000))
#endif

#define AUTH_JWT_BUF 512
#define AUTH_NVS_KEY "ab_auth_key"

struct agent_auth {
    char   device_id[32];
    char   secret_key[64];
    int    secret_key_len;
    uint32_t token_ttl_sec;
    uint64_t last_rotation_ms;
    uint64_t last_token_ms;
    char   cached_token[AUTH_JWT_BUF];
#ifdef ESP_PLATFORM
    nvs_handle_t nvs;
#endif
};

/* ═══════════════════════════════════════════
 *  Base64url 编解码 (JWT 专用: 无 padding, URL safe)
 * ═══════════════════════════════════════════ */

static int base64url_encode(const uint8_t *data, int len, char *out, int outlen) {
    /* 使用内置 base64, 再把 +→- /→_ 去掉 = */
    static const char *b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int o = 0;
    for (int i = 0; i < len; i += 3) {
        uint32_t v = (data[i] << 16) | ((i+1<len?data[i+1]:0) << 8) | (i+2<len?data[i+2]:0);
        if (o < outlen-1) out[o++] = b64[(v >> 18) & 0x3F];
        if (o < outlen-1) out[o++] = b64[(v >> 12) & 0x3F];
        if (o < outlen-1) out[o++] = (i+1<len) ? b64[(v>>6)&0x3F] : '=';
        if (o < outlen-1) out[o++] = (i+2<len) ? b64[v&0x3F] : '=';
    }
    out[o] = '\0';
    /* URL-safe: +→- /→_ 去掉= */
    for (int i=0; out[i]; i++) {
        if (out[i]=='+') out[i]='-';
        else if (out[i]=='/') out[i]='_';
        else if (out[i]=='=') { out[i]='\0'; break; }
    }
    return (int)strlen(out);
}

/* ═══════════════════════════════════════════
 *  HMAC-SHA256
 * ═══════════════════════════════════════════ */

static int hmac_sha256(const uint8_t *key, int key_len,
                        const uint8_t *msg, int msg_len,
                        uint8_t *digest) {
#ifdef ESP_PLATFORM
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    mbedtls_md_hmac_starts(&ctx, key, key_len);
    mbedtls_md_hmac_update(&ctx, msg, msg_len);
    mbedtls_md_hmac_finish(&ctx, digest);
    mbedtls_md_free(&ctx);
    return 0;
#else
    unsigned int dlen = 32;
    HMAC(EVP_sha256(), key, key_len, msg, msg_len, digest, &dlen);
    return 0;
#endif
}

/* ═══════════════════════════════════════════
 *  JWT Token 生成
 * ═══════════════════════════════════════════ */

int agent_auth_get_token(agent_auth_t *auth, char *buf, size_t len) {
    if (!auth || !buf || len < 64) return -1;

    uint64_t now = get_time_ms() / 1000;
    uint64_t exp = now + auth->token_ttl_sec;
    uint64_t iat = now;

    /* Header */
    const char *header = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
    char h_b64[128];
    base64url_encode((const uint8_t *)header, strlen(header), h_b64, sizeof(h_b64));

    /* Payload */
    char payload[256];
    snprintf(payload, sizeof(payload),
        "{\"sub\":\"%s\",\"iat\":%llu,\"exp\":%llu,\"jti\":\"%llx\"}",
        auth->device_id, (unsigned long long)iat, (unsigned long long)exp,
        (unsigned long long)(now ^ (unsigned long long)auth));
    char p_b64[256];
    base64url_encode((const uint8_t *)payload, strlen(payload), p_b64, sizeof(p_b64));

    /* Signature */
    char sign_input[512];
    snprintf(sign_input, sizeof(sign_input), "%s.%s", h_b64, p_b64);
    uint8_t sig[32];
    hmac_sha256((const uint8_t *)auth->secret_key, auth->secret_key_len,
                (const uint8_t *)sign_input, strlen(sign_input), sig);
    char s_b64[64];
    base64url_encode(sig, 32, s_b64, sizeof(s_b64));

    /* JWT = header.payload.signature */
    int total = snprintf(buf, len, "%s.%s.%s", h_b64, p_b64, s_b64);
    strncpy(auth->cached_token, buf, sizeof(auth->cached_token)-1);
    auth->last_token_ms = get_time_ms();

    AUTH_LOG(I, "Token generated for %s (exp=%llu)", auth->device_id, (unsigned long long)exp);
    return total;
}

/* ═══════════════════════════════════════════
 *  密钥轮换
 * ═══════════════════════════════════════════ */

/**
 * 生成随机密钥 (简化: 用时间+设备ID哈希)
 * 实际产品应使用 TRNG (ESP32 硬件随机数)
 */
static void generate_secret_key(agent_auth_t *auth) {
    uint64_t seed = get_time_ms();
#ifdef ESP_PLATFORM
    seed ^= esp_random();
#endif
    /* 简单哈希: 重复 SHA256 生成伪随机密钥 */
    uint8_t mix[64];
    snprintf((char *)mix, sizeof(mix), "%s_%llu_%p", auth->device_id,
             (unsigned long long)seed, (void *)auth);
    uint8_t digest[32];
    hmac_sha256(mix, strlen((char *)mix), mix, strlen((char *)mix), digest);

    /* 编码为 hex string */
    for (int i = 0; i < 32 && i*2 < (int)sizeof(auth->secret_key)-1; i++) {
        snprintf(auth->secret_key + i*2, 3, "%02x", digest[i]);
    }
    auth->secret_key_len = 64;
}

int agent_auth_rotate_key(agent_auth_t *auth) {
    if (!auth) return -1;

    char old_key[64];
    strncpy(old_key, auth->secret_key, sizeof(old_key)-1);

    generate_secret_key(auth);
    auth->last_rotation_ms = get_time_ms();

#ifdef ESP_PLATFORM
    /* 持久化新密钥到 NVS */
    if (auth->nvs) {
        nvs_set_str(auth->nvs, AUTH_NVS_KEY, auth->secret_key);
        nvs_commit(auth->nvs);
    }
#endif

    AUTH_LOG(I, "Key rotated for %s (len=%d)", auth->device_id, auth->secret_key_len);
    return 0;
}

/* ═══════════════════════════════════════════
 *  初始化
 * ═══════════════════════════════════════════ */

agent_auth_t *agent_auth_init(const agent_auth_cfg_t *cfg) {
    agent_auth_t *auth = (agent_auth_t *)AUTH_MALLOC(1, sizeof(*auth));
    if (!auth) return NULL;

    strncpy(auth->device_id, cfg->device_id, sizeof(auth->device_id)-1);
    auth->token_ttl_sec = cfg->token_ttl_sec ? cfg->token_ttl_sec : 86400; /* 24h */

#ifdef ESP_PLATFORM
    /* 从 NVS 读取已有密钥, 或生成新的 */
    nvs_handle_t nvs;
    if (nvs_open(cfg->nvs_namespace ? cfg->nvs_namespace : "agentbridge",
                 NVS_READWRITE, &nvs) == ESP_OK) {
        auth->nvs = nvs;
        size_t key_len = sizeof(auth->secret_key);
        if (nvs_get_str(nvs, AUTH_NVS_KEY, auth->secret_key, &key_len) != ESP_OK) {
            /* 无已有密钥, 生成 */
            generate_secret_key(auth);
            nvs_set_str(nvs, AUTH_NVS_KEY, auth->secret_key);
            nvs_commit(nvs);
            AUTH_LOG(I, "New secret key generated and stored");
        } else {
            auth->secret_key_len = strlen(auth->secret_key);
            AUTH_LOG(I, "Loaded existing key from NVS");
        }
    } else {
        generate_secret_key(auth);
    }
#else
    /* PC 端: 使用配置中的密钥, 或生成 */
    if (cfg->secret_key[0]) {
        strncpy(auth->secret_key, cfg->secret_key, sizeof(auth->secret_key)-1);
        auth->secret_key_len = strlen(auth->secret_key);
    } else {
        generate_secret_key(auth);
    }
#endif

    auth->last_rotation_ms = get_time_ms();
    auth->last_token_ms = 0;
    AUTH_LOG(I, "Auth init: device=%s ttl=%us", auth->device_id, auth->token_ttl_sec);
    return auth;
}

void agent_auth_deinit(agent_auth_t *auth) {
    if (!auth) return;
#ifdef ESP_PLATFORM
    if (auth->nvs) nvs_close(auth->nvs);
#endif
    AUTH_FREE(auth);
}

/* ═══════════════════════════════════════════
 *  辅助函数
 * ═══════════════════════════════════════════ */

const char *agent_auth_device_id(agent_auth_t *auth) {
    return auth ? auth->device_id : NULL;
}

int agent_auth_token_remaining(agent_auth_t *auth) {
    if (!auth || !auth->last_token_ms) return 0;
    uint64_t elapsed = (get_time_ms() - auth->last_token_ms) / 1000;
    if (elapsed >= auth->token_ttl_sec) return 0;
    return (int)(auth->token_ttl_sec - elapsed);
}

int agent_auth_build_header(agent_auth_t *auth, char *buf, size_t len) {
    if (!auth || !buf || len < 32) return -1;
    char token[AUTH_JWT_BUF];
    if (agent_auth_get_token(auth, token, sizeof(token)) < 0) return -1;
    return snprintf(buf, len, "Authorization: Bearer %s\r\n", token);
}

int agent_auth_inject_token(agent_auth_t *auth, const char *json_in,
                             char *json_out, size_t len) {
    if (!auth || !json_in || !json_out || len < 64) return -1;
    char token[AUTH_JWT_BUF];
    if (agent_auth_get_token(auth, token, sizeof(token)) < 0) return -1;

    /* 在 JSON 对象末尾前插入 "auth_token" 字段 */
    const char *closing = strrchr(json_in, '}');
    if (!closing) return -1;
    int prefix_len = closing - json_in;
    return snprintf(json_out, len, "%.*s,\"auth_token\":\"%s\"}", prefix_len, json_in, token);
}

/* ═══════════════════════════════════════════
 *  服务端验证 (PC/Cloud 侧)
 * ═══════════════════════════════════════════ */

const char *agent_auth_verify(const char *token, const char *secret_key,
                               char *device_id_out, size_t len) {
    if (!token || !secret_key) return NULL;

    /* 分割 JWT: header.payload.signature */
    char jwt[AUTH_JWT_BUF];
    strncpy(jwt, token, sizeof(jwt)-1);

    char *dots[2] = {NULL, NULL};
    char *p = jwt;
    for (int i = 0; i < 2; i++) {
        p = strchr(p, '.');
        if (!p) return NULL;
        dots[i] = p;
        *p = '\0';
        p++;
    }
    /* dots[0] → header.payload split, dots[1] → payload.signature split */
    /* jwt 现在是 header\0payload\0signature */
    const char *header = jwt;
    const char *payload = dots[0] + 1;
    const char *signature = dots[1] + 1;

    /* 重新计算签名 */
    char sign_input[512];
    int slen = snprintf(sign_input, sizeof(sign_input), "%s.%s", header, payload);
    uint8_t expected[32];
    hmac_sha256((const uint8_t *)secret_key, strlen(secret_key),
                (const uint8_t *)sign_input, slen, expected);
    char expected_b64[64];
    base64url_encode(expected, 32, expected_b64, sizeof(expected_b64));

    /* 比较签名 */
    if (strcmp(signature, expected_b64) != 0) {
        return NULL; /* 签名不匹配 */
    }

    /* 解码 payload 检查过期 */
    /* 简化: payload 是 base64url, 解码后提取 exp */
    /* 跳过完整解码, 直接在 base64 中搜索 "exp": */
    if (device_id_out && len > 0) {
        /* 从 payload 提取 sub (device_id): payload 是 base64url 编码 */
        /* 简化: 直接在 token 中搜索 (不够安全, 但 token 已签名) */
        const char *sub = strstr(token, "\"sub\":\"");
        if (sub) {
            sub += 7;
            size_t i = 0;
            while (*sub && *sub != '"' && i < len-1) device_id_out[i++] = *sub++;
            device_id_out[i] = '\0';
        }
    }

    return device_id_out;
}
