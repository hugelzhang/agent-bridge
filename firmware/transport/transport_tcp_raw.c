/**
 * TCP Raw Transport — 最简 TCP 透传
 * 协议: 每行一个 JSON, '\n' 分隔, 和 transport_serial 完全相同的协议
 * 端口: 默认 9000
 * 场景: 不需要 WebSocket/HTTP 时的最轻量 IP 通信
 * 依赖: ESP-IDF lwip socket (或 POSIX socket)
 */
#include "transport_tcp_raw.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "transport_tcp";
#define TCP_BUF 1024

struct transport_tcp_raw {
    agent_bridge_t *bridge; agent_transport_t base;
    uint16_t port; int listen_fd; int client_fd;
    bool running; TaskHandle_t task; char rx_buf[TCP_BUF]; int rx_len;
};

static void tcp_task(void *arg) {
    transport_tcp_raw_t *t = arg;
    t->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(t->port), .sin_addr.s_addr = htonl(INADDR_ANY)};
    bind(t->listen_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(t->listen_fd, 1);
    ESP_LOGI(TAG, "Listening on :%d", t->port);

    while (t->running) {
        t->client_fd = accept(t->listen_fd, NULL, NULL);
        if (t->client_fd < 0) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }
        ESP_LOGI(TAG, "Client connected");

        /* 发送 tools list */
        char tools[2048];
        agent_bridge_get_tools_json(t->bridge, tools, sizeof(tools));
        send(t->client_fd, tools, strlen(tools), 0);
        send(t->client_fd, "\n", 1, 0);

        t->rx_len = 0;
        while (t->running) {
            char c;
            if (recv(t->client_fd, &c, 1, 0) <= 0) break;
            if (c == '\n' && t->rx_len > 0) {
                t->rx_buf[t->rx_len] = '\0';
                /* 解析 {"name":"dev.action","arguments":{...}} */
                const char *nm = strstr(t->rx_buf, "\"name\":\"");
                const char *ag = strstr(t->rx_buf, "\"arguments\":");
                if (nm) {
                    nm += 8; char tn[64]={0}; int i=0;
                    while (*nm && *nm != '"' && i<63) tn[i++]=*nm++;
                    const char *args = ag ? ag+12 : "{}";
                    char result[512];
                    agent_bridge_dispatch_tool(t->bridge, tn, args, result, sizeof(result));
                    send(t->client_fd, result, strlen(result), 0);
                    send(t->client_fd, "\n", 1, 0);
                }
                t->rx_len = 0;
            } else if (t->rx_len < TCP_BUF-1) {
                t->rx_buf[t->rx_len++] = c;
            }
        }
        close(t->client_fd); t->client_fd = -1;
        ESP_LOGI(TAG, "Client disconnected");
    }
    close(t->listen_fd); vTaskDelete(NULL);
}

transport_tcp_raw_t *transport_tcp_raw_create(agent_bridge_t *b, uint16_t port) {
    transport_tcp_raw_t *t = calloc(1, sizeof(*t));
    t->bridge = b; t->port = port ? port : 9000; t->client_fd = -1; t->listen_fd = -1;
    return t;
}
void transport_tcp_raw_start(transport_tcp_raw_t *t) {
    if (!t || t->running) return; t->running = true;
    xTaskCreate(tcp_task, "tcp_raw", 4096, t, 5, &t->task);
}
void transport_tcp_raw_stop(transport_tcp_raw_t *t) { if (t) { t->running = false; close(t->client_fd); close(t->listen_fd); } }
agent_transport_t *transport_tcp_raw_get_base(transport_tcp_raw_t *t) {
    if (!t) return NULL; t->base.name = "tcp_raw"; t->base.ctx = t; return &t->base;
}
