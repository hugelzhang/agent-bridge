/**
 * WebSocket Transport 实现 — 直连 Claude MCP / 任意 Agent
 *
 * 依赖: ESP-IDF esp_websocket_client, esp_timer
 * 协议: JSON-RPC 2.0 over WebSocket
 *
 * 消息格式:
 *   → tools/list: {"jsonrpc":"2.0","method":"tools/list","id":0}
 *   → tools/call: {"jsonrpc":"2.0","method":"tools/call",
 *                   "params":{"name":"fan.set_power","arguments":{...}},"id":N}
 *   → result:  {"jsonrpc":"2.0","result":{...},"id":N}
 *   ← state_update: Spontaneous device state push
 */

#include "transport_websocket.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"

static const char *TAG = "transport_ws";

#define WS_TASK_STACK  6144
#define WS_TASK_PRIO   5
#define WS_QUEUE_SIZE  16
#define WS_BUF_SIZE    2048
#define WS_JSON_BUF    1024
#define WS_RECONNECT_MIN_MS  2000
#define WS_RECONNECT_MAX_MS  60000

struct transport_websocket {
    agent_bridge_t          *bridge;
    esp_websocket_client_handle_t client;
    agent_transport_t        base;
    char                     ws_uri[128];
    char                     client_id[32];
    bool                     auto_reconnect;
    bool                     running;

    /* FreeRTOS */
    TaskHandle_t             task_handle;
    QueueHandle_t            tx_queue;      /* 发送队列: strings to send */
    int                      msg_id;        /* JSON-RPC message id counter */
    uint32_t                 reconnect_ms;   /* 当前重连间隔 */
};

/* ================================================================
 *  JSON-RPC 消息构建
 * ================================================================ */

/**
 * 构建 tools/list 请求
 */
static int build_tools_list(transport_websocket_t *ws,
                            const char *tools_json, char *buf, size_t len) {
    return snprintf(buf, len,
        "{\"jsonrpc\":\"2.0\",\"method\":\"tools/list\","
        "\"params\":{\"tools\":%s},\"id\":%d}",
        tools_json, ws->msg_id++);
}

/**
 * 构建 tool call 结果
 */
static int build_call_result(transport_websocket_t *ws,
                              const char *content, bool is_error,
                              int req_id, char *buf, size_t len) {
    return snprintf(buf, len,
        "{\"jsonrpc\":\"2.0\",\"result\":{"
        "\"content\":[{\"type\":\"text\",\"text\":\"%s\"}],"
        "\"isError\":%s"
        "},\"id\":%d}",
        content ? content : "",
        is_error ? "true" : "false",
        req_id);
}

/**
 * 构建设备状态推送 (server 主动通知)
 */
static int build_state_update(transport_websocket_t *ws,
                               const char *device, const char *state,
                               char *buf, size_t len) {
    return snprintf(buf, len,
        "{\"jsonrpc\":\"2.0\",\"method\":\"state_update\","
        "\"params\":{\"device\":\"%s\",\"state\":%s}}",
        device, state);
}

/* ================================================================
 *  JSON-RPC 消息解析
 * ================================================================ */

/**
 * 从 JSON-RPC 消息中提取 method 字段
 */
static int extract_method(const char *json, char *buf, size_t len) {
    const char *m = "\"method\":\"";
    const char *p = strstr(json, m);
    if (!p) return 0;
    p += strlen(m);
    size_t i = 0;
    while (*p && *p != '"' && i < len - 1) buf[i++] = *p++;
    buf[i] = '\0';
    return (int)i;
}

/**
 * 提取 params.name (tool name)
 */
static int extract_tool_name(const char *json, char *buf, size_t len) {
    const char *m = "\"name\":\"";
    const char *p = strstr(json, m);
    if (!p) return 0;
    p += strlen(m);
    size_t i = 0;
    while (*p && *p != '"' && i < len - 1) buf[i++] = *p++;
    buf[i] = '\0';
    return (int)i;
}

/**
 * 提取 params.arguments (返回指向原串的指针)
 */
