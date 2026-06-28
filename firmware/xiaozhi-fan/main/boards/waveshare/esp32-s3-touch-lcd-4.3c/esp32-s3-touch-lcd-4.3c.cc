#include "wifi_board.h"
#include "display/lcd_display.h"

#include "codecs/box_audio_codec.h"
#include "application.h"
#include "button.h"
#include "mcp_server.h"
#include "config.h"
#include "power_save_timer.h"
#include "i2c_device.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>

#include <esp_lcd_touch_gt911.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <driver/uart.h>
#include "uart_proto.h"  // AI Fan UART 风扇控制协议

/* AgentBridge — 设备抽象层 */
#include "agent_bridge.h"
#include "transport_mcp.h"

#define TAG "WaveshareEsp32s3TouchLCD43c"

/* AgentBridge 全局实例 (板级单例) */
static agent_bridge_t *g_bridge = nullptr;

/* ---- AgentBridge 设备: 风扇 UART 协议适配 ---- */

/** 风扇设备上下文 */
typedef struct {
    uint8_t  speed;        /* 缓存的风扇速度 0-101 */
    uint8_t  light;        /* 缓存的灯光模式 */
    uint8_t  swing;        /* 缓存的摇头模式 */
} fan_ctx_t;

static fan_ctx_t g_fan_ctx = {0, 0, 0};

/* 风扇 set_level: 0-100 -> UART 0-101 */
static int fan_set_level(void *ctx, uint8_t pct) {
    uint8_t speed = (pct > 100) ? 101 : (uint8_t)pct;
    uart_proto_set_fan(speed);
    g_fan_ctx.speed = speed;
    return 0;
}

/* 风扇 get_state */
static int fan_get_state(void *ctx, char *buf, size_t len) {
    const slave_status_t *st = uart_proto_get_status();
    bool mcu_online = (st->fan != 0 || st->light != 0 || st->angle != 0 || st->chg != 0);
    uint8_t fan   = mcu_online ? st->fan   : g_fan_ctx.speed;
    return snprintf(buf, len,
        "{\"power\":%s,\"level\":%d,\"online\":%s,\"mcu_online\":%s}",
        fan > 0 ? "true" : "false", fan,
        "true", mcu_online ? "true" : "false");
}

/* 风扇设备定义 */
static agent_device_t g_fan_dev = {
    .name         = "fan",
    .display_name = "风扇",
    .description  = "控制风扇开关和调速 (0-100%)",
    .caps         = (agent_capability_t)(AB_CAP_ON_OFF | AB_CAP_LEVEL),
    .ops          = {
        .on         = NULL,  /* 用 set_level 替代: 0=关, >0=开 */
        .set_level  = fan_set_level,
        .get_state  = fan_get_state,
    },
    .hw_ctx       = &g_fan_ctx,
};

/* 灯光 set_level: 0-100 -> 四档映射 */
static int light_set_level(void *ctx, uint8_t pct) {
    light_mode_t mode;
    if (pct == 0)       mode = LIGHT_OFF;
    else if (pct <= 33)  mode = LIGHT_SLEEP;
    else if (pct <= 66)  mode = LIGHT_AMBIENT;
    else                 mode = LIGHT_HIGH;
    uart_proto_set_light(mode);
    g_fan_ctx.light = (uint8_t)mode;
    return 0;
}

static int light_get_state(void *ctx, char *buf, size_t len) {
    const slave_status_t *st = uart_proto_get_status();
    bool mcu_online = (st->fan != 0 || st->light != 0 || st->angle != 0 || st->chg != 0);
    uint8_t light = mcu_online ? st->light : g_fan_ctx.light;
    const char *light_str;
    switch (light) {
        case LIGHT_OFF:     light_str = "off"; break;
        case LIGHT_SLEEP:   light_str = "dim"; break;
        case LIGHT_AMBIENT: light_str = "medium"; break;
        case LIGHT_HIGH:    light_str = "bright"; break;
        default:            light_str = "unknown"; break;
    }
    return snprintf(buf, len,
        "{\"level\":%d,\"mode\":\"%s\",\"online\":%s}",
        light, light_str, mcu_online ? "true" : "false");
}

static agent_device_t g_light_dev = {
    .name         = "light",
    .display_name = "灯光",
    .description  = "控制风扇灯光亮度 (0-100%)",
    .caps         = AB_CAP_LEVEL,
    .ops          = {
        .set_level  = light_set_level,
        .get_state  = light_get_state,
    },
    .hw_ctx       = &g_fan_ctx,
};

/* 摇头 set_position: 字符串 mode -> 角度值 */
static int swing_set_position(void *ctx, int16_t angle) {
    /* 使用 agent_device_t 的 set_position 承载摇头模式 */
    /* angle 语义: 0=off, 1=sweep, 2=left, 3=center, 4=right */
    /* 实际由 action "set_swing" 处理, 这里作为后备 */
    return 0;
}

