/**
 * BLE Transport — 手机直连 (配网 + 控制)
 *
 * GATT Service: AgentBridge (UUID 0xA800)
 *   Char 0xA801 (TX): ESP32 → Phone, NOTIFY, JSON 消息
 *   Char 0xA802 (RX): Phone → ESP32, WRITE, JSON 命令
 *
 * JSON 命令格式:
 *   {"name":"fan.set_power","arguments":{"power":true}}
 *
 * 场景:
 *   - 手机 App/小程序 BLE 直连控制 (无 WiFi 环境)
 *   - 配网: App 通过 BLE 发送 WiFi SSID/密码
 *   - 低功耗传感器: BLE 广播温湿度数据
 *
 * 依赖: ESP-IDF nimble (或 bluedroid)
 */
#include "transport_ble.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"

/* nimble 头文件 */
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "services/gatt/ble_svc_gatt.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char *TAG = "transport_ble";

#define BLE_SVC_UUID   0xA800
#define BLE_CHR_TX_UUID 0xA801  /* ESP32 → Phone (NOTIFY) */
#define BLE_CHR_RX_UUID 0xA802  /* Phone → ESP32 (WRITE) */
#define BLE_MAX_MTU     512
#define BLE_BUF         1024

struct transport_ble {
    agent_bridge_t *bridge; agent_transport_t base;
    char name[32]; bool running;
    uint16_t tx_handle, rx_handle; /* GATT characteristic handles */
};

static transport_ble_t *g_ble = NULL;

/* BLE 事件回调 */
static int ble_gap_event(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "BLE connected");
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected, restarting advertising");
        ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, NULL, NULL, NULL);
        return 0;
    default: return 0;
    }
}

/* GATT 写回调 (Phone → ESP32) */
static int ble_rx_write(struct ble_gatt_char_context *ctx, void *arg) {
    if (ctx->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    if (!g_ble || ctx->data_len <= 0) return 0;

    char buf[BLE_BUF];
    int len = ctx->data_len < BLE_BUF-1 ? ctx->data_len : BLE_BUF-1;
    memcpy(buf, ctx->data, len); buf[len] = '\0';

    ESP_LOGI(TAG, "RX: %.100s", buf);

    /* 解析 tool call */
    const char *nm = strstr(buf, "\"name\":\"");
    const char *ag = strstr(buf, "\"arguments\":");
    if (nm) {
        nm += 8; char tn[64]={0}; int i=0;
        while (*nm && *nm!='"' && i<63) tn[i++]=*nm++;
        char result[BLE_BUF];
        agent_bridge_dispatch_tool(g_ble->bridge, tn, ag?ag+12:"{}", result, sizeof(result)-1);

        /* NOTIFY 返回结果 */
        struct os_mbuf *om = ble_hs_mbuf_from_flat(result, strlen(result));
        ble_gattc_notify_custom(g_ble->rx_handle /* connected handle */, g_ble->tx_handle, om);
    }
    return 0;
}

/* GATT 注册服务 */
static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = BLE_UUID16_DECLARE(BLE_SVC_UUID),
      .characteristics = (struct ble_gatt_chr_def[]) {
        { .uuid = BLE_UUID16_DECLARE(BLE_CHR_TX_UUID),
          .access_cb = NULL, .flags = BLE_GATT_CHR_F_NOTIFY, .val_handle = NULL },
        { .uuid = BLE_UUID16_DECLARE(BLE_CHR_RX_UUID),
          .access_cb = ble_rx_write, .flags = BLE_GATT_CHR_F_WRITE, .val_handle = NULL },
        { 0 }
    } },
    { 0 }
};

static void ble_host_task(void *arg) {
    nimble_port_run();
}

transport_ble_t *transport_ble_create(agent_bridge_t *bridge, const char *name) {
    transport_ble_t *b = calloc(1, sizeof(*b));
    b->bridge = bridge; g_ble = b;
    if (name) strncpy(b->name, name, sizeof(b->name)-1);
    else strncpy(b->name, "AgentBridge", sizeof(b->name)-1);

    nimble_port_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(gatt_svr_svcs);
    ble_gatts_add_svcs(gatt_svr_svcs);
    ble_gatts_start();
    ble_hs_cfg.reset_cb = NULL;
    ble_hs_cfg.sync_cb = NULL;

    /* 设置 GAP 设备名 */
    ble_svc_gap_device_name_set(b->name);
    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "BLE initialized: %s", b->name);
    return b;
}

void transport_ble_start(transport_ble_t *b) {
    if (!b || b->running) return;
    b->running = true;

    /* 开始 BLE 广播 */
    struct ble_gap_adv_params adv_params = {0};
    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                      &adv_params, ble_gap_event, NULL);
    ESP_LOGI(TAG, "BLE advertising as '%s'", b->name);
}

void transport_ble_stop(transport_ble_t *b) {
    if (!b) return;
    ble_gap_adv_stop();
    nimble_port_deinit();
    b->running = false;
}

agent_transport_t *transport_ble_get_base(transport_ble_t *b) {
    if (!b) return NULL; b->base.name="ble"; b->base.ctx=b; return &b->base;
}
