#include "servo.h"
#include <string.h>
#include <stdio.h>
#ifndef SERVO_WRITE
#define SERVO_WRITE(pin, angle) /* 平台适配: 0-180 度 */
#endif

static int servo_on(void *ctx, bool on) { (void)ctx; (void)on; return 0; }
static int servo_set_pos(void *ctx, int16_t a) { servo_t *s=ctx; if(a<s->min_angle)a=s->min_angle; if(a>s->max_angle)a=s->max_angle; s->angle=a; SERVO_WRITE(s->pwm_pin,a); return 0; }
static int servo_get_state(void *ctx, char *b, size_t l) { servo_t *s=ctx; return snprintf(b,l,"{\"position\":%d,\"online\":true}",s->angle); }

void servo_init(servo_t *s, int pp, int16_t mina, int16_t maxa, const char *n, const char *d) {
    memset(s,0,sizeof(*s)); s->pwm_pin=pp; s->min_angle=mina; s->max_angle=maxa; s->angle=(mina+maxa)/2;
    s->device.name=n; s->device.display_name=d; s->device.description="Servo motor position control";
    s->device.caps=AB_CAP_POSITION; s->device.hw_ctx=s;
    s->device.ops.on=servo_on; s->device.ops.set_position=servo_set_pos; s->device.ops.get_state=servo_get_state;
}
