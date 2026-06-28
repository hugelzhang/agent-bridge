/** PWM Fan — 无极调速风扇 | platform: any MCU with PWM | adapter: PWM_WRITE(pin, duty_0_100) */
#ifndef AGENT_DEVICE_PWM_FAN_H
#define AGENT_DEVICE_PWM_FAN_H
#include "../agent_bridge.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { agent_device_t device; int pwm_pin; int tach_pin; uint8_t speed; bool on; int rpm; } pwm_fan_t;
void pwm_fan_init(pwm_fan_t *fan, int pwm_pin, int tach_pin, const char *name, const char *display);
#ifdef __cplusplus
}
#endif
#endif
