/** Servo — 舵机角度控制 | adapter: SERVO_WRITE(pin, angle_0_180) */
#ifndef AGENT_DEVICE_SERVO_H
#define AGENT_DEVICE_SERVO_H
#include "../agent_bridge.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { agent_device_t device; int pwm_pin; int16_t angle; int16_t min_angle; int16_t max_angle; } servo_t;
void servo_init(servo_t *s, int pwm_pin, int16_t min_angle, int16_t max_angle, const char *name, const char *display);
#ifdef __cplusplus
}
#endif
#endif