static const char *extract_arguments(const char *json) {
    const char *m = "\"arguments\":";
    const char *p = strstr(json, m);
    if (!p) return "{}";
    p += strlen(m);
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/**
 * 提取 JSON-RPC id 字段
 */
static int extract_id(const char *json) {
    const char *m = "\"id\":";
    const char *p = strstr(json, m);
    if (!p) return -1;
    p += strlen(m);
    while (*p == ' ' || *p == '\t') p++;
    return atoi(p);
}

/* ================================================================
 *  WebSocket 事件处理
 * ================================================================ */

static void ws_event_handler(void *arg, esp_event_base_t base,
                              int32_t event_id, void *event_data) {
    transport_websocket_t *ws = (transport_websocket_t *)arg;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected");
        ws->reconnect_ms = WS_RECONNECT_MIN_MS;

        /* 发送 tools/list (注册设备) */
        {
            char tools[WS_JSON_BUF * 2];
            char msg[WS_JSON_BUF * 2 + 128];
            agent_bridge_get_tools_json(ws->bridge, tools, sizeof(tools));
            int len = build_tools_list(ws, tools, msg, sizeof(msg));
            esp_websocket_client_send_text(ws->client, msg, len, portMAX_DELAY);
            ESP_LOGI(TAG, "Sent tools/list (%d tools)", (int)strlen(tools));
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket disconnected");
        break;

    case WEBSOCKET_EVENT_DATA: {
        char buf[WS_JSON_BUF * 2];
        int dlen = data->data_len < sizeof(buf) - 1
                   ? data->data_len : sizeof(buf) - 1;
        memcpy(buf, data->data_ptr, dlen);
        buf[dlen] = '\0';

        /* 解析消息类型 */
        char method[64];
        int mlen = extract_method(buf, method, sizeof(method));
        if (mlen <= 0) {
            ESP_LOGW(TAG, "Received non-JSON-RPC message: %.100s", buf);
            break;
        }

        ESP_LOGI(TAG, "Received: %s", method);

        if (strcmp(method, "tools/call") == 0) {
            /* Agent 下发的工具调用 → 执行设备操作 */
            char tool_name[64];
            if (extract_tool_name(buf, tool_name, sizeof(tool_name)) > 0) {
                const char *args = extract_arguments(buf);
                int req_id = extract_id(buf);

                ESP_LOGI(TAG, "Tool call: %s args=%.100s id=%d",
                         tool_name, args, req_id);

                char result[512];
                agent_bridge_dispatch_tool(ws->bridge, tool_name, args,
                                           result, sizeof(result));

                /* 返回结果 */
                char response[WS_JSON_BUF * 2];
                int rlen = build_call_result(ws, result, false, req_id,
                                              response, sizeof(response));
                esp_websocket_client_send_text(ws->client, response,
                                               rlen, portMAX_DELAY);
            }
        }
        else if (strcmp(method, "tools/list") == 0) {
            /* Server 请求工具列表 (通常是重新连接后) */
            char tools[WS_JSON_BUF * 2];
            char msg[WS_JSON_BUF * 2 + 128];
            agent_bridge_get_tools_json(ws->bridge, tools, sizeof(tools));
            int len = build_tools_list(ws, tools, msg, sizeof(msg));
            esp_websocket_client_send_text(ws->client, msg, len, portMAX_DELAY);
        }
        /* 其他 method 忽略 */
        break;
    }

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error");
        break;

    default:
        break;
    }
}

/* ================================================================
 *  WebSocket 主任务 (处理连接 + 重连 + 发送队列)
 * ================================================================ */

static void ws_task(void *arg) {
    transport_websocket_t *ws = (transport_websocket_t *)arg;

    while (ws->running) {
        /* 启动 WebSocket 连接 */
        esp_websocket_client_start(ws->client);

        /* 等待连接或断开 */
        /* esp_websocket_client_start 是阻塞的, 会保持连接直到断开 */
        /* 实际上 start() 是非阻塞的, 需要手动等待 */
        /* 简化: 用定时器检查状态 */
        while (ws->running) {
            /* 检查发送队列 */
            char *tx_msg = NULL;
            if (xQueueReceive(ws->tx_queue, &tx_msg, pdMS_TO_TICKS(1000)) == pdTRUE) {
                if (esp_websocket_client_is_connected(ws->client)) {
                    esp_websocket_client_send_text(ws->client, tx_msg,
                                                    strlen(tx_msg), portMAX_DELAY);
                }
                free(tx_msg);
            }

            /* 检查连接状态 */
            if (!esp_websocket_client_is_connected(ws->client)) {
                break;  /* 断开, 准备重连 */
            }
        }

        esp_websocket_client_stop(ws->client);

        if (!ws->running) break;

        /* 重连退避 */
        if (ws->auto_reconnect) {
            ESP_LOGI(TAG, "Reconnecting in %lu ms...", ws->reconnect_ms);
            vTaskDelay(pdMS_TO_TICKS(ws->reconnect_ms));
            ws->reconnect_ms *= 2;
            if (ws->reconnect_ms > WS_RECONNECT_MAX_MS)
                ws->reconnect_ms = WS_RECONNECT_MAX_MS;
        } else {
            break;
        }
    }

    ESP_LOGI(TAG, "WS task exiting");
    vTaskDelete(NULL);
}

/* ================================================================
 *  公共 API
 * ================================================================ */

transport_websocket_t *transport_websocket_create(agent_bridge_t *bridge,
                                                   const char *ws_uri,
                                                   const char *client_id,
                                                   bool auto_reconnect) {
    transport_websocket_t *ws = calloc(1, sizeof(*ws));
    if (!ws) return NULL;

    ws->bridge = bridge;
    ws->auto_reconnect = auto_reconnect;
    ws->reconnect_ms = WS_RECONNECT_MIN_MS;
    ws->running = false;
    strncpy(ws->ws_uri, ws_uri, sizeof(ws->ws_uri) - 1);
    strncpy(ws->client_id, client_id, sizeof(ws->client_id) - 1);

    /* 配置 WebSocket client */
    esp_websocket_client_config_t cfg = {
        .uri = ws->ws_uri,
        .reconnect_timeout_ms = 0,  /* 自己管理重连 */
        .buffer_size = WS_BUF_SIZE,
    };
    ws->client = esp_websocket_client_init(&cfg);
    esp_websocket_register_events(ws->client, WEBSOCKET_EVENT_ANY,
                                   ws_event_handler, ws);

    /* 创建发送队列 */
    ws->tx_queue = xQueueCreate(WS_QUEUE_SIZE, sizeof(char *));

    return ws;
}

void transport_websocket_start(transport_websocket_t *ws) {
    if (!ws || ws->running) return;
    ws->running = true;
    xTaskCreate(ws_task, "ws_transport", WS_TASK_STACK, ws,
                WS_TASK_PRIO, &ws->task_handle);
    ESP_LOGI(TAG, "WebSocket connecting to %s...", ws->ws_uri);
}

void transport_websocket_stop(transport_websocket_t *ws) {
    if (!ws) return;
    ws->running = false;
    esp_websocket_client_stop(ws->client);
    /* task will exit on its own */
}

agent_transport_t *transport_websocket_get_base(transport_websocket_t *ws) {
    if (!ws) return NULL;
    ws->base.name = "websocket";
    ws->base.ctx  = ws;
    return &ws->base;
}
