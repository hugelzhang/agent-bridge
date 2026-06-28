#include "dht22.h"
#include <string.h>
#include <stdio.h>
#ifndef DHT_READ
#define DHT_READ(pin, t, h) (-1) /* 平台适配: 返回 0=ok */
#endif

static int dht_get_state(void *ctx, char *b, size_t l) {
    dht22_t *d=ctx;
    float t=25.0f, h=60.0f;
    int ret=DHT_READ(d->data_pin,&t,&h);
    if(ret==0) { d->temperature=t; d->humidity=h; }
    return snprintf(b,l,"{\"temperature\":%.1f,\"humidity\":%.1f,\"online\":%s,\"sensor_ok\":%s}",
        d->temperature,d->humidity,"true",ret==0?"true":"false");
}

void dht22_init(dht22_t *d, int pin, const char *n, const char *dn) {
    memset(d,0,sizeof(*d)); d->data_pin=pin; d->temperature=25.0f; d->humidity=60.0f;
    d->device.name=n; d->device.display_name=dn; d->device.description="DHT22 temperature and humidity sensor";
    d->device.caps=AB_CAP_READ_SENSOR; d->device.hw_ctx=d; d->device.poll_ms=5000;
    d->device.ops.get_state=dht_get_state;
}
