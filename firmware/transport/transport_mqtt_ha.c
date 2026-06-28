/**
 * MQTT Transport — Home Assistant 自动发现 实现
 *
 * 依赖: ESP-IDF esp_mqtt (mqtt_client.h)
 *
 * Home Assistant MQTT Discovery 参考:
 *   https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery
 *
 * 主题结构:
 *   发现:   homeassistant/<type>/agent_<device>/config  (retained)
 *   状态:   <prefix>/<device>/state                      (JSON)
 *   命令:   <prefix>/<device>/set                        (ON/OFF 或 JSON)
 */

#include "transport_mqtt_ha.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_log.h"
#include "mqtt_client.h"

static const char *TAG = "transport_mqtt_ha";

#define MQTT_BUF_SIZE 2048

struct transport_mqtt_ha {
    agent_bridge_t    *bridge;
    esp_mqtt_client_handle_t client;
    agent_transport_t  base;
    char               topic_prefix[32];
    char               client_id[32];
    char               broker_uri[128];
    bool               running;
};

/* ================================================================
 *  Home Assistant 自动发现 — 生成 discovery payload
 * ================================================================ */

/**
 * 发布单个设备的 HA 自动发现配置
 */
static void publish_ha_discovery(transport_mqtt_ha_t *self,
                                  agent_device_t *dev) {
    if (!self->client) return;

    char topic[128];
    char payload[MQTT_BUF_SIZE];
    const char *pfx = self->topic_prefix;

    /* 1. 开关型设备 (AB_CAP_ON_OFF) → HA switch */
    if (dev->caps & AB_CAP_ON_OFF && !(dev->caps & AB_CAP_LEVEL)) {
        snprintf(topic, sizeof(topic),
                 "homeassistant/switch/agent_%s/config", dev->name);
        snprintf(payload, sizeof(payload),
            "{"
            "\"name\":\"%s\","
            "\"unique_id\":\"agent_%s\","
            "\"state_topic\":\"%s/%s/state\","
            "\"command_topic\":\"%s/%s/set\","
            "\"payload_on\":\"ON\","
            "\"payload_off\":\"OFF\","
            "\"state_on\":\"ON\","
            "\"state_off\":\"OFF\","
            "\"value_template\":\"{{ value_json.power }}\","
            "\"device\":{"
                "\"name\":\"AgentBridge ESP32\","
                "\"identifiers\":[\"%s\"],"
                "\"manufacturer\":\"AgentBridge\","
                "\"model\":\"ESP32-S3\""
            "}"
            "}",
            dev->display_name, dev->name,
            pfx, dev->name, pfx, dev->name,
            self->client_id);
    }
    /* 2. 调光设备 (AB_CAP_LEVEL) → HA light */
    else if (dev->caps & AB_CAP_LEVEL) {
        snprintf(topic, sizeof(topic),
                 "homeassistant/light/agent_%s/config", dev->name);
        snprintf(payload, sizeof(payload),
            "{"
            "\"name\":\"%s\","
            "\"unique_id\":\"agent_%s\","
            "\"schema\":\"json\","
            "\"state_topic\":\"%s/%s/state\","
            "\"command_topic\":\"%s/%s/set\","
            "\"brightness\":true,"
            "\"brightness_scale\":100,"
            "\"supported_color_modes\":[\"brightness\"],"
            "\"value_template\":\"{{ value_json.level }}\","
            "\"state_value_template\":\"{{ 'ON' if value_json.level|int > 0 else 'OFF' }}\","
            "\"device\":{"
                "\"name\":\"AgentBridge ESP32\","
                "\"identifiers\":[\"%s\"],"
                "\"manufacturer\":\"AgentBridge\","
                "\"model\":\"ESP32-S3\""
            "}"
            "}",
            dev->display_name, dev->name,
            pfx, dev->name, pfx, dev->name,
            self->client_id);
    }
    /* 3. 传感器 (AB_CAP_READ_SENSOR) → HA sensor */
    else if (dev->caps & AB_CAP_READ_SENSOR) {
        /* 温度 */
        snprintf(topic, sizeof(topic),
                 "homeassistant/sensor/agent_%s_temp/config", dev->name);
        snprintf(payload, sizeof(payload),
            "{"
            "\"name\":\"%s Temperature\","
            "\"unique_id\":\"agent_%s_temp\","
            "\"state_topic\":\"%s/%s/state\","
            "\"unit_of_measurement\":\"°C\","
            "\"value_template\":\"{{ value_json.temperature }}\","
            "\"device_class\":\"temperature\","
            "\"device\":{"
                "\"name\":\"AgentBridge ESP32\","
                "\"identifiers\":[\"%s\"]"
            "}"
            "}",
            dev->display_name, dev->name,
            pfx, dev->name,
            self->client_id);
        esp_mqtt_client_publish(self->client, topic, payload, 0, 1, 1);

        /* 湿度 */
        snprintf(topic, sizeof(topic),
                 "homeassistant/sensor/agent_%s_hum/config", dev->name);
        snprintf(payload, sizeof(payload),
            "{"
            "\"name\":\"%s Humidity\","
            "\"unique_id\":\"agent_%s_hum\","
            "\"state_topic\":\"%s/%s/state\","
            "\"unit_of_measurement\":\"%%\","
            "\"value_template\":\"{{ value_json.humidity }}\","
            "\"device_class\":\"humidity\","
            "\"device\":{"
                "\"name\":\"AgentBridge ESP32\","
                "\"identifiers\":[\"%s\"]"
            "}"
            "}",
            dev->display_name, dev->name,
            pfx, dev->name,
            self->client_id);
        esp_mqtt_client_publish(self->client, topic, payload, 0, 1, 1);
        return;  /* 已手动 publish, 跳过下面的 publish */
    }
    else {
        return;  /* 不支持的类型, 跳过 */
    }

    esp_mqtt_client_publish(self->client, topic, payload, 0, 1, 1);
    ESP_LOGI(TAG, "HA discovery: %s → %s", dev->name, topic);
}

