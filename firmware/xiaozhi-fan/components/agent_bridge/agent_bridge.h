/**
 * AgentBridge — MCU 端 Agent I/O 通用协议栈
 *
 * 定位: 让你的 MCU 设备能被任何 Agent (Claude/DeepSeek/Dify/HA) 自动发现和控制。
 *       一套设备代码，多协议后端可切换。
 *
 * 用法:
 *   1. agent_bridge_init(&cfg)               初始化
 *   2. agent_bridge_register_device(&fan_dev) 注册设备
 *   3. agent_bridge_set_transport(&mcp_ws)    选择通信后端
 *   4. agent_bridge_task()                    主循环 (每 tick 调一次)
 *
 * 许可证: MIT
 * 版本:   0.1.0
 */

#ifndef AGENT_BRIDGE_H
#define AGENT_BRIDGE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 *  版本
 * ================================================================ */
#define AGENT_BRIDGE_VERSION_MAJOR 0
#define AGENT_BRIDGE_VERSION_MINOR 1
#define AGENT_BRIDGE_VERSION_PATCH 0

/* ================================================================
 *  设备能力枚举
 * ================================================================ */
typedef enum {
    AB_CAP_NONE        = 0,
    AB_CAP_ON_OFF      = (1 << 0),   /* 开关 (on/off) */
    AB_CAP_LEVEL       = (1 << 1),   /* 无极调速/调光 (0-100%) */
    AB_CAP_READ_SENSOR = (1 << 2),   /* 传感器读取 (temperature, humidity, ...) */
    AB_CAP_POSITION    = (1 << 3),   /* 位置控制 (舵机/步进: 0-180°) */
    AB_CAP_COLOR       = (1 << 4),   /* 颜色控制 (RGB/HSV) */
} agent_capability_t;

/* ================================================================
 *  设备状态键 (用于 get_state 返回的 JSON)
 * ================================================================ */
#define AB_STATE_POWER       "power"        /* bool   - 开关状态 */
#define AB_STATE_LEVEL       "level"        /* int    - 0-100 百分比 */
#define AB_STATE_TEMPERATURE "temperature"  /* float  - 摄氏度 */
#define AB_STATE_HUMIDITY    "humidity"     /* float  - 百分比 */
#define AB_STATE_POSITION    "position"     /* int    - 0-180 角度 */
#define AB_STATE_ONLINE      "online"       /* bool   - 设备是否在线 */

/* ================================================================
 *  前向声明
 * ================================================================ */
typedef struct agent_bridge   agent_bridge_t;
typedef struct agent_device   agent_device_t;
typedef struct agent_transport agent_transport_t;
typedef struct agent_cfg      agent_cfg_t;

/* ================================================================
 *  设备操作接口 (由硬件层实现)
 * ================================================================ */

/** 设备操作函数表 */
typedef struct {
    /**
     * 执行开关操作
     * @param ctx  设备私有上下文 (GPIO 引脚等)
     * @param on   true=开, false=关
     * @return     0=成功, 其他=错误码
     */
    int (*on)(void *ctx, bool on_off);

    /**
     * 设置输出等级 (PWM/调速/调光)
     * @param ctx  设备私有上下文
     * @param pct  0-100 (0=关, 100=全速/最亮)
     * @return     0=成功
     */
    int (*set_level)(void *ctx, uint8_t pct);

    /**
     * 获取设备当前状态, 输出为 JSON 片段
     * 例如: {"power":true,"level":75}
     * @param ctx  设备私有上下文
     * @param buf  输出缓冲区
     * @param len  缓冲区大小
     * @return     实际写入字节数 (不含 '\0'), 或 -1 表示失败
     */
    int (*get_state)(void *ctx, char *buf, size_t len);

    /**
     * 设置位置 (舵机/步进)
     * @param ctx   设备私有上下文
     * @param angle 角度值, 语义由设备定义
     * @return      0=成功
     */
    int (*set_position)(void *ctx, int16_t angle);

    /**
     * 设置颜色 (RGB LED)
     * @param ctx  设备私有上下文
     * @param r,g,b 0-255
     * @return     0=成功
     */
    int (*set_color)(void *ctx, uint8_t r, uint8_t g, uint8_t b);
} agent_device_ops_t;

/* ================================================================
 *  设备定义 (由应用层填充, 注册到 bridge)
 * ================================================================ */

struct agent_device {
    const char          *name;         /* 唯一标识, 如 "living_room_fan" */
    const char          *display_name; /* 人类可读, 如 "客厅风扇" */
    const char          *description;  /* Agent 用, 如 "控制客厅风扇开关和调速" */
    agent_capability_t   caps;         /* 能力位掩码 */
    agent_device_ops_t   ops;          /* 操作函数 */
    void                *hw_ctx;       /* 硬件上下文 (传给 ops) */
    uint32_t             poll_ms;      /* 传感器轮询间隔 (ms), 0=不轮询 */

    /* 以下由 bridge 内部使用 */
    agent_bridge_t      *bridge;
    uint32_t             last_poll_tick;
    char                 state_cache[256];
};

