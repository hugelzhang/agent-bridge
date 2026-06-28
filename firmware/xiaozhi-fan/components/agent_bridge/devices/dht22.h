/** DHT22 — 温湿度传感器 | adapter: DHT_READ(pin, &temp, &hum) → 0=ok */
#ifndef AGENT_DEVICE_DHT22_H
#define AGENT_DEVICE_DHT22_H
#include "../agent_bridge.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { agent_device_t device; int data_pin; float temperature; float humidity; } dht22_t;
void dht22_init(dht22_t *d, int data_pin, const char *name, const char *display);
#ifdef __cplusplus
}
#endif
#endif
