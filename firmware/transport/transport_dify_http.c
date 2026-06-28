/**
 * Dify HTTP Transport 实现 — ESP32 端迷你 HTTP Server
 *
 * 依赖: ESP-IDF esp_http_server
 * 编译: 需要链接 esp_http_server 组件
 *
 * 功能:
 *   GET  /tools  → 返回 MCP tools/list JSON 数组
 *   POST /call   → 接收 {"name":"...", "arguments":{...}}, 执行, 返回结果
 *   GET  /health → {"status":"ok"}
 */

#include "transport_dify_http.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ---- ESP-IDF 依赖 ---- */
#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "transport_dify_http";

#define HTTP_MAX_BUF 2048

struct transport_dify_http {
    agent_bridge_t    *bridge;
    httpd_handle_t     server;
    uint16_t           port;
    agent_transport_t  base;     /* 嵌入的 transport 接口 */
    bool               running;
};

/* ================================================================
 *  GET /tools — 返回工具列表
 * ================================================================ */
static esp_err_t handle_get_tools(httpd_req_t *req) {
    transport_dify_http_t *self = (transport_dify_http_t *)req->user_ctx;
    char buf[HTTP_MAX_BUF];
    int len = agent_bridge_get_tools_json(self->bridge, buf, sizeof(buf) - 1);
    buf[len] = '\0';

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

/* ================================================================
 *  POST /call — 执行工具调用
 *  Body: {"name":"fan.set_power","arguments":{"power":true}}
 * ================================================================ */
static esp_err_t handle_post_call(httpd_req_t *req) {
    transport_dify_http_t *self = (transport_dify_http_t *)req->user_ctx;

    /* 读取请求体 */
    char body[HTTP_MAX_BUF];
    int recv_len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (recv_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[recv_len] = '\0';

    /* 提取 tool name 和 arguments */
    /* 简单 JSON 解析: "name":"xxx", "arguments":{...} */
    const char *name_marker = "\"name\":\"";
    const char *name_start = strstr(body, name_marker);
    if (!name_start) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'name' field");
        return ESP_FAIL;
    }
    name_start += strlen(name_marker);

    char tool_name[64];
    size_t i = 0;
    while (*name_start && *name_start != '"' && i < sizeof(tool_name) - 1) {
        tool_name[i++] = *name_start++;
    }
    tool_name[i] = '\0';

    /* 提取 arguments */
    const char *args_marker = "\"arguments\":";
    const char *args_start = strstr(body, args_marker);
    const char *args = "{}";
    if (args_start) {
        args_start += strlen(args_marker);
        while (*args_start == ' ' || *args_start == '\t') args_start++;
        args = args_start;
    }

    ESP_LOGI(TAG, "tool call: %s args=%.100s", tool_name, args);

    /* 执行设备操作 */
    char result[1024];
    agent_bridge_dispatch_tool(self->bridge, tool_name, args,
                               result, sizeof(result));

    /* 返回结果 */
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, result, strlen(result));
    return ESP_OK;
}

/* ================================================================
 *  GET /health
 * ================================================================ */
static esp_err_t handle_health(httpd_req_t *req) {
    const char *resp = "{\"status\":\"ok\",\"transport\":\"dify_http\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

/* ================================================================
 *  注册 URI handlers
 * ================================================================ */
int transport_dify_http_start(transport_dify_http_t *http) {
    if (!http || http->running) return -1;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = http->port;
    config.max_uri_handlers = 8;

    if (httpd_start(&http->server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server on port %d", http->port);
        return -1;
    }

    /* 所有 handler 的 user_ctx 指向 http 对象 */
    httpd_uri_t uri_tools = {
        .uri      = "/tools",
        .method   = HTTP_GET,
        .handler  = handle_get_tools,
        .user_ctx = http,
    };
    httpd_register_uri_handler(http->server, &uri_tools);

    httpd_uri_t uri_call = {
        .uri      = "/call",
        .method   = HTTP_POST,
        .handler  = handle_post_call,
        .user_ctx = http,
    };
    httpd_register_uri_handler(http->server, &uri_call);

    httpd_uri_t uri_health = {
        .uri      = "/health",
        .method   = HTTP_GET,
        .handler  = handle_health,
        .user_ctx = http,
    };
    httpd_register_uri_handler(http->server, &uri_health);

    http->running = true;
    ESP_LOGI(TAG, "HTTP server started on port %d", http->port);
    ESP_LOGI(TAG, "  GET  http://<ip>:%d/tools", http->port);
    ESP_LOGI(TAG, "  POST http://<ip>:%d/call", http->port);
    return 0;
}

void transport_dify_http_stop(transport_dify_http_t *http) {
    if (!http || !http->running) return;
    httpd_stop(http->server);
    http->running = false;
}

/* ================================================================
 *  Transport 接口 (agent_transport_t)
 *  这里 connect/disconnect/send/recv 由 HTTP server 自动处理
 * ================================================================ */

static int dify_connect(const char *uri) { return 0; }
static int dify_send(const char *json)   { return 0; }
static int dify_recv(char *b, size_t l, uint32_t t) { return 0; }
static void dify_disconnect(void) {}

agent_transport_t *transport_dify_http_get_base(transport_dify_http_t *http) {
    http->base.name      = "dify_http";
    http->base.connect   = dify_connect;
    http->base.send      = dify_send;
    http->base.recv      = dify_recv;
    http->base.disconnect = dify_disconnect;
    http->base.ctx       = http;
    return &http->base;
}

/* ================================================================
 *  构造 & 析构
 * ================================================================ */

transport_dify_http_t *transport_dify_http_create(agent_bridge_t *bridge,
                                                   uint16_t port) {
    transport_dify_http_t *http = calloc(1, sizeof(*http));
    if (!http) return NULL;

    http->bridge  = bridge;
    http->port    = port;
    http->server  = NULL;
    http->running = false;
    return http;
}
