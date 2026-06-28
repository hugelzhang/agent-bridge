/**
 * AgentBridge OTA 实现 — HTTP 下载 + ESP32 OTA 刷写
 * 依赖: ESP-IDF esp_https_ota, esp_ota_ops
 */
#include "agent_ota.h"
#include "../agent_bridge.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "agent_ota"
#define OTA_BUF 256

struct agent_ota {
    agent_bridge_t *bridge;
    char version[32];
    char current_sha256[65];
    bool upgrading;
    agent_ota_progress_cb_t progress_cb;
    void *progress_ctx;
};

/* ═══════════════════════════════════════════
 *  AgentBridge device "ota" 操作回调
 * ═══════════════════════════════════════════ */

static int ota_on(void *ctx, bool on)  { (void)ctx; (void)on; return 0; }
static int ota_set_level(void *ctx, uint8_t pct) { (void)ctx; (void)pct; return 0; }

static int ota_get_state(void *ctx, char *buf, size_t len) {
    agent_ota_t *ota = (agent_ota_t *)ctx;
    const esp_app_desc_t *desc = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    return snprintf(buf, len,
        "{\"version\":\"%s\",\"project\":\"%s\",\"partition\":\"%s\","
        "\"idf_version\":\"%s\",\"upgrading\":%s}",
        desc->version, desc->project_name,
        running ? running->label : "unknown",
        desc->idf_ver, ota->upgrading ? "true" : "false");
}

/* ═══════════════════════════════════════════
 *  OTA 升级任务 (FreeRTOS)
 * ═══════════════════════════════════════════ */

typedef struct {
    agent_ota_t *ota;
    char url[256];
    char sha256[65];
} ota_task_args_t;

static void ota_task(void *arg) {
    ota_task_args_t *args = (ota_task_args_t *)arg;
    agent_ota_t *ota = args->ota;

    ESP_LOGI(TAG, "OTA starting: %s", args->url);
    ota->upgrading = true;

    esp_http_client_config_t http_cfg = {
        .url = args->url,
        .timeout_ms = 30000,
    };

    /* 高级 OTA: 如果 URL 是 https, 可配置证书验证 */
    /* 简化版: 跳过 TLS 验证 (仅开发) */
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
        .partial_http_download = false,
    };

    esp_err_t ret = esp_https_ota(&ota_cfg);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA completed, rebooting in 3s...");
        if (ota->progress_cb) ota->progress_cb(100, ota->progress_ctx);
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(ret));
        ota->upgrading = false;
        if (ota->progress_cb) ota->progress_cb(-1, ota->progress_ctx);
    }
    free(args);
    vTaskDelete(NULL);
}

/* ═══════════════════════════════════════════
 *  公共 API
 * ═══════════════════════════════════════════ */

agent_ota_t *agent_ota_init(agent_bridge_t *bridge) {
    agent_ota_t *ota = calloc(1, sizeof(*ota));
    ota->bridge = bridge;

    /* 获取固件版本 */
    const esp_app_desc_t *desc = esp_app_get_description();
    strncpy(ota->version, desc->version, sizeof(ota->version)-1);

    /* 注册为 AgentBridge device "ota" */
    agent_device_t dev = {
        .name = "ota", .display_name = "Firmware Update",
        .description = "Over-the-air firmware upgrade",
        .caps = AB_CAP_ON_OFF,  /* on=触发升级 */
        .hw_ctx = ota,
        .ops = { .on = ota_on, .set_level = ota_set_level, .get_state = ota_get_state },
    };
    agent_bridge_register_device(bridge, &dev);

    ESP_LOGI(TAG, "OTA init: version=%s", ota->version);
    return ota;
}

int agent_ota_start_upgrade(agent_ota_t *ota, const char *url,
                             const char *sha256, agent_ota_progress_cb_t cb,
                             void *user_data) {
    if (!ota || ota->upgrading) return -1;

    ota->progress_cb = cb;
    ota->progress_ctx = user_data;
    if (sha256) strncpy(ota->current_sha256, sha256, sizeof(ota->current_sha256)-1);

    ota_task_args_t *args = calloc(1, sizeof(*args));
    args->ota = ota;
    strncpy(args->url, url, sizeof(args->url)-1);
    if (sha256) strncpy(args->sha256, sha256, sizeof(args->sha256)-1);

    xTaskCreate(ota_task, "ota_upgrade", 8192, args, 5, NULL);
    return 0;
}

const char *agent_ota_get_version(agent_ota_t *ota) {
    return ota ? ota->version : NULL;
}

void agent_ota_mark_valid(agent_ota_t *ota) {
    (void)ota;
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "Firmware marked as valid, rollback cancelled");
}

void agent_ota_deinit(agent_ota_t *ota) {
    if (ota) free(ota);
}