/**
 * 发布所有已注册设备的 HA 发现
 */
static void publish_all_discoveries(transport_mqtt_ha_t *self) {
    /* 通过 bridge 的内部设备列表 */
    char dev_list[2048];
    agent_bridge_get_device_list_json(self->bridge, dev_list, sizeof(dev_list));
    ESP_LOGI(TAG, "Publishing HA discovery for devices");

    /* 逐个设备发布发现 */
    /* agent_bridge 内部用数组存储, 这里用工具列表来遍历设备名 */
    char tools[4096];
    agent_bridge_get_tools_json(self->bridge, tools, sizeof(tools));

    /* 从 tools JSON 中提取唯一设备名 */
    /* 简单做法: 搜索 "name":"<dev>." 并去重 */
    char seen_devices[32][32] = {{0}};
    int seen_count = 0;

    const char *pos = tools;
    while ((pos = strstr(pos, "\"name\":\"")) != NULL) {
        pos += 8;  /* skip "name":" */
        char dev_name[32];
        size_t i = 0;
        while (*pos && *pos != '.' && i < sizeof(dev_name) - 1) {
            dev_name[i++] = *pos++;
        }
        dev_name[i] = '\0';

        /* 去重 */
        bool dup = false;
        for (int j = 0; j < seen_count; j++) {
            if (strcmp(seen_devices[j], dev_name) == 0) { dup = true; break; }
        }
        if (!dup && seen_count < 32) {
            strncpy(seen_devices[seen_count++], dev_name, sizeof(seen_devices[0]) - 1);
        }
        pos++;
    }

    /* 发布发现 */
    char dev_list_buf[4096];
    agent_bridge_get_device_list_json(self->bridge, dev_list_buf, sizeof(dev_list_buf));
    /* dev_list_buf 包含设备能力, 但我们简化处理: 从工具列表推断 */
    for (int i = 0; i < seen_count; i++) {
        char dev_name[32];
        strncpy(dev_name, seen_devices[i], sizeof(dev_name) - 1);
        /* 判断设备类型: 查 tools 中有 set_level → light, set_power → switch */
        bool has_level = false, has_power = false, has_sensor = false;

        const char *p = tools;
        char level_tool[64], power_tool[64], sensor_tool[64];
        snprintf(level_tool, sizeof(level_tool), "\"%s.set_level\"", dev_name);
        snprintf(power_tool, sizeof(power_tool), "\"%s.set_power\"", dev_name);
        snprintf(sensor_tool, sizeof(sensor_tool), "\"%s.get_state\"", dev_name);
        has_level = strstr(tools, level_tool) != NULL;
        has_power = strstr(tools, power_tool) != NULL;

        /* 构造临时设备结构用于发布 */
        agent_device_t tmp = {0};
        tmp.name = dev_name;
        tmp.display_name = dev_name;
        if (has_level && has_power) {
            tmp.caps = (agent_capability_t)(AB_CAP_ON_OFF | AB_CAP_LEVEL);
        } else if (has_power) {
            tmp.caps = AB_CAP_ON_OFF;
        } else {
            tmp.caps = AB_CAP_READ_SENSOR;
        }
        publish_ha_discovery(self, &tmp);
    }
}

