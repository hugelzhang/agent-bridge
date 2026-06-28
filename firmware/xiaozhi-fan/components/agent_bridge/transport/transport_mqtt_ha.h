/**
 * MQTT Transport — Home Assistant 自动发现
 *
 * 功能:
 *   1. 连接 MQTT Broker (如 Home Assistant 内置 Mosquitto)
 *   2. 每个 AgentBridge 设备自动生成 HA auto-discovery 消息
 *   3. 设备状态变化 → 发布 MQTT state 消息
 *   4. HA 下发命令 → 解析 → agent_bridge_dispatch_tool → 执行
 *
 * 用法:
 *   transport_mqtt_ha_t *mqtt = transport_mqtt_ha_create(bridge,
 *       "mqtt://homeassistant.local:1883", "agent_esp32", "user", "pass");
 *   transport_mqtt_ha_start(mqtt);
 *
 * 平台依赖: ESP-IDF esp_mqtt
 *
 * Home Assistant 侧零配置 — 设备自动出现在面板中.
 */

#ifndef AGENT_TRANSPORT_MQTT_HA_H
#define AGENT_TRANSPORT_MQTT_HA_H

#include "../agent_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct transport_mqtt_ha transport_mqtt_ha_t;

/**
 * 创建 MQTT HA transport
 * @param bridge    AgentBridge 实例
 * @param broker_uri MQTT broker 地址, 如 "mqtt://192.168.1.100:1883"
 * @param client_id  客户端 ID, 如 "agent_esp32_001"
 * @param username   MQTT 用户名 (可选, NULL = 匿名)
 * @param password   MQTT 密码 (可选)
 * @param topic_prefix 主题前缀, 默认 "agentbridge"
 */
transport_mqtt_ha_t *transport_mqtt_ha_create(agent_bridge_t *bridge,
                                               const char *broker_uri,
                                               const char *client_id,
                                               const char *username,
                                               const char *password,
                                               const char *topic_prefix);

/** 启动 MQTT 连接 (异步) */
void transport_mqtt_ha_start(transport_mqtt_ha_t *mqtt);

/** 停止连接 */
void transport_mqtt_ha_stop(transport_mqtt_ha_t *mqtt);

/** 发布设备状态 (设备驱动变化时调用) */
void transport_mqtt_ha_publish_state(transport_mqtt_ha_t *mqtt,
                                      const char *device_name,
                                      const char *state_json);

/** 获取内嵌 transport 指针 */
agent_transport_t *transport_mqtt_ha_get_base(transport_mqtt_ha_t *mqtt);

#ifdef __cplusplus
}
#endif

#endif /* AGENT_TRANSPORT_MQTT_HA_H */
