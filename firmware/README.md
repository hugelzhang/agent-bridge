# AgentBridge

**MCU 端 Agent I/O 通用协议栈**

让你的 ESP32 / GD32 / STM32 设备能被任何 AI Agent (Claude/DeepSeek/Dify/Home Assistant) 自动发现和控制。一套设备代码，多协议后端可切换。

## 核心理念

```
                 ┌──────────────────────┐
                 │   AI Agent           │
                 │ (Claude/DeepSeek/HA) │
                 └──────────┬───────────┘
                            │ MCP / MQTT / HTTP / 串口
                 ┌──────────┴───────────┐
                 │    AgentBridge       │  ← 本仓库
                 │    设备注册 + 协议适配│
                 └──────────┬───────────┘
                            │ GPIO / PWM / ADC / UART
                 ┌──────────┴───────────┐
                 │    物理设备          │
                 │  (灯/风扇/传感器)    │
                 └──────────────────────┘
```

## 快速开始

```c
#include "agent_bridge.h"
#include "devices/relay.h"

void app_main(void) {
    // 1. 初始化硬件
    gpio_init();

    // 2. 创建并注册设备
    relay_t light;
    relay_init(&light, GPIO_NUM_4, RELAY_ACTIVE_HIGH,
               "light", "Living Room Light");

    agent_bridge_t *bridge = agent_bridge_init(NULL);
    agent_bridge_register_device(bridge, &light.device);

    // 3. 设置通信后端 (串口 / MCP WebSocket / MQTT)
    transport_serial_t serial;
    transport_serial_init(&serial, bridge, 115200);
    agent_bridge_set_transport(bridge, &serial.base, NULL);

    // 4. 主循环
    while (1) {
        agent_bridge_task(bridge);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
```

## 目录结构

```
firmware/
├── agent_bridge.h        # 公共 API
├── agent_bridge.c        # 核心实现
├── devices/              # 设备驱动
│   ├── relay.h/c         #   继电器 (开关)
│   └── (更多设备驱动)
├── transport/            # 传输层
│   ├── transport_serial.h/c  # 串口/UART
│   └── (MQTT / WebSocket / ...)
└── examples/             # 示例
    └── fan_bridge_example.c  # AI 风扇 重构示例
```

## 设备能力

| 能力 | 枚举 | Agent 看到的 tool |
|------|------|-------------------|
| 开关 | `AB_CAP_ON_OFF` | `device.set_power({power: true/false})` |
| 调速 | `AB_CAP_LEVEL` | `device.set_level({level: 0-100})` |
| 传感器 | `AB_CAP_READ_SENSOR` | `device.get_state()` |
| 位置 | `AB_CAP_POSITION` | `device.set_position({position: angle})` |
| 颜色 | `AB_CAP_COLOR` | `device.set_color({color: "#FF0000"})` |

## 支持的 Agent 后端

| 后端 | 传输层 | 状态 |
|------|--------|------|
| 小智 AI (MCP) | WebSocket | 规划中 |
| Claude API (MCP) | WebSocket | 规划中 |
| Dify | HTTP REST | 规划中 |
| Home Assistant | MQTT | 规划中 |
| 串口上位机 | UART | ✅ v0.1 |

## 移植到新平台

只需实现 3 个宏:

```c
#define GPIO_WRITE(pin, level)  /* 你的平台 GPIO 写 */
#define AB_MALLOC(size)         /* 你的 malloc */
#define AB_FREE(ptr)            /* 你的 free */
```

## 许可证

MIT
