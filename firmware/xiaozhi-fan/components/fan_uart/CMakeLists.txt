idf_component_register(SRCS "uart_proto.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver freertos esp_timer)
