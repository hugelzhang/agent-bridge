# AgentBridge ESP-IDF Component
#
# 集成到 ESP32 项目:
#   idf.py create-component agent_bridge
#   cp -r agent-bridge/firmware/* components/agent_bridge/
#
# 或者通过 CMakeLists.txt 引用:
#   set(EXTRA_COMPONENT_DIRS /path/to/agent-bridge/firmware)

idf_component_register(
    SRCS
        "agent_bridge.c"
        "devices/relay.c"
        "transport/transport_serial.c"
    INCLUDE_DIRS
        "."
        "devices"
        "transport"
    REQUIRES
        driver          # gpio
        freertos        # task
)
