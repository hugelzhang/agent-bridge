/**
 * 示例: 使用 AgentBridge 重构 AI 风扇
 *
 * 对比现有代码 (esp32-s3-touch-lcd-4.3c.cc InitializeTools):
 *
 *   旧代码:                             新代码:
 *   ─────────────────────────────────   ─────────────────────────────────
 *   McpServer::GetInstance()            agent_bridge_init(&cfg)
 *     .AddTool("self.fan.set_speed",     agent_bridge_register_device(
 *       ..., [](properties) {                bridge, &fan.device);
 *         uart_proto_set_fan(...);
 *       });                             agent_bridge_set_transport(
 *                                          bridge, &mcp_ws, "ws://...");
 *   McpServer::GetInstance()
 *     .AddTool("self.fan.set_light",    agent_bridge_task(bridge);
 *       ...);
 *
 *   关键变化:
 *     1. 设备定义和通信协议解耦
 *     2. 新增设备只需填充 agent_device_t, 不用关心 MCP/JSON 细节
 *     3. 换 Agent 后端只需换 transport, 设备代码不动
 */

#include "agent_bridge.h"
#include "devices/relay.h"

/* ---- 平台头文件 (ESP32 示例) ---- */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

/* ---- 硬件定义 ---- */
#define FAN_RELAY_GPIO    4
#define LIGHT_RELAY_GPIO  5
#define LED_GPIO          2

/* ================================================================
 *  1. 硬件操作实现 (适配 esp-idf)
 * ================================================================ */

/* GPIO 写操作 — relay.c 中通过 GPIO_WRITE 宏调用 */
#undef GPIO_WRITE
#define GPIO_WRITE(pin, level)  gpio_set_level((gpio_num_t)(pin), (level))

/* 初始化 GPIO */
static void hw_gpio_init(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << FAN_RELAY_GPIO) |
                        (1ULL << LIGHT_RELAY_GPIO) |
                        (1ULL << LED_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

/* ================================================================
 *  2. 创建代理设备 (使用 relay 驱动)
 * ================================================================ */

static relay_t fan_relay;    /* 风扇开关 */
static relay_t light_relay;  /* 灯光开关 */
/* 风扇调速需要 UART 协议或 PWM, 这里展示用 relay 做 on/off */
/* 复杂设备可以自定义 agent_device_t 填充更复杂的 ops */

/* ================================================================
 *  3. 日志回调
 * ================================================================ */

static void log_callback(const char *level, const char *msg) {
    printf("[AgentBridge/%s] %s\n", level, msg);
}

/* ================================================================
 *  4. 主任务
 * ================================================================ */

void agent_bridge_task_main(void *arg) {
    (void)arg;

    /* ---- 4a. 硬件初始化 ---- */
    hw_gpio_init();

    /* ---- 4b. 初始化设备 ---- */
    relay_init(&fan_relay, FAN_RELAY_GPIO, RELAY_ACTIVE_HIGH,
               "fan", "Living Room Fan");
    relay_init(&light_relay, LIGHT_RELAY_GPIO, RELAY_ACTIVE_HIGH,
               "light", "Living Room Light");

    /* ---- 4c. 初始化 AgentBridge ---- */
    agent_cfg_t cfg = {
        .log_cb    = log_callback,
        .user_data = NULL,
    };
    agent_bridge_t *bridge = agent_bridge_init(&cfg);

    /* ---- 4d. 注册设备 ---- */
    agent_bridge_register_device(bridge, &fan_relay.device);
    agent_bridge_register_device(bridge, &light_relay.device);

    /* ---- 4e. 上电广播设备能力 (串口) ---- */
    char dev_list[1024];
    agent_bridge_get_device_list_json(bridge, dev_list, sizeof(dev_list));
    printf("%s\n", dev_list);

    /* ---- 4f. 主循环 ---- */
    while (1) {
        agent_bridge_task(bridge);

        /* 检查串口输入 (非阻塞) */
        /* transport_serial_feed(&serial, uart_read_byte()); */

        vTaskDelay(pdMS_TO_TICKS(10));  /* ~100Hz tick */
    }
}

/* ---- 入口 ---- */
void app_main(void) {
    xTaskCreate(agent_bridge_task_main, "agent_bridge",
                8192, NULL, 5, NULL);
}
