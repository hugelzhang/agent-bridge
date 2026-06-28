/**
 * 继电器设备驱动 (平台无关接口)
 *
 * 平台适配: 实现 gpio_write(pin, level) 宏即可
 *   - ESP32:  #define gpio_write(pin, level) gpio_set_level(pin, level)
 *   - GD32:   #define gpio_write(pin, level) gpio_bit_write(pin, level)
 *   - 裸机:   #define gpio_write(pin, level) HAL_GPIO_WritePin(...)
 */

#include "relay.h"
#include <string.h>
#include <stdio.h>

/* ---- 平台适配: 修改此宏匹配你的 MCU ---- */
#ifndef GPIO_WRITE
#define GPIO_WRITE(pin, level)  /* 由平台提供 */
#endif

/* ---- 操作实现 ---- */

static int relay_on(void *ctx, bool on_off) {
    relay_t *relay = (relay_t *)ctx;
    int level = (relay->active_level == RELAY_ACTIVE_HIGH) ? on_off : !on_off;
    GPIO_WRITE(relay->gpio_pin, level);
    relay->state = on_off;
    return 0;
}

static int relay_get_state(void *ctx, char *buf, size_t len) {
    relay_t *relay = (relay_t *)ctx;
    return snprintf(buf, len, "{\"power\":%s,\"online\":true}",
                    relay->state ? "true" : "false");
}

/* ---- 初始化 ---- */

void relay_init(relay_t *relay, int gpio_pin, relay_active_t active_level,
                const char *name, const char *display_name) {
    memset(relay, 0, sizeof(*relay));

    relay->gpio_pin     = gpio_pin;
    relay->active_level = active_level;
    relay->state        = false;

    /* 填充 agent_device 基类 */
    relay->device.name         = name;
    relay->device.display_name = display_name;
    relay->device.description  = "On/Off switch (relay)";
    relay->device.caps         = AB_CAP_ON_OFF;
    relay->device.hw_ctx       = relay;
    relay->device.ops.on       = relay_on;
    relay->device.ops.get_state = relay_get_state;
    /* 未使用的 ops 保持 NULL */
}
