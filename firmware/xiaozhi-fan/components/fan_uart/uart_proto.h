/**
 * UART 协议驱动 — MC32F7073 下位机通讯
 *
 * 协议规格: EVK 外部板 UART 协议 V1.0 (2026-06-12)
 * 物理层:  9600-8-N-1, 半双工, TTL 电平
 *
 * 使用方法:
 *   1. uart_proto_init()    初始化 UART1 + 创建接收/定时器任务
 *   2. uart_proto_send_xxx() 发送控制命令 (带自动重试)
 *   3. 回调通知上层 (ACK/NACK/状态变更)
 */

#ifndef UART_PROTO_H
#define UART_PROTO_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 *  帧常量
 * ================================================================ */
#define UPROTO_SOF              0xA5
#define UPROTO_EOF              0x5A
#define UPROTO_MAX_DATA         32
#define UPROTO_MIN_FRAME_LEN    4       /* SOF + CMD + LEN + EOF */
#define UPROTO_MAX_FRAME_LEN    (4 + UPROTO_MAX_DATA)

/* ================================================================
 *  命令码
 * ================================================================ */
#define CMD_STATUS_REPORT       0x01    /* 下位机 → ESP32 */
#define CMD_STATUS_QUERY        0x02    /* ESP32 → 下位机 */
#define CMD_SET_ANGLE           0x10    /* 摇头控制 */
#define CMD_SET_LIGHT           0x11    /* 灯光控制 */
#define CMD_SET_FAN             0x12    /* 风扇控制 */
#define CMD_SHUTDOWN            0x13    /* 关机 */
#define CMD_ACK                 0x20    /* 应答-成功 */
#define CMD_NACK                0x21    /* 应答-失败 */

/* ================================================================
 *  错误码 (NACK 中的 ERROR 字段)
 * ================================================================ */
#define ERR_PARAM_OUT_OF_RANGE  0x01
#define ERR_DEVICE_BUSY         0x02
#define ERR_HW_EXEC_FAIL        0x03
#define ERR_READ_ONLY_FIELD     0x05
#define ERR_CMD_NOT_SUPPORTED   0x06

/* ================================================================
 *  操作参数
 * ================================================================ */
typedef enum {
    ANGLE_OFF    = 0x00,
    ANGLE_SWEEP  = 0x01,   /* 实际: 左右摆动 */
    ANGLE_LEFT   = 0x02,   /* 实际: 向左 */
    ANGLE_CENTER = 0x03,   /* 实际: 居中 */
    ANGLE_RIGHT  = 0x04,   /* 实际: 向右 */
} angle_mode_t;

typedef enum {
    LIGHT_OFF     = 0x00,
    LIGHT_SLEEP   = 0x01,
    LIGHT_AMBIENT = 0x02,
    LIGHT_HIGH    = 0x03,
} light_mode_t;

/* 风扇 0=关, 1~100=档位, 101=最大 */
#define PROTO_FAN_OFF     0
#define PROTO_FAN_MAX     101

/* ================================================================
 *  状态结构体 (对应 0x01 状态上报帧)
 * ================================================================ */
typedef struct {
    uint8_t  chg;       /* 充电状态 (固定 0, 未用) */
    uint8_t  light;     /* 灯光模式 (LIGHT_OFF/ SLEEP/ AMBIENT/ HIGH) */
    uint8_t  angle;     /* 摇头模式 (ANGLE_OFF/ LEFT/ CENTER/ RIGHT/ SWING) */
    uint8_t  fan;       /* 风扇档位 (0~101) */
} slave_status_t;

/* ================================================================
 *  回调类型
 * ================================================================ */

/** 收到 ACK 时调用: cmd=被确认的命令, status=0x00 */
typedef void (*uart_ack_cb_t)(uint8_t cmd, uint8_t status);

/** 收到 NACK 时调用: cmd=被拒绝的命令, error=错误码 */
typedef void (*uart_nack_cb_t)(uint8_t cmd, uint8_t error);

/** 收到状态上报或查询响应时调用 */
typedef void (*uart_status_cb_t)(const slave_status_t *st);

/** 收到无法识别的帧 */
typedef void (*uart_error_cb_t)(const char *msg);

/* ================================================================
 *  API
 * ================================================================ */

/**
 * 初始化 UART1 协议栈
 *  - GPIO11=TX, GPIO12=RX
 *  - 创建接收任务 + 状态轮询定时器
 */
void uart_proto_init(void);

/**
 * 设置回调函数 (在 init 之前或之后调用均可)
 */
void uart_proto_set_ack_callback(uart_ack_cb_t cb);
void uart_proto_set_nack_callback(uart_nack_cb_t cb);
void uart_proto_set_status_callback(uart_status_cb_t cb);
void uart_proto_set_error_callback(uart_error_cb_t cb);

/* ---- 控制命令 (带重试) ---- */

/** 设置风扇速度 (0=关, 1~100, 101=最大) */
void uart_proto_set_fan(uint8_t speed);

/** 设置灯光模式 */
void uart_proto_set_light(light_mode_t mode);

/** 设置摇头模式 */
void uart_proto_set_angle(angle_mode_t mode);

/** 关机 (不等 ACK) */
void uart_proto_shutdown(void);

/** 查询下位机状态 (异步, 结果通过 status_cb 回调) */
void uart_proto_query_status(void);

/** 获取最近一次状态快照 */
const slave_status_t *uart_proto_get_status(void);

#ifdef __cplusplus
}
#endif

#endif /* UART_PROTO_H */
