#include "pwm_fan.h"
#include <string.h>
#include <stdio.h>
#ifndef PWM_WRITE
#define PWM_WRITE(pin, duty) /* 平台适配: duty 0-100 */
#endif
#ifndef GPIO_READ
#define GPIO_READ(pin) 0
#endif

static int fan_on(void *ctx, bool on) { pwm_fan_t *f=ctx; f->on=on; PWM_WRITE(f->pwm_pin, on?(f->speed?f->speed:50):0); return 0; }
static int fan_set_level(void *ctx, uint8_t pct) { pwm_fan_t *f=ctx; f->speed=pct>100?100:pct; f->on=pct>0; PWM_WRITE(f->pwm_pin, f->on?f->speed:0); return 0; }
static int fan_get_state(void *ctx, char *b, size_t l) { pwm_fan_t *f=ctx; f->rpm=GPIO_READ(f->tach_pin)*30; return snprintf(b,l,"{\"power\":%s,\"level\":%d,\"rpm\":%d,\"online\":true}",f->on?"true":"false",f->speed,f->rpm); }

void pwm_fan_init(pwm_fan_t *f, int pp, int tp, const char *n, const char *d) {
    memset(f,0,sizeof(*f)); f->pwm_pin=pp; f->tach_pin=tp;
    f->device.name=n; f->device.display_name=d; f->device.description="PWM Fan with speed control and tachometer feedback";
    f->device.caps=AB_CAP_ON_OFF|AB_CAP_LEVEL; f->device.hw_ctx=f;
    f->device.ops.on=fan_on; f->device.ops.set_level=fan_set_level; f->device.ops.get_state=fan_get_state;
}
