# AgentBridge Protocol Specification v0.1

> **RFC-style white paper — 任何人都能基于本文档实现 AgentBridge 协议**

## Abstract

AgentBridge is a lightweight JSON-based protocol for connecting IoT/MCU devices to AI Agents. A device registers its capabilities once; the agent auto-discovers them as callable tools. Multiple transport layers (HTTP, MQTT, WebSocket, BLE, CoAP, ...) can operate concurrently on the same device.

## 1. Protocol Layers

```
┌─────────────────────────────────┐
│  Application: Device Ops        │  on/off, set_level, get_state
├─────────────────────────────────┤
│  Presentation: AgentBridge JSON │  tool schema, dispatch, state
├─────────────────────────────────┤
│  Session: Auth (JWT HS256)      │  device identity + token
├─────────────────────────────────┤
│  Transport: HTTP/WS/MQTT/...    │  any reliable message channel
└─────────────────────────────────┘
```

## 2. Device Model

### 2.1 Capabilities

```
AB_CAP_ON_OFF      = 0x01  // Switch (relay, power)
AB_CAP_LEVEL       = 0x02  // Continuous level (PWM, dimmer)
AB_CAP_READ_SENSOR = 0x04  // Sensor read (temperature, humidity)
AB_CAP_POSITION    = 0x08  // Position control (servo)
AB_CAP_COLOR       = 0x10  // Color control (RGB LED)
```

### 2.2 Device Structure (C)

```c
typedef struct {
    const char          *name;         // e.g. "living_room_fan"
    const char          *display_name; // e.g. "Living Room Fan"
    const char          *description;  // For Agent context
    agent_capability_t   caps;         // Bitmask of capabilities
    agent_device_ops_t   ops;          // Hardware operation callbacks
    void                *hw_ctx;       // Hardware context (GPIO pin, etc.)
} agent_device_t;
```

### 2.3 Operations

```c
typedef struct {
    int (*on)(void *ctx, bool on_off);
    int (*set_level)(void *ctx, uint8_t pct);       // 0-100
    int (*get_state)(void *ctx, char *buf, size_t len);
    int (*set_position)(void *ctx, int16_t angle);
    int (*set_color)(void *ctx, uint8_t r, uint8_t g, uint8_t b);
} agent_device_ops_t;
```

## 3. Tool Naming Convention

Tools are auto-generated from device capabilities:

| Capability | Tool Name | Arguments |
|-----------|-----------|-----------|
| ON_OFF | `<device>.set_power` | `{"power": true/false}` |
| LEVEL | `<device>.set_level` | `{"level": 0-100}` |
| ALL | `<device>.get_state` | `{}` |
| POSITION | `<device>.set_position` | `{"position": angle}` |
| COLOR | `<device>.set_color` | `{"color": "#RRGGBB"}` |

## 4. Message Format

### 4.1 JSON-RPC 2.0 (Primary)

All Agent↔Device communication uses JSON-RPC 2.0:

**Device → Agent (tools/list):**
```json
{
    "jsonrpc": "2.0",
    "method": "tools/list",
    "params": {
        "tools": [
            {
                "name": "fan.set_power",
                "description": "Turn Living Room Fan on or off",
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "power": {"type": "boolean"}
                    },
                    "required": ["power"]
                }
            }
        ]
    },
    "id": 1
}
```

**Agent → Device (tools/call):**
```json
{
    "jsonrpc": "2.0",
    "method": "tools/call",
    "params": {
        "name": "fan.set_power",
        "arguments": {"power": true}
    },
    "id": 2
}
```

**Device → Agent (result):**
```json
{
    "jsonrpc": "2.0",
    "result": {
        "content": [{"type": "text", "text": "{\"power\":true}"}],
        "isError": false
    },
    "id": 2
}
```

### 4.2 HTTP/CoAP (REST)

| Method | Path | Body | Response |
|--------|------|------|----------|
| GET | `/tools` | — | `[{name, description, inputSchema}, ...]` |
| POST | `/call` | `{"name":"...", "arguments":{...}}` | `{"content":[...], "isError":false}` |
| GET | `/health` | — | `{"status":"ok"}` |
| GET | `/openapi.json` | — | OpenAPI 3.0 spec |

