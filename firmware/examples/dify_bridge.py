#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Dify Bridge — PC 端串口↔HTTP 桥接器

功能: 把 ESP32 串口设备暴露为 HTTP API, 供 Dify/任意 HTTP Agent 调用.

用法:
  1. ESP32 烧录 firmware, 串口连接 PC
  2. python dify_bridge.py --port COM3 --baud 115200 --http 8080
  3. Dify 自定义工具 → OpenAPI → http://<pc-ip>:8080

架构:
  Dify/Agent (HTTP POST)
      │
      ▼
  dify_bridge.py (:8080)   ← PC 端
      │ 串口 UART
      ▼
  ESP32 (agent_bridge)     ← MCU 端
      │
      ▼
  设备 (风扇/灯/传感器)
"""

import json
import time
import threading
import argparse
import sys
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse

try:
    import serial
except ImportError:
    print("需要 pyserial: pip install pyserial")
    sys.exit(1)


class SerialBridge:
    """串口通信: 发送 JSON 命令到 ESP32, 等待 JSON 响应"""

    def __init__(self, port, baud=115200, timeout=2.0):
        self.ser = serial.Serial(port, baud, timeout=timeout)
        self.lock = threading.Lock()
        time.sleep(2)  # 等 ESP32 启动
        self._read_device_list()

    def _read_device_list(self):
        """上电后读取 ESP32 发送的设备列表"""
        self.devices = []
        self.tools = []
        deadline = time.time() + 5
        while time.time() < deadline:
            line = self.ser.readline().decode('utf-8', errors='ignore').strip()
            if not line:
                continue
            try:
                data = json.loads(line)
                if 'devices' in data:
                    self.devices = data['devices']
                    print(f"[Bridge] Found {len(self.devices)} devices")
                if isinstance(data, list) and len(data) > 0 and 'name' in data[0]:
                    self.tools = data
                    print(f"[Bridge] Found {len(self.tools)} tools")
                if self.devices or self.tools:
                    break
            except json.JSONDecodeError:
                pass
        if not self.devices and not self.tools:
            print("[Bridge] WARNING: No device list received. "
                  "Make sure ESP32 sends device_list JSON on boot.")

    def call_tool(self, name, arguments):
        """发送 tool call 到 ESP32, 等响应"""
        with self.lock:
            cmd = json.dumps({
                "type": "tool_call",
                "name": name,
                "arguments": arguments,
            }) + "\n"
            self.ser.write(cmd.encode())
            self.ser.flush()

            deadline = time.time() + 5
            while time.time() < deadline:
                line = self.ser.readline().decode('utf-8', errors='ignore').strip()
                if not line:
                    continue
                try:
                    return json.loads(line)
                except json.JSONDecodeError:
                    return {"content": [{"type": "text", "text": line}], "isError": False}
            return {"content": [{"type": "text", "text": "timeout"}], "isError": True}

    def close(self):
        self.ser.close()


class BridgeHTTPHandler(BaseHTTPRequestHandler):
    bridge: SerialBridge = None  # 类变量, 在 main 中设置

    def log_message(self, fmt, *args):
        print(f"[HTTP] {args[0]}")

    def _send_json(self, data, status=200):
        body = json.dumps(data, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", len(body))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = urlparse(self.path).path
        if path == "/tools":
            tools = self.bridge.tools if self.bridge.tools else []
            self._send_json(tools)
        elif path == "/devices":
            self._send_json({"devices": self.bridge.devices})
        elif path == "/health":
            self._send_json({"status": "ok", "transport": "serial_bridge"})
        else:
            self._send_json({"error": "not found"}, 404)

    def do_POST(self):
        path = urlparse(self.path).path
        if path != "/call":
            self._send_json({"error": "not found"}, 404)
            return

        content_len = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(content_len).decode()
        try:
            req = json.loads(body)
        except json.JSONDecodeError:
            self._send_json({"error": "invalid JSON"}, 400)
            return

        name = req.get("name", "")
        args = req.get("arguments", {})
        print(f"[Bridge] -> ESP32: {name}({json.dumps(args)})")
        result = self.bridge.call_tool(name, args)
        self._send_json(result)

    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()


def main():
    parser = argparse.ArgumentParser(description="Dify Bridge — Serial ↔ HTTP")
    parser.add_argument("--port", default="COM3", help="ESP32 串口 (默认 COM3)")
    parser.add_argument("--baud", type=int, default=115200, help="波特率")
    parser.add_argument("--http", type=int, default=8080, help="HTTP 监听端口")
    args = parser.parse_args()

    print(f"[Bridge] Opening {args.port} @ {args.baud}...")
    bridge = SerialBridge(args.port, args.baud)
    BridgeHTTPHandler.bridge = bridge

    server = HTTPServer(("0.0.0.0", args.http), BridgeHTTPHandler)
    print(f"[Bridge] HTTP server on http://0.0.0.0:{args.http}")
    print(f"  GET  http://localhost:{args.http}/tools")
    print(f"  POST http://localhost:{args.http}/call")
    print(f"  Ctrl+C to stop")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[Bridge] Shutting down...")
    finally:
        bridge.close()
        server.server_close()


if __name__ == "__main__":
    main()
