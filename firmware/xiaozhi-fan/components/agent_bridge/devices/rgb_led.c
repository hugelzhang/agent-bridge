#include "rgb_led.h"
#include <string.h>
#include <stdio.h>
#ifndef RGB_WRITE
#define RGB_WRITE(rp, gp, bp, r, g, b) /* 平台适配 */
#endif

static int led_on(void *ctx, bool on) { rgb_led_t *l=ctx; l->on=on; RGB_WRITE(l->r_pin,l->g_pin,l->b_pin,on?l->r:0,on?l->g:0,on?l->b:0); return 0; }
static int led_set_color(void *ctx, uint8_t r, uint8_t g, uint8_t b) { rgb_led_t *l=ctx; l->r=r;l->g=g;l->b=b; l->on=true; RGB_WRITE(l->r_pin,l->g_pin,l->b_pin,r,g,b); return 0; }
static int led_get_state(void *ctx, char *b, size_t len) { rgb_led_t *l=ctx; return snprintf(b,len,"{\"power\":%s,\"color\":\"#%02X%02X%02X\",\"online\":true}",l->on?"true":"false",l->r,l->g,l->b); }

void rgb_led_init(rgb_led_t *l, int rp, int gp, int bp, const char *n, const char *d) {
    memset(l,0,sizeof(*l)); l->r_pin=rp; l->g_pin=gp; l->b_pin=bp; l->r=l->g=l->b=255;
    l->device.name=n; l->device.display_name=d; l->device.description="RGB LED with full color control";
    l->device.caps=(agent_capability_t)(AB_CAP_ON_OFF|AB_CAP_COLOR); l->device.hw_ctx=l;
    l->device.ops.on=led_on; l->device.ops.set_color=led_set_color; l->device.ops.get_state=led_get_state;
}