/* ================================================================
 *  MQTT 事件处理
 * ================================================================ */

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)event_data;
    transport_mqtt_ha_t *self = (transport_mqtt_ha_t *)arg;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        /* 订阅所有命令主题 */
        {
            char cmd_topic[64];
            snprintf(cmd_topic, sizeof(cmd_topic), "%s/+/set", self->topic_prefix);
            esp_mqtt_client_subscribe(self->client, cmd_topic, 0);
            ESP_LOGI(TAG, "Subscribed: %s", cmd_topic);
        }
        /* 发布 HA 自动发现 */
        publish_all_discoveries(self);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        self->running = false;
        break;

    case MQTT_EVENT_DATA: {
        /* 解析命令主题: <prefix>/<device>/set */
        char topic[128];
        int topic_len = ev->topic_len < sizeof(topic) - 1
                        ? ev->topic_len : sizeof(topic) - 1;
        memcpy(topic, ev->topic, topic_len);
        topic[topic_len] = '\0';

        /* 提取设备名 */
        char cmd_data[256];
        int data_len = ev->data_len < sizeof(cmd_data) - 1
                       ? ev->data_len : sizeof(cmd_data) - 1;
        memcpy(cmd_data, ev->data, data_len);
        cmd_data[data_len] = '\0';

        /* 从 topic 提取设备名: <prefix>/<device>/set */
        const char *pfx = self->topic_prefix;
        size_t pfx_len = strlen(pfx);
        if (strncmp(topic, pfx, pfx_len) == 0 && topic[pfx_len] == '/') {
            const char *dev_start = topic + pfx_len + 1;
            const char *slash = strchr(dev_start, '/');
            if (slash) {
                char dev_name[32];
                size_t dlen = slash - dev_start;
                if (dlen >= sizeof(dev_name)) dlen = sizeof(dev_name) - 1;
                memcpy(dev_name, dev_start, dlen);
                dev_name[dlen] = '\0';

                /* 判断命令类型 */
                char tool_name[64];
                char args[256];
                bool valid = true;

                if (strcmp(cmd_data, "ON") == 0 || strcmp(cmd_data, "OFF") == 0) {
                    /* Switch 命令: ON/OFF */
                    snprintf(tool_name, sizeof(tool_name), "%s.set_power", dev_name);
                    snprintf(args, sizeof(args), "{\"power\":%s}",
                             strcmp(cmd_data, "ON") == 0 ? "true" : "false");
                }
                else if (cmd_data[0] == '{') {
                    /* JSON 命令 (Light: {"state":"ON","brightness":128}) */
                    /* 尝试解析 JSON */
                    const char *state = strstr(cmd_data, "\"state\":\"ON\"");
                    const char *bri = strstr(cmd_data, "\"brightness\":");

                    if (state) {
                        snprintf(tool_name, sizeof(tool_name), "%s.set_power", dev_name);
                        snprintf(args, sizeof(args), "{\"power\":true}");
                    }
                    else if (bri) {
                        int level = 0;
                        sscanf(bri + 13, "%d", &level);
                        level = (level * 100) / 255;  /* HA 0-255 → 0-100 */
                        snprintf(tool_name, sizeof(tool_name), "%s.set_level", dev_name);
                        snprintf(args, sizeof(args), "{\"level\":%d}", level);
                    }
                    else {
                        valid = false;
                    }
                }
                else {
                    valid = false;
                }

                if (valid) {
                    ESP_LOGI(TAG, "Command: %s(%s)", tool_name, args);
                    char result[512];
                    agent_bridge_dispatch_tool(self->bridge, tool_name, args,
                                               result, sizeof(result));

                    /* 发布状态更新 */
                    /* 从设备获取状态并发布 */
                    char state_topic[64];
                    snprintf(state_topic, sizeof(state_topic),
                             "%s/%s/state", pfx, dev_name);
                    esp_mqtt_client_publish(self->client, state_topic,
                                            result, 0, 0, 0);
                }
            }
        }
        break;
    }

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error");
        break;

    default:
        break;
    }
}

