# 踩坑日志

> **项目**: phase7-ai-fan-voice — UART + 音频 + WiFi 共存
> **MCU**: ESP32-S3 (Waveshare 4.3C)
> **日期**: 2026-06-28

## 坑列表（8 个）

| 坑# | 问题 | 状态 |
|-----|------|------|
| #19 | uart_proto.c 编译进固件破坏 AFE | ⚠️ 当前直接用 Level2 ISR，无需 `#ifdef` |
| #20 | UART 在 board 构造函数中 init 太早 | ✅ 移到 `Initialize()` 中 `audio_service_.Start()` 之后 |
| #21 | auto-wake 回调阻塞主循环 | ✅ 移到 `HandleStateChangedEvent` idle 分支 |
| #22 | `flag=0` ≠ 无中断 → 下位机无响应 | ✅ `ESP_INTR_FLAG_LEVEL1 \| ESP_INTR_FLAG_LOWMED` → 最终 Level2 |
| #23 | uart_rx 栈溢出 → 无限重启 | ✅ `TASK_STACK` 2048→4096 + 移除热路径日志 |
| #24 | UART 延迟初始化依赖状态机 → 永不触发 | ✅ 移到 `Initialize()` 中直接初始化 |
| #25 | WiFi `MAX_MODEM` 极度省电 → 频繁掉线 | ✅ 改为 `BALANCED` (MIN_MODEM) |
| #26 | UART `LOWMED` 仍导致音频卡顿 | ✅ 降至 `Level2` 独立中断级别 |

## 最终架构

```
中断级别:
  Level1: I2S(音频) > WiFi > Timer > GPIO
  Level2: UART(风扇控制) — 完全隔离

初始化顺序:
  Application::Initialize()
    ├── audio_service_.Start()   // AFE 音频管线
    └── uart_proto_init()        // UART 风扇 (Level2 ISR)

WiFi 省电:
  active → PERFORMANCE (NONE)    // 零延迟
  idle   → BALANCED (MIN_MODEM)  // 不掉线
```

详见父项目 `docs/phase7-pitfalls-and-fixes.md`。
