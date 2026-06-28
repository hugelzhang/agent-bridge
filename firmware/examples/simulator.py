#!/usr/bin/env python3
"""
AgentBridge Simulator — PC 端全链路模拟器

不依赖任何硬件, 在电脑上跑完整的 Agent 控制流程:
  1. 注册虚拟设备 (风扇、灯、传感器)
  2. 启动 HTTP Server 暴露 /tools /call 端点
  3. 任何 HTTP Agent (Dify/curl/Postman) 都能控制

用法:
  python simulator.py                    # 默认 8080 端口
  python simulator.py --port 9090        # 自定义端口

测试:
  curl http://localhost:8080/tools        # 查看设备列表
  curl -X POST http://localhost:8080/call \
    -H "Content-Type: application/json" \
    -d '{"name":"fan.set_power","arguments":{"power":true}}'
"""

import json
import time
import argparse
import threading
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse


# ═══════════════════════════════════════════
# 虚拟设备 (模拟物理硬件)
# ═══════════════════════════════════════════

class VirtualDevice:
    def __init__(self, name, display_name, description, caps):
        self.name = name
        self.display_name = display_name
        self.description = description
        self.caps = caps  # bitmask
        self.state = {}
        self._init_state()

    def _init_state(self):
        if "on_off" in self.caps:
            self.state["power"] = False
        if "level" in self.caps:
            self.state["level"] = 0
        if "sensor" in self.caps:
            self.state["temperature"] = 25.0
            self.state["humidity"] = 60.0

    def set_power(self, on):
        if "on_off" not in self.caps:
            return False, "device has no on_off capability"
        self.state["power"] = on
        if "level" in self.caps and on and self.state["level"] == 0:
            self.state["level"] = 50  # 默认开50%
        return True, f"{'ON' if on else 'OFF'}"

    def set_level(self, pct):
        if "level" not in self.caps:
            return False, "device has no level capability"
        pct = max(0, min(100, pct))
        self.state["level"] = pct
        self.state["power"] = pct > 0
        return True, f"level={pct}%"

    def get_state(self):
        return dict(self.state)


CAP_ON_OFF = 1 << 0
CAP_LEVEL  = 1 << 1
CAP_SENSOR = 1 << 2


# ═══════════════════════════════════════════
# AgentBridge (设备注册 + 工具分发)
# ═══════════════════════════════════════════

class AgentBridge:
    def __init__(self):
        self.devices = {}

    def register(self, dev):
        self.devices[dev.name] = dev
        print(f"  [Bridge] + {dev.name} ({dev.display_name})")

    def get_tools(self):
        tools = []
        for dev in self.devices.values():
            if "on_off" in dev.caps:
                tools.append({
                    "name": f"{dev.name}.set_power",
                    "description": f"Turn {dev.display_name} on or off",
                    "inputSchema": {
                        "type": "object",
                        "properties": {
                            "power": {"type": "boolean", "description": "true=on, false=off"}
                        },
                        "required": ["power"]
                    }
                })
            if "level" in dev.caps:
                tools.append({
                    "name": f"{dev.name}.set_level",
                    "description": f"Set {dev.display_name} level (0-100)",
                    "inputSchema": {
                        "type": "object",
                        "properties": {
                            "level": {"type": "integer", "minimum": 0, "maximum": 100}
                        },
                        "required": ["level"]
                    }
                })
            tools.append({
                "name": f"{dev.name}.get_state",
                "description": f"Get current state of {dev.display_name}",
                "inputSchema": {"type": "object", "properties": {}, "required": []}
            })
        return tools

    def get_openapi(self, server_url="http://localhost:8080"):
        """生成 OpenAPI 3.0 规范"""
        tool_names = []
        for dev in self.devices.values():
            actions = []
            if "on_off" in dev.caps: actions.append("set_power")
            if "level" in dev.caps: actions.append("set_level")
            actions.append("get_state")
            for a in actions:
                tool_names.append(f"{dev.name}.{a}")
        return {
            "openapi": "3.0.0",
            "info": {
                "title": "AgentBridge Devices",
                "version": "1.0.0",
                "description": "Auto-generated device API"
            },
            "servers": [{"url": server_url}],
            "paths": {
                "/call": {
                    "post": {
                        "operationId": "agent_bridge_call",
                        "summary": "Execute device tool call",
                        "requestBody": {
                            "required": True,
                            "content": {
                                "application/json": {
                                    "schema": {
                                        "type": "object",
                                        "properties": {
                                            "name": {"type": "string",
                                                     "enum": tool_names},
                                            "arguments": {"type": "object"}
                                        },
                                        "required": ["name", "arguments"]
                                    }
                                }
                            }
                        },
                        "responses": {
                            "200": {"description": "Tool result"}
                        }
                    }
                },
                "/tools": {
                    "get": {
                        "operationId": "list_tools",
                        "summary": "List all tools",
                        "responses": {"200": {"description": "Tool list"}}
                    }
                }
            }
        }

    def dispatch(self, tool_name, arguments):
        parts = tool_name.rsplit(".", 1)
        if len(parts) != 2:
            return {"error": f"invalid tool name: {tool_name}"}
        dev_name, action = parts

        dev = self.devices.get(dev_name)
        if not dev:
            return {"error": f"device not found: {dev_name}"}

        if action == "set_power":
            ok, msg = dev.set_power(arguments.get("power", False))
            return {"success": ok, "state": dev.get_state(), "message": msg}
        elif action == "set_level":
            ok, msg = dev.set_level(arguments.get("level", 0))
            return {"success": ok, "state": dev.get_state(), "message": msg}
        elif action == "get_state":
            return {"state": dev.get_state()}
        else:
            return {"error": f"unsupported action: {action}"}


