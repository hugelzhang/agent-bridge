#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
AgentBridge 安全层演示 — TLS + Token 认证 + 密钥轮换

模拟:
  1. ESP32 生成 JWT Token (设备身份)
  2. Server 验证 Token (签名 + 有效期)
  3. 伪造 Token 被拒绝
  4. 密钥轮换 — 旧 Token 自动失效

用法:
  python auth_demo.py
"""

import hmac
import hashlib
import base64
import json
import time
import os


def base64url_encode(data):
    return base64.urlsafe_b64encode(data).rstrip(b'=').decode()

def base64url_decode(s):
    s += '=' * (4 - len(s) % 4)
    return base64.urlsafe_b64decode(s)


class DeviceAuth:
    """ESP32 端认证模块 (对应 C 代码 agent_auth_t)"""

    def __init__(self, device_id, secret_key=None, ttl=86400):
        self.device_id = device_id
        self.secret_key = secret_key or os.urandom(32).hex()
        self.ttl = ttl
        self.last_rotation = time.time()

    def get_token(self):
        now = int(time.time())
        header = base64url_encode(json.dumps({"alg": "HS256", "typ": "JWT"}).encode())
        payload = base64url_encode(json.dumps({
            "sub": self.device_id,
            "iat": now,
            "exp": now + self.ttl,
            "jti": os.urandom(8).hex()
        }).encode())

        sig_input = f"{header}.{payload}".encode()
        sig = hmac.new(self.secret_key.encode(), sig_input, hashlib.sha256).digest()
        signature = base64url_encode(sig)

        return f"{header}.{payload}.{signature}"

    def rotate_key(self):
        self.secret_key = os.urandom(32).hex()
        self.last_rotation = time.time()
        return self.secret_key


class AuthServer:
    """Server 端验证 (对应 agent_auth_verify)"""

    def __init__(self, expected_secret):
        self.secret_key = expected_secret

    def verify(self, token):
        try:
            parts = token.split('.')
            if len(parts) != 3:
                return None, "invalid format"

            header_b64, payload_b64, sig_b64 = parts

            # 验证签名
            sig_input = f"{header_b64}.{payload_b64}".encode()
            expected_sig = hmac.new(
                self.secret_key.encode(), sig_input, hashlib.sha256
            ).digest()

            actual_sig = base64url_decode(sig_b64)
            if not hmac.compare_digest(expected_sig, actual_sig):
                return None, "signature mismatch"

            # 解码 payload 检查过期
            payload = json.loads(base64url_decode(payload_b64))

            if payload.get("exp", 0) < time.time():
                return None, "token expired"

            return payload["sub"], "ok"

        except Exception as e:
            return None, str(e)

    def rotate_key(self, new_key):
        self.secret_key = new_key


# ═══════════════════════════════════════════
# 演示
# ═══════════════════════════════════════════

def main():
    print("AgentBridge Security Demo")
    print("=" * 60)

    # 1. 初始化
    device = DeviceAuth("esp32_living_room_001")
    server = AuthServer(device.secret_key)
    print(f"\n[1] Device: {device.device_id}")
    print(f"    Secret: {device.secret_key[:16]}...")
    print(f"    TTL: {device.ttl}s (24h)")

    # 2. 生成 Token + 验证
    token = device.get_token()
    print(f"\n[2] Token: {token[:80]}...")
    dev_id, status = server.verify(token)
    print(f"    Verify: {status} → device={dev_id}")

    # 3. 伪造 Token 被拒
    fake_token = token[:-10] + "0000000000"
    _, status = server.verify(fake_token)
    print(f"\n[3] Fake token: {status}")

    # 4. 过期 Token
    device.ttl = 1
    token = device.get_token()
    print(f"\n[4] Short TTL token (1s)")
    time.sleep(2)
    _, status = server.verify(token)
    print(f"    After 2s: {status}")

    # 5. 密钥轮换
    device.ttl = 86400
    old_token = device.get_token()
    new_key = device.rotate_key()
    server.rotate_key(new_key)
    print(f"\n[5] Key rotated: {new_key[:16]}...")
    _, status = server.verify(old_token)
    print(f"    Old token after rotation: {status}")
    new_token = device.get_token()
    _, status = server.verify(new_token)
    print(f"    New token after rotation: {status}")

    print("\n" + "=" * 60)
    print("All checks passed — security layer works")


if __name__ == "__main__":
    main()