/* ================================================================
 *  传输层接口 (由通信层实现: MCP WebSocket / MQTT / 串口)
 * ================================================================ */

/** Agent 下发命令的回调: (tool_name, json_args, user_data) */
typedef void (*agent_cmd_cb_t)(const char *tool_name,
                               const char *json_args,
                               void *user_data);

struct agent_transport {
    const char *name;   /* "mcp_ws", "mqtt", "serial_at" */

    /**
     * 连接到 Agent 后端
     * @param uri  连接地址 (ws://... / mqtt://... / COM3)
     * @return     0=成功
     */
    int (*connect)(const char *uri);

    /**
     * 发送 JSON 消息给 Agent
     * @param json_str  完整的 JSON 字符串
     * @return          0=成功
     */
    int (*send)(const char *json_str);

    /**
     * 接收消息 (非阻塞)
     * @param buf        接收缓冲区
     * @param len        缓冲区大小
     * @param timeout_ms 超时 (0=立即返回)
     * @return           >0=收到的字节数, 0=无数据, -1=错误
     */
    int (*recv)(char *buf, size_t len, uint32_t timeout_ms);

    /**
     * 注册收到 Agent 命令时的回调
     */
    void (*on_command)(agent_cmd_cb_t cb, void *user_data);

    /** 断开连接 */
    void (*disconnect)(void);

    /** 传输层私有上下文 */
    void *ctx;
};

/* ================================================================
 *  Bridge 配置
 * ================================================================ */

struct agent_cfg {
    /** 设备能力变更时回调 (可选) */
    void (*on_device_list_changed)(void *user_data);
    void *user_data;

    /** 调试日志回调 (可选) */
    void (*log_cb)(const char *level, const char *msg);
};

/* ================================================================
 *  AgentBridge 公共 API
 * ================================================================ */

/**
 * 初始化 AgentBridge
 * @param cfg  配置 (可为 NULL 使用默认)
 * @return     bridge 实例
 */
agent_bridge_t *agent_bridge_init(const agent_cfg_t *cfg);

/**
 * 去初始化, 释放资源
 */
void agent_bridge_deinit(agent_bridge_t *bridge);

/**
 * 注册一个设备
 *
 * 注册后设备能力自动生成 MCP-tool-compatible JSON schema,
 * Agent 端可以发现并调用。
 *
 * @param bridge  bridge 实例
 * @param dev     设备定义 (bridge 会深拷贝 name/display_name/description)
 * @return        0=成功, -1=重复名称
 */
int agent_bridge_register_device(agent_bridge_t *bridge, agent_device_t *dev);

/**
 * 注销设备
 * @return 0=成功, -1=未找到
 */
int agent_bridge_unregister_device(agent_bridge_t *bridge, const char *name);

/**
 * 设置传输层 (切换 Agent 后端)
 * 会断开当前连接, 使用新 transport 重连
 */
int agent_bridge_set_transport(agent_bridge_t *bridge,
                               agent_transport_t *transport,
                               const char *uri);

/**
 * 分发 tool call 到设备 (供外部 transport adapter 调用)
 *
 * @param bridge     bridge 实例
 * @param tool_name  工具名, 格式: "device_name.action" (如 "fan.set_power")
 * @param args_json  参数 JSON (如 '{"power":true}')
 * @param result_buf 结果输出缓冲区
 * @param result_len 缓冲区大小
 * @return           >0=写入字节数, 0=无输出, -1=失败
 */
int agent_bridge_dispatch_tool(agent_bridge_t *bridge,
                               const char *tool_name,
                               const char *args_json,
                               char *result_buf, size_t result_len);

/**
 * 主循环 — 每 tick 调用一次
 *
 * 做的事:
 *   1. 处理收到的 Agent 命令 (tool call → 设备操作)
 *   2. 轮询传感器设备 (按 poll_ms 周期)
 *   3. 上报状态变化
 *
 * 在 FreeRTOS 中放到一个 task 里调用; 在裸机中放到 while(1) superloop.
 */
void agent_bridge_task(agent_bridge_t *bridge);

/**
 * 手动触发某个设备状态上报 (设备驱动检测到变化时调用)
 */
void agent_bridge_notify_state(agent_bridge_t *bridge, const char *device_name);

/**
 * 获取设备列表的 JSON (MCP tools/list 格式)
 * @param buf  输出缓冲区
 * @param len  缓冲区大小
 * @return     实际写入字节数
 */
int agent_bridge_get_tools_json(agent_bridge_t *bridge, char *buf, size_t len);

/**
 * 获取设备能力描述的 JSON (设备发现)
 * @return 实际写入字节数
 */
int agent_bridge_get_device_list_json(agent_bridge_t *bridge, char *buf, size_t len);

/**
 * 获取 OpenAPI 3.0 规范 JSON (Dify / 标准 Agent 可直接导入)
 * @return 实际写入字节数
 */
int agent_bridge_get_openapi_json(agent_bridge_t *bridge,
                                  const char *server_url,
                                  char *buf, size_t len);

/**
 * 获取版本字符串
 */
const char *agent_bridge_version(void);

#ifdef __cplusplus
}
#endif

#endif /* AGENT_BRIDGE_H */
