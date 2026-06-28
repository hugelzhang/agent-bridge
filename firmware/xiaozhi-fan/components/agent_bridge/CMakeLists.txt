# AgentBridge ESP-IDF Component
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
        driver
        freertos
        esp_timer
)
