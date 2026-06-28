/**
 * AgentBridge OTA 固件升级模块
 *
 * 功能:
 *   - HTTP OTA 下载 + 自动刷写
 *   - SHA256 完整性校验
 *   - 双分区回滚保护 (ESP32 OTA0/OTA1)
 *   - 进度回调
 *   - 注册为 AgentBridge device tool, 可远程触发
 *
 * 用法:
 *   agent_ota_t *ota = agent_ota_init(bridge);
 *   agent_ota_start_upgrade(ota, "http://server/firmware.bin", "sha256...");
 *
 * 平台: ESP32 (需要 CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)
 */
#ifndef AGENT_OTA_H
#define AGENT_OTA_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct agent_ota agent_ota_t;
typedef struct agent_bridge agent_bridge_t;

/** OTA 进度回调: percent 0-100 */
typedef void (*agent_ota_progress_cb_t)(int percent, void *user_data);

/**
 * 初始化 OTA 模块, 注册为 AgentBridge device "ota"
 * 生成以下 tool:
 *   ota.upgrade    — 触发升级 (url + sha256)
 *   ota.get_state  — 查询当前版本/状态
 */
agent_ota_t *agent_ota_init(agent_bridge_t *bridge);

/** 启动升级 (异步) */
int agent_ota_start_upgrade(agent_ota_t *ota, const char *firmware_url,
                             const char *sha256_hex, agent_ota_progress_cb_t cb,
                             void *user_data);

/** 获取当前固件版本 */
const char *agent_ota_get_version(agent_ota_t *ota);

/** 标记当前固件为有效 (防止回滚) */
void agent_ota_mark_valid(agent_ota_t *ota);

/** 反初始化 */
void agent_ota_deinit(agent_ota_t *ota);

#ifdef __cplusplus
}
#endif
#endif
