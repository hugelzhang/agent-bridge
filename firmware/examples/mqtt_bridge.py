#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
MQTT Bridge — PC 端 MQTT 设备模拟器

模拟 ESP32 设备通过 MQTT 接入 Home Assistant:
  - 发布 HA 自动发现消息 (Home Assistant 自动创建设备)
  - 接收 HA 控制命令 (开关/调光)
  - 发布设备状态更新

用法:
  pip install paho-mqtt
  python mqtt_bridge.py --broker localhost --port 1883

验证:
  Home Assistant → 设置 → 设备与服务 → MQTT → 自动出现 AgentBridge 设备
  或直接用 MQTT 客户端模拟:
    mosquitto_sub -t 'agentbridge/#' -v
    mosquitto_pub -t 'agentbridge/fan/set' -m 'ON'
"""

import json
import time
import argparse
import random
import threading
import paho.mqtt.client as mqtt


# ═══════════════════════════════════════════
# 虚拟设备
# ═══════════════════════════════════════════

class VirtualDevice:
    def __init__(self, name, display, caps):
        self.name = name
        self.display = display
        self.caps = caps  # dict with on_off/level/sensor keys
        self.state = {}
        if "on_off" in caps:
            self.state["power"] = False
        if "level" in caps:
            self.state["level"] = 0
        if "sensor" in caps:
            self.state["temperature"] = 25.0
            self.state["humidity"] = 60.0

    def set_power(self, on):
        self.state["power"] = on
        if "level" in self.caps and on and self.state.get("level", 0) == 0:
            self.state["level"] = 50

    def set_level(self, pct):
        pct = max(0, min(100, pct))
        self.state["level"] = pct
        self.state["power"] = pct > 0

    def get_state(self):
        return dict(self.state)


# ═══════════════════════════════════════════
# MQTT + HA Auto-Discovery
# ═══════════════════════════════════════════

class MqttBridge:
    def __init__(self, broker_host, broker_port, prefix="agentbridge",
                 client_id="agent_pc_sim"):
        self.prefix = prefix
        self.client_id = client_id
        self.devices = {}
        self.client = mqtt.Client(client_id=client_id)
        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message
        self.client.connect(broker_host, broker_port, 60)
        self.client.loop_start()

    def register(self, dev):
        self.devices[dev.name] = dev
        print(f"  [MQTT] + {dev.name} ({dev.display})")

    def publish_discovery(self):
        """发布所有设备的 HA 自动发现消息"""
        for dev in self.devices.values():
            if "on_off" in dev.caps and "level" not in dev.caps:
                self._discover_switch(dev)
            elif "level" in dev.caps:
                self._discover_light(dev)
            elif "sensor" in dev.caps:
                self._discover_sensor(dev)

    def _discover_switch(self, dev):
        topic = f"homeassistant/switch/agent_{dev.name}/config"
        payload = {
            "name": dev.display,
            "unique_id": f"agent_{dev.name}",
            "state_topic": f"{self.prefix}/{dev.name}/state",
            "command_topic": f"{self.prefix}/{dev.name}/set",
            "payload_on": "ON", "payload_off": "OFF",
            "state_on": "ON", "state_off": "OFF",
            "value_template": "{{ value_json.power }}",
            "device": {
                "name": "AgentBridge PC Sim",
                "identifiers": [self.client_id],
                "manufacturer": "AgentBridge",
                "model": "Simulator"
            }
        }
        self.client.publish(topic, json.dumps(payload), retain=True)

    def _discover_light(self, dev):
        topic = f"homeassistant/light/agent_{dev.name}/config"
        payload = {
            "name": dev.display,
            "unique_id": f"agent_{dev.name}",
            "schema": "json",
            "state_topic": f"{self.prefix}/{dev.name}/state",
            "command_topic": f"{self.prefix}/{dev.name}/set",
            "brightness": True,
            "brightness_scale": 100,
            "supported_color_modes": ["brightness"],
            "value_template": "{{ value_json.level }}",
            "device": {
                "name": "AgentBridge PC Sim",
                "identifiers": [self.client_id],
                "manufacturer": "AgentBridge",
                "model": "Simulator"
            }
        }
        self.client.publish(topic, json.dumps(payload), retain=True)

    def _discover_sensor(self, dev):
        for sensor_type, unit, device_class, key in [
            ("temp", "°C", "temperature", "temperature"),
            ("hum", "%", "humidity", "humidity")
        ]:
            topic = f"homeassistant/sensor/agent_{dev.name}_{sensor_type}/config"
            payload = {
                "name": f"{dev.display} {'Temperature' if 'temp' in sensor_type else 'Humidity'}",
                "unique_id": f"agent_{dev.name}_{sensor_type}",
                "state_topic": f"{self.prefix}/{dev.name}/state",
                "unit_of_measurement": unit,
                "value_template": f"{{{{ value_json.{key} }}}}",
                "device_class": device_class,
                "device": {
                    "name": "AgentBridge PC Sim",
                    "identifiers": [self.client_id]
                }
            }
            self.client.publish(topic, json.dumps(payload), retain=True)

    def _on_connect(self, client, userdata, flags, rc):
        print(f"  [MQTT] Connected (rc={rc})")
        cmd_topic = f"{self.prefix}/+/set"
        client.subscribe(cmd_topic)
        print(f"  [MQTT] Subscribed: {cmd_topic}")
        self.publish_discovery()
        print(f"  [MQTT] HA discovery published for {len(self.devices)} devices")

    def _on_message(self, client, userdata, msg):
        """处理 HA 下发的控制命令"""
        topic = msg.topic
        payload = msg.payload.decode()
        print(f"\n  <<< MQTT {topic}: {payload}")

        # 解析: <prefix>/<device>/set
        parts = topic.split("/")
        if len(parts) < 3:
            return
        dev_name = parts[-2]

        dev = self.devices.get(dev_name)
        if not dev:
            print(f"  [MQTT] Unknown device: {dev_name}")
            return

        old_state = dev.get_state()

        # 处理命令
        if payload in ("ON", "OFF"):
            dev.set_power(payload == "ON")
        elif payload.startswith("{"):
            try:
                cmd = json.loads(payload)
                if "brightness" in cmd:
                    pct = int(cmd["brightness"] * 100 / 255)
                    dev.set_level(pct)
                elif "state" in cmd:
                    dev.set_power(cmd["state"] == "ON")
            except json.JSONDecodeError:
                pass

        new_state = dev.get_state()
        if new_state != old_state:
            # 发布状态更新
            state_topic = f"{self.prefix}/{dev_name}/state"
            # HA switch 需要 "ON"/"OFF" 而不是 JSON
            if "on_off" in dev.caps and "level" not in dev.caps:
                state_payload = "ON" if dev.state["power"] else "OFF"
            else:
                state_payload = json.dumps(new_state)
            client.publish(state_topic, state_payload)
            print(f"  >>> MQTT {state_topic}: {state_payload}")

    def publish_states(self):
        """发布所有设备当前状态"""
        for dev in self.devices.values():
            state_topic = f"{self.prefix}/{dev.name}/state"
            if "on_off" in dev.caps and "level" not in dev.caps:
                payload = "ON" if dev.state["power"] else "OFF"
            else:
                payload = json.dumps(dev.get_state())
            self.client.publish(state_topic, payload)

    def stop(self):
        self.client.loop_stop()
        self.client.disconnect()


# ═══════════════════════════════════════════
# main
# ═══════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(description="MQTT Bridge — HA Simulator")
    parser.add_argument("--broker", default="localhost")
    parser.add_argument("--port", type=int, default=1883)
    parser.add_argument("--prefix", default="agentbridge")
    args = parser.parse_args()

    print("MQTT Bridge — Home Assistant Simulator")
    print(f"  Broker: {args.broker}:{args.port}")
    print("=" * 50)

    bridge = MqttBridge(args.broker, args.port, args.prefix)

    # 注册虚拟设备
    fan = VirtualDevice("fan", "Living Room Fan", {"on_off", "level"})
    light = VirtualDevice("light", "Desk Light", {"on_off", "level"})
    sensor = VirtualDevice("sensor", "Room Sensor", {"sensor"})

    bridge.register(fan)
    bridge.register(light)
    bridge.register(sensor)

    print(f"\n  {len(bridge.devices)} devices ready")
    print(f"  Commands:")
    print(f"    mosquitto_pub -t '{args.prefix}/fan/set' -m 'ON'")
    print(f"    mosquitto_pub -t '{args.prefix}/light/set' -m '{{\"brightness\":128}}'")
    print(f"  States:")
    print(f"    mosquitto_sub -t '{args.prefix}/#' -v")
    print(f"\n  Ctrl+C to stop\n")

    # 传感器模拟
    def sensor_loop():
        while True:
            sensor.state["temperature"] += random.uniform(-0.3, 0.3)
            sensor.state["humidity"] += random.uniform(-1.0, 1.0)
            sensor.state["temperature"] = round(sensor.state["temperature"], 1)
            sensor.state["humidity"] = round(max(0, min(100, sensor.state["humidity"])), 1)
            bridge.publish_states()
            time.sleep(10)

    threading.Thread(target=sensor_loop, daemon=True).start()

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\nShutting down...")
        bridge.stop()


if __name__ == "__main__":
    main()
