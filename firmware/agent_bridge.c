/**
 * AgentBridge 核心实现
 *
 * 功能:
 *   - 设备注册表管理 (链表)
 *   - Agent tool call 解析与分发
 *   - MCP tools/list 和 tools/call 消息处理
 *   - 传感器轮询
 *   - 状态变更通知
 *
 * 依赖: 无外部库 (仅标准 C + 内部 JSON 构建器)
 */

#include "agent_bridge.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ================================================================
 *  内部常量
 * ================================================================ */
#define AB_MAX_DEVICES       32
#define AB_MAX_MSG_LEN       1024
#define AB_JSON_BUF_SIZE     512
#define AB_DEFAULT_POLL_MS   5000

/* ================================================================
 *  内部 JSON 构建器 (零依赖)
 * ================================================================ */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} json_writer_t;

static void jw_init(json_writer_t *jw, char *buf, size_t cap) {
    jw->buf = buf;
    jw->len = 0;
    jw->cap = cap;
    if (cap > 0) buf[0] = '\0';
}

static int jw_append(json_writer_t *jw, const char *fmt, ...) {
    if (jw->len >= jw->cap) return -1;
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(jw->buf + jw->len, jw->cap - jw->len, fmt, args);
    va_end(args);
    if (n < 0 || (size_t)n >= jw->cap - jw->len) {
        jw->len = jw->cap - 1;
        jw->buf[jw->len] = '\0';
        return -1;
    }
    jw->len += n;
    return n;
}

/* ================================================================
 *  MCP 消息生成 (工具列表 / 工具调用结果)
 * ================================================================ */

static int __attribute__((unused)) build_tool_schema(const agent_device_t *dev, char *buf, size_t len) {
    json_writer_t jw;
    jw_init(&jw, buf, len);

    jw_append(&jw,
        "{"
        "\"name\":\"%s\","
        "\"description\":\"%s\","
        "\"inputSchema\":{"
            "\"type\":\"object\","
            "\"properties\":{",
        dev->name, dev->description);

    int prop_count = 0;

    if (dev->caps & AB_CAP_ON_OFF) {
        if (prop_count++ > 0) jw_append(&jw, ",");
        jw_append(&jw, "\"power\":{\"type\":\"boolean\",\"description\":\"true=on, false=off\"}");
    }

    if (dev->caps & AB_CAP_LEVEL) {
        if (prop_count++ > 0) jw_append(&jw, ",");
        jw_append(&jw, "\"level\":{\"type\":\"integer\",\"description\":\"0-100 percentage\",\"minimum\":0,\"maximum\":100}");
    }

    if (dev->caps & AB_CAP_POSITION) {
        if (prop_count++ > 0) jw_append(&jw, ",");
        jw_append(&jw, "\"position\":{\"type\":\"integer\",\"description\":\"angle in degrees\"}");
    }

    if (dev->caps & AB_CAP_COLOR) {
        if (prop_count++ > 0) jw_append(&jw, ",");
        jw_append(&jw, "\"color\":{\"type\":\"string\",\"description\":\"RGB hex, e.g. #FF0000\"}");
    }

    jw_append(&jw, "}},\"required\":[]}");

    return (int)jw.len;
}

static int build_tool_result(const char *text_content, bool is_error, char *buf, size_t len) {
    json_writer_t jw;
    jw_init(&jw, buf, len);
    jw_append(&jw,
        "{\"content\":[{\"type\":\"text\",\"text\":\"%s\"}],\"isError\":%s}",
        text_content ? text_content : "",
        is_error ? "true" : "false");
    return (int)jw.len;
}

/* ================================================================
 *  简易 JSON 解析 (仅支持提取顶层字符串/整数/布尔字段)
 * ================================================================ */

/**
 * 从 JSON 对象中提取字符串值
 * 例如: {"power":true} 中提取 "power" → "true"
 * 返回值: 找到的值字符串, 或 NULL
 * 注意: 返回指针指向静态 buffer, 非线程安全
 */