static int swing_get_state(void *ctx, char *buf, size_t len) {
    const slave_status_t *st = uart_proto_get_status();
    bool mcu_online = (st->fan != 0 || st->light != 0 || st->angle != 0 || st->chg != 0);
    uint8_t swing = mcu_online ? st->angle : g_fan_ctx.swing;
    const char *swing_str;
    switch (swing) {
        case ANGLE_OFF:    swing_str = "off"; break;
        case ANGLE_SWEEP:  swing_str = "sweep"; break;
        case ANGLE_LEFT:   swing_str = "left"; break;
        case ANGLE_CENTER: swing_str = "center"; break;
        case ANGLE_RIGHT:  swing_str = "right"; break;
        default:           swing_str = "unknown"; break;
    }
    return snprintf(buf, len,
        "{\"position\":%d,\"mode\":\"%s\",\"online\":%s}",
        swing, swing_str, mcu_online ? "true" : "false");
}

static agent_device_t g_swing_dev = {
    .name         = "swing",
    .display_name = "摇头",
    .description  = "控制风扇摇头方向",
    .caps         = AB_CAP_POSITION,
    .ops          = {
        .get_state    = swing_get_state,
        .set_position = swing_set_position,
    },
    .hw_ctx       = &g_fan_ctx,
};

class CustomBacklight : public Backlight {
public:
    CustomBacklight(esp_io_expander_handle_t io_handle)
        : Backlight(), io_handle_(io_handle) {}

protected:
    esp_io_expander_handle_t io_handle_;

    virtual void SetBrightnessImpl(uint8_t brightness) override {
        if (brightness > 100) brightness = 100;
        int flipped_brightness = 100 - brightness;

        custom_io_expander_set_pwm(io_handle_, flipped_brightness * 255 / 100);
    }
};