# ═══════════════════════════════════════════
# HTTP Server
# ═══════════════════════════════════════════

class BridgeHandler(BaseHTTPRequestHandler):
    bridge: AgentBridge = None

    def log_message(self, fmt, *args):
        print(f"  [HTTP] {args[0]}")

    def _json(self, data, status=200):
        body = json.dumps(data, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = urlparse(self.path).path
        if path == "/tools":
            self._json(self.bridge.get_tools())
        elif path == "/openapi.json":
            self._json(self.bridge.get_openapi())
        elif path == "/health":
            self._json({"status": "ok"})
        else:
            self._json({"error": "not found", "try": "GET /tools"}, 404)

    def do_POST(self):
        path = urlparse(self.path).path
        if path != "/call":
            self._json({"error": "not found"}, 404)
            return
        try:
            length = int(self.headers.get("Content-Length", 0))
            body = json.loads(self.rfile.read(length))
        except (ValueError, json.JSONDecodeError):
            self._json({"error": "invalid JSON"}, 400)
            return

        name = body.get("name", "")
        args = body.get("arguments", {})
        print(f"\n  >>> {name}({json.dumps(args)})")
        result = self.bridge.dispatch(name, args)
        print(f"  <<< {json.dumps(result, ensure_ascii=False)}")
        self._json(result)

    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()


# ═══════════════════════════════════════════
# 温度模拟 (后台线程, 每5秒更新传感器)
# ═══════════════════════════════════════════

def sensor_simulator(bridge, interval=5):
    import random
    while True:
        time.sleep(interval)
        for dev in bridge.devices.values():
            if "sensor" in dev.caps:
                dev.state["temperature"] += random.uniform(-0.3, 0.3)
                dev.state["humidity"] += random.uniform(-1.0, 1.0)
                dev.state["temperature"] = round(dev.state["temperature"], 1)
                dev.state["humidity"] = round(max(0, min(100, dev.state["humidity"])), 1)


# ═══════════════════════════════════════════
# main
# ═══════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(description="AgentBridge Simulator")
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args()

    print("AgentBridge Simulator v0.1")
    print("=" * 50)

    # 注册设备
    bridge = AgentBridge()
    bridge.register(VirtualDevice("fan", "Living Room Fan",
        "Controls the living room fan", ["on_off", "level"]))
    bridge.register(VirtualDevice("light", "Desk Light",
        "Controls the desk light brightness", ["on_off", "level"]))
    bridge.register(VirtualDevice("sensor", "Room Sensor",
        "Room temperature and humidity sensor", ["sensor"]))
    print(f"\n  3 devices, {len(bridge.get_tools())} tools registered")

    # 传感器模拟线程
    threading.Thread(target=sensor_simulator, args=(bridge,),
                     daemon=True).start()

    # HTTP Server
    BridgeHandler.bridge = bridge
    server = HTTPServer(("0.0.0.0", args.port), BridgeHandler)
    print(f"\n  HTTP server: http://localhost:{args.port}")
    print(f"  Tools:       curl http://localhost:{args.port}/tools")
    print(f"  Turn on fan: curl -X POST http://localhost:{args.port}/call")
    print(f'    -H \"Content-Type: application/json\"')
    print(f"    -d '{{\"name\":\"fan.set_power\",\"arguments\":{{\"power\":true}}}}'")
    print(f"\n  Ctrl+C to stop\n")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down...")
        server.server_close()


if __name__ == "__main__":
    main()