### 4.3 MQTT (Home Assistant)

| Direction | Topic | Payload |
|-----------|-------|---------|
| Device→HA | `homeassistant/<type>/agent_<dev>/config` | HA discovery JSON (retained) |
| Device→HA | `<prefix>/<device>/state` | `{"power": "ON", "level": 75}` |
| HA→Device | `<prefix>/<device>/set` | `ON` / `OFF` / `{"brightness": 128}` |

### 4.4 WebSocket (MCP)

Same JSON-RPC 2.0 format as §4.1, transmitted over WebSocket frames.

## 5. Security

### 5.1 Authentication (JWT HS256)

Every message carries a `auth_token` field:

```json
{
    "name": "fan.set_power",
    "arguments": {"power": true},
    "auth_token": "eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOi..."
}
```

Token structure:
```
base64url(header).base64url(payload).base64url(signature)

header:  {"alg":"HS256","typ":"JWT"}
payload: {"sub":"<device_id>","iat":<ts>,"exp":<ts>,"jti":"<nonce>"}
```

### 5.2 Key Rotation

- Default token TTL: 24 hours
- Server rejects expired tokens (HTTP 401)
- Device auto-refreshes before expiry
- Key rotation invalidates all outstanding tokens

### 5.3 TLS

All transports SHOULD use TLS in production:
- HTTP → HTTPS (port 443)
- WebSocket → WSS (port 443)
- MQTT → MQTTS (port 8883)

Certificate verification RECOMMENDED. Skip only in development.

## 6. Transport Matrix

| Transport | Port | Latency | Power | Range | Use Case |
|-----------|------|---------|-------|-------|----------|
| HTTP | 80/8080 | Low | Medium | LAN/WAN | Dify, curl, REST clients |
| WebSocket | 80/8080 | Very Low | Medium | LAN/WAN | Claude MCP, real-time Agent |
| MQTT | 1883 | Low | Low | LAN/WAN | Home Assistant, IoT cloud |
| TCP Raw | 9000 | Very Low | Low | LAN | Telnet, custom clients |
| CoAP | 5683 | Low | Very Low | LAN/WAN | NB-IoT, 6LoWPAN, satellite |
| Serial/UART | — | Very Low | Very Low | <2m | PC bridge, debug |
| BLE | — | Medium | Very Low | <30m | Phone direct control |
| ESP-NOW | — | Very Low | Very Low | <200m | Mesh sensor network |
| Modbus RTU | — | Low | Low | <1200m | Industrial PLC, meters |

## 7. Device Discovery

### 7.1 Zero-Configuration

On boot, the device broadcasts its device list via all active transports:

```
Serial: {"type":"device_list","devices":[...]}
MQTT:   homeassistant/<type>/agent_<dev>/config (retained)
HTTP:   GET /openapi.json (OpenAPI 3.0)
WebSocket: {"jsonrpc":"2.0","method":"tools/list",...}
```

### 7.2 OpenAPI 3.0 Import

`GET /openapi.json` returns a standard OpenAPI 3.0 document with all device operations enumerated. Compatible with Dify, Postman, and any OpenAPI-compatible tool.

## 8. OTA Firmware Upgrade

```
POST /api/ota/upgrade
{
    "url": "http://server/firmware.bin",
    "sha256": "a1b2c3..."
}
```

- Device downloads firmware via HTTP
- Validates SHA256 checksum
- Writes to inactive OTA partition
- Sets boot partition flag
- Reboots into new firmware
- On boot failure: automatic rollback to previous version

## 9. Reference Implementation

- **C library**: `agent_bridge.c/h` — core protocol stack (827 lines)
- **Platform**: ESP32-S3 (primary), portable to any C99 MCU
- **Repository**: https://github.com/hugelzhang/agent-bridge
- **License**: MIT

## 10. Version History

| Version | Date | Changes |
|---------|------|---------|
| 0.1 | 2026-06-29 | Initial specification. 10 transports, JWT auth, OTA. |

---

*This document defines the AgentBridge protocol. Implementations MUST follow §2-§5 for interoperability. §6-§8 are RECOMMENDED extensions.*