static const char *json_get_str(const char *json, const char *key) {
    static char val_buf[128];
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);

    const char *pos = strstr(json, search);
    if (!pos) return NULL;

    pos = strchr(pos + strlen(search), ':');
    if (!pos) return NULL;
    pos++;  /* skip ':' */

    /* skip whitespace */
    while (*pos == ' ' || *pos == '\t' || *pos == '\n') pos++;

    if (*pos == '"') {
        /* string value */
        pos++;
        size_t i = 0;
        while (*pos && *pos != '"' && i < sizeof(val_buf) - 1) {
            if (*pos == '\\' && *(pos + 1)) pos++;  /* skip escape */
            val_buf[i++] = *pos++;
        }
        val_buf[i] = '\0';
        return val_buf;
    } else {
        /* number/bool/null — read until , or } */
        size_t i = 0;
        while (*pos && *pos != ',' && *pos != '}' && *pos != ']'
               && i < sizeof(val_buf) - 1) {
            val_buf[i++] = *pos++;
        }
        val_buf[i] = '\0';
        return val_buf;
    }
}

/**
 * 从 JSON 对象中提取整数值
 */
static int json_get_int(const char *json, const char *key, int default_val) {
    const char *val = json_get_str(json, key);
    if (!val) return default_val;
    return atoi(val);
}

/**
 * 从 JSON 对象中提取布尔值
 */
static bool json_get_bool(const char *json, const char *key, bool default_val) {
    const char *val = json_get_str(json, key);
    if (!val) return default_val;
    return (strcmp(val, "true") == 0 || strcmp(val, "1") == 0);
}

/* ================================================================
 *  工具名称提取
 *  从 MCP tools/call 消息中提取 tool name 和 arguments
 * ================================================================ */

/**
 * 从 MCP JSON-RPC 消息中提取 method 字段
 * 例如: {"method":"tools/call","params":{"name":"fan.set_speed","arguments":{...}}}
 */
static int __attribute__((unused)) json_get_method(const char *json, char *buf, size_t len) {
    const char *val = json_get_str(json, "method");
    if (!val) return 0;
    size_t n = strlen(val);
    if (n >= len) n = len - 1;
    memcpy(buf, val, n);
    buf[n] = '\0';
    return (int)n;
}

/**
 * 提取 tool call 中的 tool name
 */
static int json_get_tool_name(const char *json, char *buf, size_t len) {
    /* params.name */
    /* 简单做法: 在整个 JSON 中搜索 "name":"<tool_name>" */
    const char *marker = "\"name\":\"";
    const char *pos = strstr(json, marker);
    if (!pos) {
        /* 也可能在 params 对象内部 */
        return 0;
    }
    pos += strlen(marker);
    size_t i = 0;
    while (*pos && *pos != '"' && i < len - 1) {
        buf[i++] = *pos++;
    }
    buf[i] = '\0';
    return (int)i;
}

/**
 * 提取 tool call 中的 arguments JSON
 * 返回指向 arguments 对象的指针 (在原字符串内)
 */
static const char *json_get_tool_args(const char *json) {
    const char *marker = "\"arguments\":";
    const char *pos = strstr(json, marker);
    if (!pos) return "{}";
    pos += strlen(marker);
    while (*pos == ' ' || *pos == '\t') pos++;
    return pos;  /* 指向 { 或 " */
}

/* ================================================================
 *  AgentBridge 内部结构
 * ================================================================ */

struct agent_bridge {
    agent_device_t   *devices[AB_MAX_DEVICES];
    int               device_count;

    agent_transport_t *transport;
    char               transport_uri[128];

    agent_cmd_cb_t     cmd_callback;
    void              *cmd_user_data;

    agent_cfg_t        cfg;

    char               recv_buf[AB_MAX_MSG_LEN];
    char               work_buf[AB_JSON_BUF_SIZE];

    bool               initialized;
    uint32_t           tick_ms;    /* 累计 tick (需要外部提供或自己计时) */
};

/* ================================================================
 *  日志宏
 * ================================================================ */

