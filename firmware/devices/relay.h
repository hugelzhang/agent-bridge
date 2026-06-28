/**
 * 示例设备: 继电器开关
 *
 * 用法:
 *   relay_t relay = relay_create(GPIO_NUM_4, true);  // GPIO4, 低电平触发
 *   agent_bridge_register_device(bridge, &relay.device);
 */

#ifndef AGENT_DEVICE_RELAY_H
#define AGENT_DEVICE_RELAY_H

#include "../agent_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 继电器触发方式 */
typedef enum {
    RELAY_ACTIVE_HIGH = 0,  /* 高电平吸合 */
    RELAY_ACTIVE_LOW  = 1,  /* 低电平吸合 (常用) */
} relay_active_t;

/** 继电器设备上下文 */
typedef struct {
    agent_device_t  device;       /* 基类, 注册到 bridge */
    int             gpio_pin;     /* GPIO 引脚号 */
    relay_active_t  active_level; /* 有效电平 */
    bool            state;        /* 当前状态 */
} relay_t;

/**
 * 初始化继电器设备
 * @param relay        设备指针
 * @param gpio_pin     GPIO 引脚 (如 4)
 * @param active_level 有效电平
 * @param name         设备名 (如 "living_room_light")
 * @param display_name 显示名 (如 "客厅灯")
 */
void relay_init(relay_t *relay, int gpio_pin, relay_active_t active_level,
                const char *name, const char *display_name);

#ifdef __cplusplus
}
#endif

#endif /* AGENT_DEVICE_RELAY_H */
