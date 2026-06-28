#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
AgentBridge MCP Server — WebSocket 服务端模拟 Claude MCP

ESP32 通过 WebSocket 连接此服务, 自动注册设备 tools.
服务端提供 HTTP API 供用户/AI 调用设备:
  GET  /tools         → 查看已注册的工具列表
  POST /call          → 调用设备工具 → WebSocket → ESP32

用法:
  pip install websockets
  python mcp_server.py                    # 默认 ws://0.0.0.0:8080
  python mcp_server.py --port 9090

架构:
  ESP32 (WebSocket Client)
      │ ws://pc-ip:8080
      ▼
  mcp_server.py (WebSocket Server + HTTP API)
      │
  curl POST /call → WebSocket → ESP32 → 设备执行 → 返回结果
"""

import json
import asyncio
import argparse
import logging
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse
import threading
import websockets

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(name)s] %(message)s")
log = logging.getLogger("MCP-Server")


# ═══════════════════════════════════════════
# WebSocket Server (MCP protocol)
# ═══════════════════════════════════════════

class McpSession:
    """一个 ESP32 连接"""
    def __init__(self, ws, addr):
        self.ws = ws
        self.addr = addr
        self.tools = []
        self.pending = {}   # id → Future

    async def send_json(self, data):
        await self.ws.send(json.dumps(data, ensure_ascii=False))

    async def call_tool(self, tool_name, arguments, timeout=10):
        """向 ESP32 发起 tool call, 等待结果"""
        req_id = int(asyncio.get_event_loop().time() * 1000)
        msg = {
            "jsonrpc": "2.0",
            "method": "tools/call",
            "params": {"name": tool_name, "arguments": arguments},
            "id": req_id,
        }
        future = asyncio.Future()
        self.pending[req_id] = future
        await self.send_json(msg)
        try:
            return await asyncio.wait_for(future, timeout=timeout)
        except asyncio.TimeoutError:
            self.pending.pop(req_id, None)
            return {"error": "timeout"}


class McpServer:
    def __init__(self):
        self.sessions = {}  # addr → McpSession

    async def handle(self, ws):
        addr = ws.remote_address
        session = McpSession(ws, addr)
        self.sessions[str(addr)] = session
        log.info(f"ESP32 connected: {addr}")

        try:
            async for raw in ws:
                try:
                    msg = json.loads(raw)
                except json.JSONDecodeError:
                    continue

                rid = msg.get("id")
                method = msg.get("method")

                if method == "tools/list":
                    tools = msg.get("params", {}).get("tools", [])
                    session.tools = tools
                    log.info(f"Received tools/list: {len(tools)} tools")
                    await session.send_json({
                        "jsonrpc": "2.0", "result": {"ok": True}, "id": rid})

                elif "result" in msg:
                    # ESP32 返回的 tool call 结果
                    if rid is not None and rid in session.pending:
                        session.pending[rid].set_result(msg["result"])
                        del session.pending[rid]

                elif method == "state_update":
                    params = msg.get("params", {})
                    log.info(f"State update: {params}")

        except websockets.exceptions.ConnectionClosed:
            log.info(f"ESP32 disconnected: {addr}")
        finally:
            self.sessions.pop(str(addr), None)

    def get_session(self):
        """返回第一个活跃 session (简化: 单设备场景)"""
        for s in self.sessions.values():
            return s
        return None

    async def call_tool(self, tool_name, arguments, timeout=10):
        s = self.get_session()
        if not s:
            return {"error": "no ESP32 connected"}
        return await s.call_tool(tool_name, arguments, timeout)


# ═══════════════════════════════════════════
# HTTP API (供 Dify/curl/Claude 调用)
# ═══════════════════════════════════════════

class HttpHandler(BaseHTTPRequestHandler):
    server_instance: McpServer = None

    def log_message(self, fmt, *args):
        log.info(f"HTTP {args[0]}")

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
        s = self.server_instance.get_session()
        if path == "/tools":
            self._json(s.tools if s else [])
        elif path == "/health":
            self._json({"status": "ok", "connected": s is not None})
        elif path == "/openapi.json":
            tools = s.tools if s else []
            self._json({
                "openapi": "3.0.0",
                "info": {"title": "AgentBridge via MCP", "version": "1.0.0"},
                "servers": [{"url": f"http://{self.server.server_address[0]}:{self.server.server_address[1]}"}],
                "paths": {
                    "/call": {
                        "post": {
                            "operationId": "mcp_call",
                            "requestBody": {
                                "required": True,
                                "content": {
                                    "application/json": {
                                        "schema": {
                                            "type": "object",
                                            "properties": {
                                                "name": {"type": "string",
                                                         "enum": [t["name"] for t in tools]},
                                                "arguments": {"type": "object"}
                                            },
                                            "required": ["name", "arguments"]
                                        }
                                    }
                                }
                            },
                            "responses": {"200": {"description": "OK"}}
                        }
                    }
                }
            })
        else:
            self._json({"error": "not found"}, 404)

    def do_POST(self):
        if urlparse(self.path).path != "/call":
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
        log.info(f"HTTP → WS call: {name}({json.dumps(args)})")

        # 必须用 async, 这里用同步桥接
        loop = asyncio.new_event_loop()
        result = loop.run_until_complete(
            self.server_instance.call_tool(name, args))
        loop.close()
        self._json(result)

    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()


# ═══════════════════════════════════════════
# main
# ═══════════════════════════════════════════

async def main():
    parser = argparse.ArgumentParser(description="AgentBridge MCP Server")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--http", type=int, default=8081)
    args = parser.parse_args()

    mcp = McpServer()
    HttpHandler.server_instance = mcp

    # HTTP server (独立线程)
    httpd = HTTPServer((args.host, args.http), HttpHandler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    log.info(f"HTTP API: http://{args.host}:{args.http}")
    log.info(f"  GET  http://localhost:{args.http}/tools")
    log.info(f"  POST http://localhost:{args.http}/call")
    log.info(f"  GET  http://localhost:{args.http}/openapi.json")

    # WebSocket server
    log.info(f"WebSocket: ws://{args.host}:{args.port}")
    log.info(f"  ESP32 connect to: ws://<pc-ip>:{args.port}")
    log.info(f"\n  Test: curl -X POST http://localhost:{args.http}/call \\")
    log.info(f'    -H "Content-Type: application/json" \\')
    log.info(f"    -d '{{\"name\":\"fan.set_power\",\"arguments\":{{\"power\":true}}}}'")
    log.info(f"\n  Waiting for ESP32 connection...\n")

    async with websockets.serve(mcp.handle, args.host, args.port):
        await asyncio.Future()  # run forever


if __name__ == "__main__":
    asyncio.run(main())
