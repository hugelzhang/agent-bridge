/**
 * CoAP Transport — 轻量 UDP, 约束网络协议 (RFC 7252)
 *
 * CoAP 端点:
 *   GET  /tools    → MCP 工具列表
 *   POST /call     → 执行工具调用
 *   消息格式: JSON (和非 CoAP transport 一致)
 *
 * 场景: 6LoWPAN / NB-IoT / 卫星 IoT — 带宽极度受限的网络
 * 依赖: ESP-IDF lwip socket (UDP)
 */
#include "transport_coap.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "transport_coap";
#define COAP_BUF 1500
#define COAP_PORT 5683

/* CoAP 消息头 (简化版, 仅 CON+POST, Token=0) */
#define COAP_VER  0x40  /* version 1, type CON */
#define COAP_POST 0x02
#define COAP_GET  0x01
#define COAP_CONTENT_2_05 0x45  /* 2.05 Content */
#define COAP_BAD_REQ_4_00 0x80  /* 4.00 Bad Request */
#define COAP_NOT_FOUND_4_04 0x84  /* 4.04 Not Found */

struct transport_coap {
    agent_bridge_t *bridge; agent_transport_t base;
    uint16_t port; int sock; bool running; TaskHandle_t task;
};

/**
 * CoAP URI 路径映射 (极简: 只认 /tools 和 /call)
 */
static const char *get_path(const uint8_t *buf, int len) {
    /* CoAP option 11 = Uri-Path, 在 token(0字节,简化) 之后 */
    /* 极简实现: 搜索 "/tools" 或 "/call" 字符串 */
    if (len < 20) return NULL;
    /* 跳过 4 字节头 */
    const uint8_t *ptr = buf + 4;
    while (ptr < buf + len - 6) {
        if (memcmp(ptr, "/tools", 6) == 0) return "/tools";
        if (memcmp(ptr, "/call", 5) == 0) return "/call";
        ptr++;
    }
    return NULL;
}

/**
 * 构建 CoAP 响应: header(4B) + token(0) + payload
 */
static int build_response(uint8_t type_code, const char *payload, uint8_t *out, int max) {
    if (max < 4) return 0;
    out[0] = (COAP_VER | type_code);  /* Ver + Type */
    out[1] = (type_code & 0x1F);       /* Code */
    out[2] = 0;  /* Message ID (简化=0) */
    out[3] = 0;
    int plen = payload ? strlen(payload) : 0;
    if (4 + plen > max) plen = max - 5;
    /* Payload marker 0xFF */
    if (plen > 0) {
        out[4] = 0xFF;
        memcpy(out + 5, payload, plen);
        return 5 + plen;
    }
    return 4;
}

static void coap_task(void *arg) {
    transport_coap_t *c = arg;
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(c->port), .sin_addr.s_addr = htonl(INADDR_ANY)};
    c->sock = socket(AF_INET, SOCK_DGRAM, 0);
    bind(c->sock, (struct sockaddr *)&addr, sizeof(addr));
    ESP_LOGI(TAG, "CoAP listening on :%d", c->port);

    uint8_t rx[COAP_BUF];
    struct sockaddr_in client;
    socklen_t clen = sizeof(client);

    while (c->running) {
        int n = recvfrom(c->sock, rx, COAP_BUF, 0, (struct sockaddr *)&client, &clen);
        if (n < 4) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }

        const char *path = get_path(rx, n);
        char response[COAP_BUF - 10] = {0};
        uint8_t out[COAP_BUF];
        int outlen;

        if (!path) {
            outlen = build_response(COAP_NOT_FOUND_4_04, "{\"error\":\"not_found\"}", out, sizeof(out));
        } else if (strcmp(path, "/tools") == 0) {
            agent_bridge_get_tools_json(c->bridge, response, sizeof(response));
            outlen = build_response(COAP_CONTENT_2_05, response, out, sizeof(out));
        } else if (strcmp(path, "/call") == 0) {
            /* 从 payload 提取 JSON body */
            const uint8_t *body = memchr(rx, 0xFF, n);
            char payload[512] = {0};
            if (body && (body - rx + 1 < n)) {
                int blen = n - (body - rx + 1);
                if (blen > 511) blen = 511;
                memcpy(payload, body + 1, blen);
            }
            /* 解析 tool call */
            const char *nm = strstr(payload, "\"name\":\"");
            const char *ag = strstr(payload, "\"arguments\":");
            if (nm) {
                nm += 8; char tn[64]={0}; int i=0;
                while (*nm && *nm!='"'&&i<63) tn[i++]=*nm++;
                char result[512];
                agent_bridge_dispatch_tool(c->bridge, tn, ag?ag+12:"{}", result, sizeof(result));
                outlen = build_response(COAP_CONTENT_2_05, result, out, sizeof(out));
            } else {
                outlen = build_response(COAP_BAD_REQ_4_00, "{\"error\":\"missing name\"}", out, sizeof(out));
            }
        } else {
            outlen = build_response(COAP_NOT_FOUND_4_04, "{\"error\":\"not_found\"}", out, sizeof(out));
        }
        sendto(c->sock, out, outlen, 0, (struct sockaddr *)&client, clen);
    }
    close(c->sock); vTaskDelete(NULL);
}

transport_coap_t *transport_coap_create(agent_bridge_t *b, uint16_t port) {
    transport_coap_t *c = calloc(1, sizeof(*c));
    c->bridge = b; c->port = port ? port : COAP_PORT; return c;
}
void transport_coap_start(transport_coap_t *c) { if (c && !c->running) { c->running = true; xTaskCreate(coap_task, "coap", 4096, c, 5, &c->task); } }
void transport_coap_stop(transport_coap_t *c) { if (c) { c->running = false; if (c->sock>=0) close(c->sock); } }
agent_transport_t *transport_coap_get_base(transport_coap_t *c) { if (!c) return NULL; c->base.name="coap"; c->base.ctx=c; return &c->base; }
