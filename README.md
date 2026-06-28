# AgentBridge

> MCU 做 Agent I/O 前端 — 让你的硬件能被 AI 语音/文字控制

[![Version](https://img.shields.io/badge/version-0.1.0-blue)](firmware/agent_bridge.h)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)

## 这是什么

AgentBridge 是一个**嵌入式 C 库**, 解决一个具体问题:

> 你有一个 ESP32/GD32/STM32 设备 (控制灯、风扇、传感器),
> 想让 Claude/DeepSeek/Dify/Home Assistant 等 AI Agent 发现并控制它。

现有方案: 每个 Agent 后端写一套协议适配代码 (MCP Server、MQTT topic、HTTP handler)。  
AgentBridge: **定义一次设备, 自动适配所有 Agent 后端。**

## 5 分钟理解

```c
// 定义设备
relay_t light;
relay_init(&light, GPIO_NUM_4, RELAY_ACTIVE_HIGH, "light", "客厅灯");

// 注册到 bridge → 自动生成:
//   - tool: "light.set_power"   (Agent 可调用)
//   - tool: "light.get_state"   (Agent 可查询)
agent_bridge_register_device(bridge, &light.device);

// 用户说: "打开客厅灯" → Claude → MCP → WebSocket → ESP32 → GPIO → 继电器
```

## 项目结构

- [`firmware/`](firmware/) — AgentBridge C 库 (核心协议栈)
- [`docs/`](docs/) — 设计文档
- [`项目规划书.md`](项目规划书.md) — 完整三年规划

## 当前状态: v0.1

- [x] 核心 API 设计
- [x] 设备抽象层 (继电器示例)
- [x] 串口传输层
- [x] 设备自动发现 (JSON)
- [x] MCP 工具列表生成
- [ ] MCP WebSocket 传输层 (基于小智代码)
- [ ] MQTT 传输层
- [ ] 传感器设备驱动 (DHT22)
- [ ] 实际硬件验证

## 下一步

1. 用 AgentBridge API 重写 `phase7-ai-fan-voice/main/boards/waveshare/esp32-s3-touch-lcd-4.3c/esp32-s3-touch-lcd-4.3c.cc` 中的 `InitializeTools()`
2. 烧录验证: 小智语音控制风扇, 通过 AgentBridge 协议栈
3. 添加第二个 Agent 后端 (Dify HTTP), 验证设备代码无需改动

## 许可证

MIT License
