/**
 * 示例: AgentBridge 两种传输模式对比
 *
 * 同一套设备代码, 切换传输后端只需改一行:
 *
 *   // 方案 A: ESP32 跑 HTTP Server, Agent 通过 HTTP POST 控制
 *   transport_dify_http_t *http = transport_dify_http_create(bridge, 8080);
 *   transport_dify_http_start(http);
 *
 *   // 方案 B: 串口透传, PC 端跑 dify_bridge.py 桥接
 *   transport_serial_t serial;
 *   transport_serial_init(&serial, bridge, 115200);
 *
 *   设备定义完全不变:
 *     agent_bridge_register_device(bridge, &fan_relay.device);
 *     agent_bridge_register_device(bridge, &light_relay.device);
 */

#include "agent_bridge.h"
#include "devices/relay.h"
#include "transport/transport_dify_http.h"
#include "transport/transport_serial.h"
#include "transport/transport_mqtt_ha.h"
#include "transport/transport_websocket.h"
#include "transport/transport_tcp_raw.h"
#include "transport/transport_coap.h"
#include "transport/transport_modbus.h"
#include "transport/transport_espnow.h"
#include "transport/transport_ble.h"

#include <stdio.h>

/* ---- 平台头文件 (ESP32 示例) ---- */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#define FAN_RELAY_GPIO    4
#define LIGHT_RELAY_GPIO  5

/* ---- GPIO 适配 ---- */
#undef GPIO_WRITE
#define GPIO_WRITE(pin, level)  gpio_set_level((gpio_num_t)(pin), (level))

static void hw_gpio_init(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << FAN_RELAY_GPIO) | (1ULL << LIGHT_RELAY_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

static void log_callback(const char *level, const char *msg) {
    printf("[AgentBridge/%s] %s\n", level, msg);
}

/* ================================================================
 *  设备定义 (无论用哪种传输, 这段代码完全不变)
 * ================================================================ */
static relay_t fan_relay;
static relay_t light_relay;

static void register_devices(agent_bridge_t *bridge) {
    relay_init(&fan_relay, FAN_RELAY_GPIO, RELAY_ACTIVE_HIGH,
               "fan", "Living Room Fan");
    relay_init(&light_relay, LIGHT_RELAY_GPIO, RELAY_ACTIVE_HIGH,
               "light", "Living Room Light");

    agent_bridge_register_device(bridge, &fan_relay.device);
    agent_bridge_register_device(bridge, &light_relay.device);

    printf("Devices registered: fan, light\n");
}

/* ================================================================
 *  main — 四通道同时运行
 * ================================================================ */
void app_main(void) {
    hw_gpio_init();
    agent_cfg_t cfg = { .log_cb = log_callback };
    agent_bridge_t *bridge = agent_bridge_init(&cfg);
    register_devices(bridge);

    /* 软件层 (无需额外硬件) */
    transport_dify_http_start(transport_dify_http_create(bridge, 8080));
    transport_websocket_start(transport_websocket_create(bridge, "ws://pc:8080", "esp32_001", true));
    transport_mqtt_ha_start(transport_mqtt_ha_create(bridge, "mqtt://ha.local:1883", "esp32_lr", NULL, NULL, "ab"));
    transport_tcp_raw_start(transport_tcp_raw_create(bridge, 9000));
    transport_coap_start(transport_coap_create(bridge, 5683));

    /* 硬件层 (需相应外设) */
    /* transport_modbus_start(transport_modbus_create(bridge, 2, 17, 18, 19, 9600)); */
    /* transport_espnow_start(transport_espnow_create(bridge, NULL)); */
    /* transport_ble_start(transport_ble_create(bridge, "AgentBridge")); */

    transport_serial_t serial;
    transport_serial_init(&serial, bridge, 115200);

    printf("AgentBridge: 9 transports available, 5 active\n");
    while (1) { agent_bridge_task(bridge); vTaskDelay(pdMS_TO_TICKS(10)); }
}