/* ================================================================
 *  公共 API
 * ================================================================ */

transport_mqtt_ha_t *transport_mqtt_ha_create(agent_bridge_t *bridge,
                                               const char *broker_uri,
                                               const char *client_id,
                                               const char *username,
                                               const char *password,
                                               const char *topic_prefix) {
    transport_mqtt_ha_t *mqtt = calloc(1, sizeof(*mqtt));
    if (!mqtt) return NULL;

    mqtt->bridge = bridge;
    strncpy(mqtt->topic_prefix, topic_prefix ? topic_prefix : "agentbridge",
            sizeof(mqtt->topic_prefix) - 1);
    strncpy(mqtt->client_id, client_id, sizeof(mqtt->client_id) - 1);
    if (broker_uri) {
        strncpy(mqtt->broker_uri, broker_uri, sizeof(mqtt->broker_uri) - 1);
    }

    /* 配置 MQTT client */
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = mqtt->broker_uri,
        .credentials = {
            .client_id = mqtt->client_id,
            .username = username ? username : NULL,
            .authentication.password = password ? password : NULL,
        },
        .session.keepalive = 60,
    };

    mqtt->client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(mqtt->client, ESP_EVENT_ANY_ID,
                                    mqtt_event_handler, mqtt);
    return mqtt;
}

void transport_mqtt_ha_start(transport_mqtt_ha_t *mqtt) {
    if (!mqtt || mqtt->running) return;
    esp_mqtt_client_start(mqtt->client);
    mqtt->running = true;
    ESP_LOGI(TAG, "MQTT connecting to %s...", mqtt->broker_uri);
}

void transport_mqtt_ha_stop(transport_mqtt_ha_t *mqtt) {
    if (!mqtt) return;
    esp_mqtt_client_stop(mqtt->client);
    mqtt->running = false;
}

void transport_mqtt_ha_publish_state(transport_mqtt_ha_t *mqtt,
                                      const char *device_name,
                                      const char *state_json) {
    if (!mqtt || !mqtt->client || !mqtt->running) return;
    char topic[64];
    /* 适配 HA switch 格式: 把 {"power":true} → "ON" / {"power":false} → "OFF" */
    /* 简化: 直接发 JSON, 由 HA value_template 解析 */
    snprintf(topic, sizeof(topic), "%s/%s/state",
             mqtt->topic_prefix, device_name);
    esp_mqtt_client_publish(mqtt->client, topic, state_json, 0, 0, 0);
}

agent_transport_t *transport_mqtt_ha_get_base(transport_mqtt_ha_t *mqtt) {
    if (!mqtt) return NULL;
    mqtt->base.name = "mqtt_ha";
    mqtt->base.ctx  = mqtt;
    return &mqtt->base;
}