class WaveshareEsp32s3TouchLCD43c : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    LcdDisplay* display_;
    esp_io_expander_handle_t io_expander = NULL;
    PowerSaveTimer* power_save_timer_;
    CustomBacklight *backlight_;

    void InitializePowerSaveTimer() {
        // 暂时禁用休眠省电, 排查唤不醒根因
        power_save_timer_ = new PowerSaveTimer(-1, -1, -1);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(10); });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();});
        // power_save_timer_->SetEnabled(true);
    }

    void InitializeGpio() {
        // Zero-initialize the GPIO configuration structure
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE; // Disable interrupts for this pin
        io_conf.pin_bit_mask = 1ULL << BSP_LCD_TOUCH_INT;    // Select the GPIO pin using a bitmask
        io_conf.mode = GPIO_MODE_OUTPUT;          // Set pin as output
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE; // Disable pull-up
        gpio_config(&io_conf); // Apply the configuration
    }

    void InitializeCodecI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = BSP_I2C_SDA,
            .scl_io_num = BSP_I2C_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeCustomio(void) {
        esp_err_t ret = custom_io_expander_new_i2c_ch32v003(i2c_bus_, BSP_IO_EXPANDER_I2C_ADDRESS, &io_expander);
        if (ret != ESP_OK || io_expander == NULL) {
            ESP_LOGE(TAG, "CH422G init failed (0x%x), io_expander=%p", ret, io_expander);
            return;
        }
        ESP_LOGI(TAG, "CH422G OK (addr=0x%x)", BSP_IO_EXPANDER_I2C_ADDRESS);
        esp_io_expander_set_dir(io_expander, BSP_POWER_AMP_IO | BSP_LCD_BACKLIGHT | BSP_LCD_TOUCH_RST , IO_EXPANDER_OUTPUT);
        esp_io_expander_set_level(io_expander, BSP_POWER_AMP_IO | BSP_LCD_BACKLIGHT | BSP_LCD_TOUCH_RST , 1);

        esp_io_expander_set_level(io_expander, BSP_LCD_TOUCH_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(200));
        gpio_set_level(BSP_LCD_TOUCH_INT, 0);
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_io_expander_set_level(io_expander, BSP_LCD_TOUCH_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(200));
        ESP_LOGI(TAG, "Touch RST sequence done");
    }

    void InitializeRGB() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel_handle = nullptr;

        esp_lcd_rgb_panel_config_t rgb_config = {
            .clk_src = LCD_CLK_SRC_DEFAULT,
            .timings = {
                .pclk_hz = 16 * 1000 * 1000,
                .h_res = BSP_LCD_H_RES,
                .v_res = BSP_LCD_V_RES,
                .hsync_pulse_width = 4,
                .hsync_back_porch = 4,
                .hsync_front_porch = 8,
                .vsync_pulse_width = 4,
                .vsync_back_porch = 4,
                .vsync_front_porch = 8,
                .flags = {
                    .pclk_active_neg = true
                }
            },
            .data_width = 16,
            .bits_per_pixel = 16,
            .num_fbs = 2,
            .bounce_buffer_size_px = BSP_LCD_H_RES * 10,
            .psram_trans_align = 64,
            .hsync_gpio_num = BSP_LCD_HSYNC,
            .vsync_gpio_num = BSP_LCD_VSYNC,
            .de_gpio_num = BSP_LCD_DE,
            .pclk_gpio_num = BSP_LCD_PCLK,
            .disp_gpio_num = BSP_LCD_DISP,
            .data_gpio_nums = {
                BSP_LCD_DATA0, BSP_LCD_DATA1, BSP_LCD_DATA2, BSP_LCD_DATA3,
                BSP_LCD_DATA4, BSP_LCD_DATA5, BSP_LCD_DATA6, BSP_LCD_DATA7,
                BSP_LCD_DATA8, BSP_LCD_DATA9, BSP_LCD_DATA10, BSP_LCD_DATA11,
                BSP_LCD_DATA12, BSP_LCD_DATA13, BSP_LCD_DATA14, BSP_LCD_DATA15
            },
            .flags = {
                .fb_in_psram = 1,
            },
        };

        ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&rgb_config, &panel_handle));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

        display_ = new RgbLcdDisplay(panel_io, panel_handle,
                                  BSP_LCD_H_RES, BSP_LCD_V_RES, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                  DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);

        backlight_ = new CustomBacklight(io_expander);
        backlight_->RestoreBrightness();                    
    }

    void InitializeTouch() {
        esp_lcd_touch_handle_t tp;
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = BSP_LCD_H_RES - 1,
            .y_max = BSP_LCD_V_RES - 1,
            .rst_gpio_num = GPIO_NUM_NC,
            .int_gpio_num = GPIO_NUM_NC,
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = 0,
                .mirror_x = 0,
                .mirror_y = 0,
            },
        };
        esp_lcd_panel_io_handle_t tp_io_handle = NULL;
        esp_lcd_panel_io_i2c_config_t tp_io_config = {
            .dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, 
            .control_phase_bytes = 1,
            .dc_bit_offset = 0,
            .lcd_cmd_bits = 16,                            
            .flags =
            {
                .disable_control_phase = 1,
            }
	    };
        tp_io_config.scl_speed_hz = 400 * 1000;

        esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle);

        ESP_LOGI(TAG, "Initialize touch controller");
        ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, &tp));
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(),
            .handle = tp,
        };
        lvgl_port_add_touch(&touch_cfg);
        ESP_LOGI(TAG, "Touch panel initialized successfully");
    }

    void InitializeTools() {
        /* ── AgentBridge: 设备注册 + MCP 桥接 ── */
        agent_cfg_t cfg = {};
        g_bridge = agent_bridge_init(&cfg);

        agent_bridge_register_device(g_bridge, &g_fan_dev);
        agent_bridge_register_device(g_bridge, &g_light_dev);
        agent_bridge_register_device(g_bridge, &g_swing_dev);

        /* 桥接: AgentBridge 设备 → 小智 McpServer */
        transport_mcp_attach(g_bridge);

        /* 原有独立工具: WiFi 重新配网 */
        auto &mcp = McpServer::GetInstance();
        mcp.AddTool("self.system.reconfigure_wifi",
            "Reboot the device and enter WiFi configuration mode.\n"
            "**CAUTION** You must ask the user to confirm this action.",
            PropertyList(), [this](const PropertyList& properties) {
                EnterWifiConfigMode();
                return true;
            });
    }

public:
    WaveshareEsp32s3TouchLCD43c() {
        ESP_LOGI(TAG, "=== Board init start ===");
        InitializePowerSaveTimer();
        ESP_LOGI(TAG, "PowerSaveTimer OK");
        InitializeGpio();
        ESP_LOGI(TAG, "GPIO OK");
        InitializeCodecI2c();
        ESP_LOGI(TAG, "I2C OK (SDA=%d SCL=%d)", BSP_I2C_SDA, BSP_I2C_SCL);
        InitializeCustomio();
        InitializeRGB();
        ESP_LOGI(TAG, "LCD OK");
        InitializeTouch();
        ESP_LOGI(TAG, "Touch OK");
        // AI Fan: UART 延后到 Application::Initialize 音频就绪后
        // (在构造函数中初始化会破坏 AFE 任务启动)
        ESP_LOGI(TAG, "UART fan control deferred");
        InitializeTools();
        ESP_LOGI(TAG, "Tools OK");
        GetBacklight()->SetBrightness(100);
        ESP_LOGI(TAG, "=== Board init done ===");
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            BSP_I2S_MCLK, 
            BSP_I2S_SCLK, 
            BSP_I2S_LCLK, 
            BSP_I2S_DOUT, 
            BSP_I2S_DSIN,
            BSP_PA_PIN, 
            BSP_CODEC_ES8311_ADDR, 
            BSP_CODEC_ES7210_ADDR, 
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight *GetBacklight() override {
         return backlight_;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(WaveshareEsp32s3TouchLCD43c);