#define AB_LOG(level, bridge, fmt, ...) \
    do { \
        if ((bridge)->cfg.log_cb) { \
            char _lbuf[256]; \
            snprintf(_lbuf, sizeof(_lbuf), "[%s] " fmt, level, ##__VA_ARGS__); \
            (bridge)->cfg.log_cb(level, _lbuf); \
        } \
    } while(0)

/* ================================================================
 * 设备查找
 * ================================================================ */

static agent_device_t *find_device(agent_bridge_t *bridge, const char *name) {
    for (int i = 0; i < bridge->device_count; i++) {
        if (strcmp(bridge->devices[i]->name, name) == 0) {
            return bridge->devices[i];
        }
    }
    return NULL;
}

/* ================================================================
 *  工具名映射: Agent 看到的 tool name ↔ 设备+操作
 *
 *  命名规则: <device_name>.<action>
 *  例如: 设备 "fan" → tool "fan.on", "fan.off", "fan.set_level", "fan.get_state"
 *
 *  Agent 看到的工具列表:
 *   对于有 AB_CAP_ON_OFF 的设备 → 暴露 set_power tool
 *   对于有 AB_CAP_LEVEL 的设备  → 暴露 set_level tool
 *   所有设备                   → 暴露 get_state tool
 * ================================================================ */

/**
 * 解析 tool name → 设备名 + 动作
 * tool name 格式: <device_name>.<action>
 * 例如: "fan.set_level" → device="fan", action="set_level"
 */
static int parse_tool_name(const char *tool_name,
                           char *dev_name, size_t dev_len,
                           char *action, size_t act_len) {
    const char *dot = strrchr(tool_name, '.');
    if (!dot) return -1;

    size_t dlen = dot - tool_name;
    if (dlen >= dev_len) dlen = dev_len - 1;
    memcpy(dev_name, tool_name, dlen);
    dev_name[dlen] = '\0';

    const char *act = dot + 1;
    size_t alen = strlen(act);
    if (alen >= act_len) alen = act_len - 1;
    memcpy(action, act, alen);
    action[alen] = '\0';

    return 0;
}

/* ================================================================
 *  处理 Agent 下发的 tool call
 * ================================================================ */

static void handle_tool_call(agent_bridge_t *bridge,
                             const char *tool_name,
                             const char *args_json) {
    char dev_name[64], action[32];
    if (parse_tool_name(tool_name, dev_name, sizeof(dev_name),
                        action, sizeof(action)) != 0) {
        AB_LOG("WARN", bridge, "invalid tool name: %s", tool_name);
        return;
    }

    agent_device_t *dev = find_device(bridge, dev_name);
    if (!dev) {
        AB_LOG("WARN", bridge, "device not found: %s", dev_name);
        return;
    }

    char result[256] = {0};
    bool success = true;

    if (strcmp(action, "set_power") == 0 && (dev->caps & AB_CAP_ON_OFF)) {
        bool on = json_get_bool(args_json, "power", false);
        if (dev->ops.on) {
            int ret = dev->ops.on(dev->hw_ctx, on);
            success = (ret == 0);
            snprintf(result, sizeof(result),
                     success ? "{\"power\":%s}" : "{\"error\":\"hw_error\"}",
                     on ? "true" : "false");
        }
    }
    else if (strcmp(action, "set_level") == 0 && (dev->caps & AB_CAP_LEVEL)) {
        int pct = json_get_int(args_json, "level", 0);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        if (dev->ops.set_level) {
            int ret = dev->ops.set_level(dev->hw_ctx, (uint8_t)pct);
            success = (ret == 0);
            snprintf(result, sizeof(result),
                     success ? "{\"level\":%d}" : "{\"error\":\"hw_error\"}", pct);
        }
    }
    else if (strcmp(action, "set_position") == 0 && (dev->caps & AB_CAP_POSITION)) {
        int angle = json_get_int(args_json, "position", 0);
        if (dev->ops.set_position) {
            int ret = dev->ops.set_position(dev->hw_ctx, (int16_t)angle);
            success = (ret == 0);
            snprintf(result, sizeof(result),
                     success ? "{\"position\":%d}" : "{\"error\":\"hw_error\"}", angle);
        }
    }
    else if (strcmp(action, "set_color") == 0 && (dev->caps & AB_CAP_COLOR)) {
        if (dev->ops.set_color) {
            /* 解析 #RRGGBB */
            const char *hex = json_get_str(args_json, "color");
            uint8_t r = 0, g = 0, b = 0;
            if (hex && hex[0] == '#' && strlen(hex) >= 7) {
                unsigned int ur, ug, ub;
                if (sscanf(hex + 1, "%2x%2x%2x", &ur, &ug, &ub) == 3) {
                    r = (uint8_t)ur; g = (uint8_t)ug; b = (uint8_t)ub;
                }
            }
            int ret = dev->ops.set_color(dev->hw_ctx, r, g, b);
            success = (ret == 0);
            snprintf(result, sizeof(result),
                     success ? "{\"color\":\"#%02X%02X%02X\"}"
                             : "{\"error\":\"hw_error\"}", r, g, b);
        }
    }
    else if (strcmp(action, "get_state") == 0) {
        /* get_state 是所有设备的通用操作 */
        if (dev->ops.get_state) {
            int n = dev->ops.get_state(dev->hw_ctx, result, sizeof(result) - 1);
            if (n < 0) {
                snprintf(result, sizeof(result), "{\"error\":\"read_error\"}");
                success = false;
            } else {
                result[n] = '\0';
            }
        } else {
            /* 没有 get_state 实现, 返回缓存 */
            snprintf(result, sizeof(result), "%s", dev->state_cache);
        }
    }
    else {
        snprintf(result, sizeof(result),
                 "{\"error\":\"unsupported action: %s\"}", action);
        success = false;
    }

    /* 发送结果回 Agent */
    if (bridge->transport && bridge->transport->send) {
        char tool_result[AB_JSON_BUF_SIZE];
        build_tool_result(result, !success, tool_result, sizeof(tool_result));

        /* 包装为 MCP JSON-RPC 响应 */
        /* 简化: 直接发 tool_result JSON, 由传输层包装 */
        bridge->transport->send(tool_result);
    }
}

/* ================================================================
 *  生成工具列表 (MCP tools/list 响应)
 * ================================================================ */

int agent_bridge_get_tools_json(agent_bridge_t *bridge, char *buf, size_t len) {
    json_writer_t jw;
    jw_init(&jw, buf, len);
    jw_append(&jw, "[");

    for (int i = 0; i < bridge->device_count; i++) {
        agent_device_t *dev = bridge->devices[i];

        /* 为每个设备的每种能力生成一个 tool */

        if (dev->caps & AB_CAP_ON_OFF) {
            if (i > 0 || jw.len > 1) jw_append(&jw, ",");
            jw_append(&jw,
                "{\"name\":\"%s.set_power\","
                "\"description\":\"Turn %s on or off\","
                "\"inputSchema\":{"
                    "\"type\":\"object\","
                    "\"properties\":{"
                        "\"power\":{\"type\":\"boolean\",\"description\":\"true=on, false=off\"}"
                    "},"
                    "\"required\":[\"power\"]"
                "}}",
                dev->name, dev->display_name);
        }

        if (dev->caps & AB_CAP_LEVEL) {
            if (jw.len > 1) jw_append(&jw, ",");
            jw_append(&jw,
                "{\"name\":\"%s.set_level\","
                "\"description\":\"Set %s level (0-100%%)\","
                "\"inputSchema\":{"
                    "\"type\":\"object\","
                    "\"properties\":{"
                        "\"level\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100}"
                    "},"
                    "\"required\":[\"level\"]"
                "}}",
                dev->name, dev->display_name);
        }

        if (dev->caps & AB_CAP_POSITION) {
            if (jw.len > 1) jw_append(&jw, ",");
            jw_append(&jw,
                "{\"name\":\"%s.set_position\","
                "\"description\":\"Set %s position/angle\","
                "\"inputSchema\":{"
                    "\"type\":\"object\","
                    "\"properties\":{"
                        "\"position\":{\"type\":\"integer\"}"
                    "},"
                    "\"required\":[\"position\"]"
                "}}",
                dev->name, dev->display_name);
        }

        if (dev->caps & AB_CAP_COLOR) {
            if (jw.len > 1) jw_append(&jw, ",");
            jw_append(&jw,
                "{\"name\":\"%s.set_color\","
                "\"description\":\"Set %s RGB color\","
                "\"inputSchema\":{"
                    "\"type\":\"object\","
                    "\"properties\":{"
                        "\"color\":{\"type\":\"string\",\"description\":\"Hex color, e.g. #FF0000\"}"
                    "},"
                    "\"required\":[\"color\"]"
                "}}",
                dev->name, dev->display_name);
        }

        /* 每个设备都有 get_state */
        if (jw.len > 1) jw_append(&jw, ",");
        jw_append(&jw,
            "{\"name\":\"%s.get_state\","
            "\"description\":\"Get current state of %s\","
            "\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"required\":[]}}",
            dev->name, dev->display_name);
    }

    jw_append(&jw, "]");
    return (int)jw.len;
}

/* ================================================================
 *  设备能力描述 (用于 Agent 自动发现)
 * ================================================================ */

int agent_bridge_get_device_list_json(agent_bridge_t *bridge, char *buf, size_t len) {
    json_writer_t jw;
    jw_init(&jw, buf, len);
    jw_append(&jw, "{\"devices\":[");

    for (int i = 0; i < bridge->device_count; i++) {
        agent_device_t *dev = bridge->devices[i];
        if (i > 0) jw_append(&jw, ",");
        jw_append(&jw,
            "{\"name\":\"%s\",\"display_name\":\"%s\",\"description\":\"%s\",\"capabilities\":[",
            dev->name, dev->display_name, dev->description);

        int cap_count = 0;
        if (dev->caps & AB_CAP_ON_OFF) {
            if (cap_count++ > 0) jw_append(&jw, ",");
            jw_append(&jw, "\"on_off\"");
        }
        if (dev->caps & AB_CAP_LEVEL) {
            if (cap_count++ > 0) jw_append(&jw, ",");
            jw_append(&jw, "\"level\"");
        }
        if (dev->caps & AB_CAP_READ_SENSOR) {
            if (cap_count++ > 0) jw_append(&jw, ",");
            jw_append(&jw, "\"sensor\"");
        }
        if (dev->caps & AB_CAP_POSITION) {
            if (cap_count++ > 0) jw_append(&jw, ",");
            jw_append(&jw, "\"position\"");
        }
        if (dev->caps & AB_CAP_COLOR) {
            if (cap_count++ > 0) jw_append(&jw, ",");
            jw_append(&jw, "\"color\"");
        }

        jw_append(&jw, "]}");
    }

    jw_append(&jw, "]}");
    return (int)jw.len;
}

/* ================================================================
 *  外部可调用的 tool dispatch (供 transport adapter 使用)
 * ================================================================ */

int agent_bridge_dispatch_tool(agent_bridge_t *bridge,
                               const char *tool_name,
                               const char *args_json,
                               char *result_buf, size_t result_len) {
    if (!bridge || !tool_name || !args_json) return -1;

    char dev_name[64], action[32];
    if (parse_tool_name(tool_name, dev_name, sizeof(dev_name),
                        action, sizeof(action)) != 0) {
        snprintf(result_buf, result_len, "{\"error\":\"invalid tool name: %s\"}", tool_name);
        return (int)strlen(result_buf);
    }

    agent_device_t *dev = find_device(bridge, dev_name);
    if (!dev) {
        snprintf(result_buf, result_len, "{\"error\":\"device not found: %s\"}", dev_name);
        return (int)strlen(result_buf);
    }

    bool success = true;

    if (strcmp(action, "set_power") == 0 && (dev->caps & AB_CAP_ON_OFF)) {
        bool on = json_get_bool(args_json, "power", false);
        if (dev->ops.on) {
            int ret = dev->ops.on(dev->hw_ctx, on);
            success = (ret == 0);
            snprintf(result_buf, result_len,
                     "{\"power\":%s,\"success\":%s}",
                     on ? "true" : "false",
                     success ? "true" : "false");
        }
    }
    else if (strcmp(action, "set_level") == 0 && (dev->caps & AB_CAP_LEVEL)) {
        int pct = json_get_int(args_json, "level", 0);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        if (dev->ops.set_level) {
            int ret = dev->ops.set_level(dev->hw_ctx, (uint8_t)pct);
            success = (ret == 0);
            snprintf(result_buf, result_len,
                     "{\"level\":%d,\"success\":%s}", pct,
                     success ? "true" : "false");
        }
    }
    else if (strcmp(action, "set_position") == 0 && (dev->caps & AB_CAP_POSITION)) {
        int angle = json_get_int(args_json, "position", 0);
        if (dev->ops.set_position) {
            int ret = dev->ops.set_position(dev->hw_ctx, (int16_t)angle);
            success = (ret == 0);
            snprintf(result_buf, result_len,
                     "{\"position\":%d,\"success\":%s}", angle,
                     success ? "true" : "false");
        }
    }
    else if (strcmp(action, "get_state") == 0) {
        if (dev->ops.get_state) {
            int n = dev->ops.get_state(dev->hw_ctx, result_buf, result_len);
            if (n < 0) {
                snprintf(result_buf, result_len, "{\"error\":\"read_error\"}");
            } else {
                result_buf[n] = '\0';
            }
            return (int)strlen(result_buf);
        } else {
            snprintf(result_buf, result_len, "%s",
                     dev->state_cache[0] ? dev->state_cache : "{}");
        }
    }
    else {
        snprintf(result_buf, result_len,
                 "{\"error\":\"unsupported action: %s\"}", action);
        success = false;
    }

    return (int)strlen(result_buf);
}

/* ================================================================
 *  公共 API 实现
 * ================================================================ */

agent_bridge_t *agent_bridge_init(const agent_cfg_t *cfg) {
    agent_bridge_t *bridge = (agent_bridge_t *)calloc(1, sizeof(agent_bridge_t));
    if (!bridge) return NULL;

    if (cfg) {
        memcpy(&bridge->cfg, cfg, sizeof(agent_cfg_t));
    }

    bridge->device_count = 0;
    bridge->transport = NULL;
    bridge->initialized = true;
    bridge->tick_ms = 0;

    return bridge;
}

void agent_bridge_deinit(agent_bridge_t *bridge) {
    if (!bridge) return;

    if (bridge->transport && bridge->transport->disconnect) {
        bridge->transport->disconnect();
    }

    free(bridge);
}

int agent_bridge_register_device(agent_bridge_t *bridge, agent_device_t *dev) {
    if (!bridge || !dev) return -1;
    if (bridge->device_count >= AB_MAX_DEVICES) return -1;

    /* 检查重名 */
    if (find_device(bridge, dev->name)) return -1;

    bridge->devices[bridge->device_count] = dev;
    dev->bridge = bridge;
    dev->last_poll_tick = 0;
    bridge->device_count++;

    AB_LOG("INFO", bridge, "device registered: %s (caps=0x%02X)",
           dev->name, dev->caps);

    /* 通知应用层 */
    if (bridge->cfg.on_device_list_changed) {
        bridge->cfg.on_device_list_changed(bridge->cfg.user_data);
    }

    return 0;
}

int agent_bridge_unregister_device(agent_bridge_t *bridge, const char *name) {
    if (!bridge || !name) return -1;

    for (int i = 0; i < bridge->device_count; i++) {
        if (strcmp(bridge->devices[i]->name, name) == 0) {
            /* 移动后续设备 */
            for (int j = i; j < bridge->device_count - 1; j++) {
                bridge->devices[j] = bridge->devices[j + 1];
            }
            bridge->device_count--;

            if (bridge->cfg.on_device_list_changed) {
                bridge->cfg.on_device_list_changed(bridge->cfg.user_data);
            }
            return 0;
        }
    }
    return -1;
}

int agent_bridge_set_transport(agent_bridge_t *bridge,
                               agent_transport_t *transport,
                               const char *uri) {
    if (!bridge || !transport) return -1;

    /* 断开旧连接 */
    if (bridge->transport && bridge->transport->disconnect) {
        bridge->transport->disconnect();
    }

    bridge->transport = transport;
    if (uri) {
        strncpy(bridge->transport_uri, uri, sizeof(bridge->transport_uri) - 1);
    }

    /* 设置命令回调 */
    if (transport->on_command) {
        transport->on_command(NULL, bridge);  /* 先注册 user_data */
    }

    /* 连接 */
    if (transport->connect && uri) {
        int ret = transport->connect(uri);
        if (ret != 0) {
            AB_LOG("ERROR", bridge, "transport connect failed: %d", ret);
            return ret;
        }
    }

    AB_LOG("INFO", bridge, "transport set: %s -> %s",
           transport->name, uri ? uri : "(none)");
    return 0;
}

void agent_bridge_task(agent_bridge_t *bridge) {
    if (!bridge || !bridge->initialized) return;

    bridge->tick_ms++;

    /* 1. 处理收到的 Agent 消息 */
    if (bridge->transport && bridge->transport->recv) {
        int n = bridge->transport->recv(bridge->recv_buf,
                                        sizeof(bridge->recv_buf) - 1, 0);
        if (n > 0) {
            bridge->recv_buf[n] = '\0';

            /* 检查是否为 MCP tools/call 消息 */
            if (strstr(bridge->recv_buf, "\"tools/call\"") ||
                strstr(bridge->recv_buf, "\"tool_call\"") ||
                strstr(bridge->recv_buf, "\"name\":")) {

                char tool_name[64];
                int tn_len = json_get_tool_name(bridge->recv_buf,
                                                tool_name, sizeof(tool_name));
                if (tn_len > 0) {
                    const char *args = json_get_tool_args(bridge->recv_buf);
                    AB_LOG("INFO", bridge, "tool call: %s args=%s",
                           tool_name, args ? args : "{}");
                    handle_tool_call(bridge, tool_name, args ? args : "{}");
                }
            }
            /* 如果是 tools/list 请求 */
            else if (strstr(bridge->recv_buf, "\"tools/list\"")) {
                char tools_json[AB_JSON_BUF_SIZE * 2];
                agent_bridge_get_tools_json(bridge, tools_json, sizeof(tools_json));
                if (bridge->transport->send) {
                    bridge->transport->send(tools_json);
                }
            }
        }
    }

    /* 2. 轮询传感器设备 */
    for (int i = 0; i < bridge->device_count; i++) {
        agent_device_t *dev = bridge->devices[i];
        if (!(dev->caps & AB_CAP_READ_SENSOR)) continue;
        if (dev->poll_ms == 0) continue;

        uint32_t elapsed = bridge->tick_ms - dev->last_poll_tick;
        /* 简化: 假设每 tick = 1ms (实际由调用方控制) */
        if (elapsed >= dev->poll_ms) {
            dev->last_poll_tick = bridge->tick_ms;
            if (dev->ops.get_state) {
                char state[256];
                int n = dev->ops.get_state(dev->hw_ctx, state, sizeof(state) - 1);
                if (n > 0) {
                    state[n] = '\0';
                    /* 状态变化才上报 */
                    if (strcmp(state, dev->state_cache) != 0) {
                        strncpy(dev->state_cache, state, sizeof(dev->state_cache) - 1);
                        agent_bridge_notify_state(bridge, dev->name);
                    }
                }
            }
        }
    }
}

void agent_bridge_notify_state(agent_bridge_t *bridge, const char *device_name) {
    if (!bridge || !device_name) return;

    agent_device_t *dev = find_device(bridge, device_name);
    if (!dev) return;

    if (bridge->transport && bridge->transport->send) {
        char state_msg[512];
        json_writer_t jw;
        jw_init(&jw, state_msg, sizeof(state_msg));

        /* 发送设备状态更新 (MQTT 或 WebSocket 格式) */
        jw_append(&jw,
            "{\"type\":\"state_update\","
            "\"device\":\"%s\","
            "\"state\":%s}",
            device_name,
            dev->state_cache[0] ? dev->state_cache : "{}");

        bridge->transport->send(state_msg);
    }
}

const char *agent_bridge_version(void) {
    return "0.1.0";
}
