/** RGB LED — 全彩灯 | adapter: RGB_WRITE(r_pin, g_pin, b_pin, r, g, b) */
#ifndef AGENT_DEVICE_RGB_LED_H
#define AGENT_DEVICE_RGB_LED_H
#include "../agent_bridge.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { agent_device_t device; int r_pin, g_pin, b_pin; uint8_t r, g, b; bool on; } rgb_led_t;
void rgb_led_init(rgb_led_t *led, int rp, int gp, int bp, const char *name, const char *display);
#ifdef __cplusplus
}
#endif
#endif
