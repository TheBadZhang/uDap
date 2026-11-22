#pragma once

/*
 * CMSIS-DAP 硬件适配层（AT32F405KBU7 / uDap 板）。
 *
 * 这份文件是 CherryDAP 的 DAP.c / SW_DP.c / JTAG_DP.c 与「硬件」之间唯一的
 * 接缝：DAP 层只管协议与时序，所有引脚怎么拉、时钟多少、包多大，都由这里
 * 的宏和 inline 函数决定。换成别的板子只需要改这一个文件。
 *
 * 板级连接（详见 board_pin_map.h）：
 *   PA5  SWCLK / TCK            （也是 SPI1_SCK，SPI 版实现要用）
 *   PA6  SWDIO 采样端（MISO）    （与 PA7 短接）
 *   PA7  SWDIO 驱动端（MOSI）    （JTAG 模式下是 TMS）
 *   PB1  JTAG TDI               （JTAG 关闭时复用为串口 DTR）
 *   PB0  JTAG TDO               （JTAG 关闭时复用为串口 RTS）
 *   PA4  目标 nRESET（开漏）
 *   PB2  LED1，PB4  LED2（低电平点亮）
 *
 * 快速路径设计：
 *   AT32 的 GPIO 有一个 scr（set/clear）寄存器：低 16 位置位、高 16 位清零，
 *   一次写入即可原子地改一个引脚，不用「读-改-写」。所以这里的 SET/CLR 都是
 *   单次 32 位 store，SWD 位翻转可以做到 2 个周期一个时钟沿。
 */

#include <stdint.h>
#include <string.h>

#include "at32f402_405.h"

#include "board_pin_map.h"
#include "drv/drv_led.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================ 时钟与时序参数 ============================ */

/* 内核主频（PLL 216 MHz，见 sdk/bsp/at32f405_clock.c）。
 * DAP.c 用它把「请求的 SWJ 时钟」换算成延时循环次数。 */
#define CPU_CLOCK 216000000U

/* 一次 GPIO 写操作占用的周期数（scr 寄存器原子写约 2 个周期） */
#define IO_PORT_WRITE_CYCLES 2U

/* 慢速路径：每个半周期的空转次数（越大越慢）。PIN_DELAY_SLOW 会把传进来的
 * clock_delay 乘上这个值，所以它同时决定 Delayms() 的折算比例，保持 8 即可 */
#define DELAY_SLOW_CYCLES 8U

/* 快速路径：每个半周期插几个 NOP。0 = 不插（跑满 GPIO 极限） */
#define DELAY_FAST_CYCLES 2U

/* ================================ 能力 ================================== */

#define DAP_SWD 1
#define DAP_JTAG 1
#define DAP_JTAG_DEV_CNT 4U

/* 默认端口：1 = SWD，2 = JTAG。用户要求默认 JTAG。 */
#define DAP_DEFAULT_PORT 2U

/* 上电默认 SWJ 时钟（Hz）。主机可以用 DAP_SWJ_Clock 命令改。 */
#define DAP_DEFAULT_SWJ_CLOCK 1000000U

/* 单包大小必须等于 USB 端点最大包长（HS 下 512），否则长命令会被拆包错位 */
#define DAP_PACKET_SIZE 512U
#define DAP_PACKET_COUNT 8U

/* 本工程不用 SWO 与 DAP UART（串口走 CDC 桥接，不占 DAP 命令）。
 * 下面这些名字即使功能关闭也必须定义：DAP.c 的 DAP_Info 命令会无条件引用
 * 它们拼「能力位」和缓冲尺寸。 */
#define SWO_UART 0
#define SWO_MANCHESTER 0
#define SWO_STREAM 0
#define SWO_BUFFER_SIZE 0U
#define SWO_UART_DRIVER 0
#define SWO_UART_MAX_BAUDRATE 0U

#define DAP_UART 0
#define DAP_UART_DRIVER 0
#define DAP_UART_RX_BUFFER_SIZE 0U
#define DAP_UART_TX_BUFFER_SIZE 0U
#define DAP_UART_USB_COM_PORT 0U

/* 目标板信息（DAP_Info 的 TARGET_* 子命令）。这不是探针自身的信息，而是
 * 「默认要调的那颗芯片」，我们让上位机自己选，所以填空串。 */
#define TARGET_FIXED 0U
#define TARGET_DEVICE_VENDOR ""
#define TARGET_DEVICE_NAME ""
#define TARGET_BOARD_VENDOR ""
#define TARGET_BOARD_NAME ""

/* 时间戳功能关闭（0 = 不支持，主机不会来问） */
#define TIMESTAMP_CLOCK 0U

#define DAP_FW_VER "1.0.0"

/* ============================== 字符串信息 ============================== */

/* dap_main.c 里实现：返回 USB 序列号（来自芯片 UID） */
extern const char *udap_serial_number(void);

__STATIC_INLINE uint8_t DAP_GetVendorString(char *str) {
    (void)str;
    return 0U;
}

__STATIC_INLINE uint8_t DAP_GetProductString(char *str) {
    static const char product[] = "uDap CMSIS-DAP";
    memcpy(str, product, sizeof(product));
    return (uint8_t)sizeof(product);
}

__STATIC_INLINE uint8_t DAP_GetSerNumString(char *str) {
    const char *serial = udap_serial_number();
    if (serial == 0) {
        return 0U;
    }
    const size_t len = strlen(serial);
    if (len == 0U || len > 32U) {
        return 0U;
    }
    memcpy(str, serial, len + 1U);
    return (uint8_t)(len + 1U);
}

/* 目标芯片/板子的说明字符串（DAP_Info 的 TARGET_* 子命令）。
 * 空串表示「不固定目标」，由上位机自己选芯片。返回 0 表示不支持。 */
__STATIC_INLINE uint8_t DAP_GetTargetDeviceVendorString(char *str) {
    (void)str;
    return 0U;
}

__STATIC_INLINE uint8_t DAP_GetTargetDeviceNameString(char *str) {
    (void)str;
    return 0U;
}

__STATIC_INLINE uint8_t DAP_GetTargetBoardVendorString(char *str) {
    (void)str;
    return 0U;
}

__STATIC_INLINE uint8_t DAP_GetTargetBoardNameString(char *str) {
    (void)str;
    return 0U;
}

__STATIC_INLINE uint8_t DAP_GetProductFirmwareVersionString(char *str) {
    static const char ver[] = DAP_FW_VER;
    memcpy(str, ver, sizeof(ver));
    return (uint8_t)sizeof(ver);
}

/* ============================ 引脚操作原语 ============================== */

/*
 * scr 寄存器：bit[n]=1 置位 ODT[n]，bit[16+n]=1 清零 ODT[n]。
 * 一次 32 位写入就能同时表达「哪些置位、哪些清零」，且不会误伤其它引脚，
 * 所以不需要关中断。
 *
 * 但只给「时序自己保证」的物理层用：spi（32 位由硬件移位）和 gpio_fast
 * （汇编内核跑在 RAM 里）。纯 C 的 SW_DP.c 从 Flash 取指，27 MHz 档每个半周期
 * 只剩几条指令，取指等待 + USB 中断抖动会把时序打散（实测同一固件 5 次里
 * 2 次报 `Unexpected ACK`）——所以 C 路退回调库函数，靠它内部的读改写
 * 天然拉开半个周期的余量，27 MHz 请求实测 6.2 µs/字且零错误。
 */
#if defined(SWD_PHY_GPIO_FAST) || defined(SWD_PHY_SPI)
#define PIN_SET(port, pins) ((port)->scr = (uint32_t)(pins))
#define PIN_CLR(port, pins) ((port)->scr = ((uint32_t)(pins) << 16))
#else
/* 纯 C 位翻转：内联成单条 `str`（和 gpio_bits_set/reset 等价），但每条之间加
 * 编译器屏障，防止 LTO 把相邻两次写合并成一次或重排——合并会让 SWDIO 与
 * SWCLK 同时变化，目标就收不到数据（实测过）。 */
#define PIN_SET(port, pins) ((port)->scr = (uint32_t)(pins))
#define PIN_CLR(port, pins) ((port)->scr = ((uint32_t)(pins) << 16))
#endif
#define PIN_READ(port, pins) (((port)->idt & (uint32_t)(pins)) != 0U)

/* PA7 是 SWDIO 的驱动端。注意 AT32 的 GPIO 方向在 cfgr（每引脚 2 bit 的 iomc
 * 字段，0=输入 1=输出 2=复用 3=模拟），而 omode 是「输出类型」（0=推挽
 * 1=开漏）——所以切方向必须改 cfgr，不能改 omode。 */
#define SWDIO_CFGR_SHIFT (7 * 2)
#define SWDIO_CFGR_MASK  (3UL << SWDIO_CFGR_SHIFT)

/* SCR 表里的引脚位 */
#define PA5_BIT GPIO_PINS_5
#define PA6_BIT GPIO_PINS_6
#define PA7_BIT GPIO_PINS_7
#define PA4_BIT GPIO_PINS_4
#define PB0_BIT GPIO_PINS_0
#define PB1_BIT GPIO_PINS_1

/* ============================== 端口初始化 ============================== */

/* 把一组引脚配成「推挽输出」或「浮空/上拉输入」 */
__STATIC_INLINE void pin_config_output(gpio_type *port, uint16_t pins, uint16_t pull) {
    gpio_init_type init;
    gpio_default_para_init(&init);
    init.gpio_pins = pins;
    init.gpio_mode = GPIO_MODE_OUTPUT;
    init.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    init.gpio_pull = pull;
    init.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init(port, &init);
}

__STATIC_INLINE void pin_config_input(gpio_type *port, uint16_t pins, uint16_t pull) {
    gpio_init_type init;
    gpio_default_para_init(&init);
    init.gpio_pins = pins;
    init.gpio_mode = GPIO_MODE_INPUT;
    init.gpio_pull = pull;
    gpio_init(port, &init);
}

/* SWD 模式：SWCLK/SWDIO 输出（空闲都拉高），另一根 SWDIO 采样脚输入上拉 */
__STATIC_INLINE void swd_port_setup(void) {
    crm_periph_clock_enable(BOARD_SWD_GPIO_CLK, TRUE);

    /* PA5/PA7 先写高再切输出，避免上电毛刺被目标当成一次时钟 */
    PIN_SET(BOARD_SWD_PORT, BOARD_SWCLK_PIN | BOARD_SWDIO_OUT_PIN);

    pin_config_input(BOARD_SWD_PORT, BOARD_SWDIO_IN_PIN, GPIO_PULL_UP);
    pin_config_output(BOARD_SWD_PORT, BOARD_SWCLK_PIN | BOARD_SWDIO_OUT_PIN, GPIO_PULL_UP);

    /* PA4 = nRESET，开漏（配置一次，之后只写 ODT） */
    {
        gpio_init_type init;
        gpio_default_para_init(&init);
        init.gpio_pins = BOARD_NRESET_PIN;
        init.gpio_mode = GPIO_MODE_OUTPUT;
        init.gpio_out_type = GPIO_OUTPUT_OPEN_DRAIN;
        init.gpio_pull = GPIO_PULL_UP;
        init.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
        PIN_SET(BOARD_NRESET_PORT, BOARD_NRESET_PIN); /* 先释放 */
        gpio_init(BOARD_NRESET_PORT, &init);
    }

#if defined(SWD_PHY_GPIO_FAST) && (SWD_PHY_GPIO_FAST != 0)
    /* 把 cfgr 的"驱动/放开"两个目标值存下来，SWD_Transfer 里切方向只写一次 */
    extern void swd_fast_dio_snapshot(void);
    swd_fast_dio_snapshot();
#endif
}

/* JTAG 模式：多出 TDI（输出）与 TDO（输入） */
__STATIC_INLINE void jtag_port_setup(void) {
    crm_periph_clock_enable(BOARD_JTAG_GPIO_CLK, TRUE);

    PIN_SET(BOARD_JTAG_PORT, BOARD_TDI_PIN); /* TDI 空闲高 */
    pin_config_output(BOARD_JTAG_PORT, BOARD_TDI_PIN, GPIO_PULL_NONE);
    pin_config_input(BOARD_JTAG_PORT, BOARD_TDO_PIN, GPIO_PULL_UP);

    swd_port_setup(); /* TCK/TMS/nRESET 与 SWD 相同 */
}

/* 把所有调试引脚放成高阻，交还给目标板/其它功能 */
__STATIC_INLINE void port_off(void) {
    pin_config_input(BOARD_SWD_PORT, BOARD_SWCLK_PIN | BOARD_SWDIO_PINS, GPIO_PULL_UP);
    pin_config_input(BOARD_JTAG_PORT, BOARD_TDI_PIN | BOARD_TDO_PIN, GPIO_PULL_NONE);
    PIN_SET(BOARD_NRESET_PORT, BOARD_NRESET_PIN);
}

/* DAP.c 里实现（DAP.h 也有声明，这里提前声明一份，供 RESET_TARGET 使用） */
extern void Delayms(uint32_t delay);

#if defined(SWD_PHY_SPI) && (SWD_PHY_SPI != 0)
/* swd_spi.c 提供：把 SPI1 接到 PA5(SCK)/PA6(MISO)/PA7(MOSI) 上 */
extern void swd_spi_port_setup(void);
#endif

/* ------------------------------ nTRST/nRESET --------------------------- */

/* nTRST 没接（板上没有），固定当作「已释放」 */
__STATIC_FORCEINLINE uint32_t PIN_nTRST_IN(void) {
    return 1U;
}

__STATIC_FORCEINLINE void PIN_nTRST_OUT(uint32_t bit) {
    (void)bit;
}

__STATIC_FORCEINLINE uint32_t PIN_nRESET_IN(void) {
    return PIN_READ(BOARD_NRESET_PORT, BOARD_NRESET_PIN) ? 1U : 0U;
}

/* 开漏：写 0 拉低（复位目标），写 1 释放（带上拉） */
__STATIC_FORCEINLINE void PIN_nRESET_OUT(uint32_t bit) {
    if ((bit & 1U) != 0U) {
        PIN_SET(BOARD_NRESET_PORT, BOARD_NRESET_PIN);
    } else {
        PIN_CLR(BOARD_NRESET_PORT, BOARD_NRESET_PIN);
    }
}

/* ============================== DAP 接口 ================================ */

__STATIC_INLINE void PORT_JTAG_SETUP(void) {
    jtag_port_setup();
#if defined(SWD_PHY_SPI) && (SWD_PHY_SPI != 0)
    swd_spi_port_setup();
#endif
    PIN_nRESET_OUT(1U);
}

__STATIC_INLINE void PORT_SWD_SETUP(void) {
    swd_port_setup();
#if defined(SWD_PHY_SPI) && (SWD_PHY_SPI != 0)
    swd_spi_port_setup();
#endif
    PIN_nRESET_OUT(1U);
}

__STATIC_INLINE void PORT_OFF(void) {
    port_off();
}

/* ------------------------------ SWCLK/TCK ------------------------------ */

__STATIC_FORCEINLINE uint32_t PIN_SWCLK_TCK_IN(void) {
    return PIN_READ(BOARD_SWD_PORT, BOARD_SWCLK_PIN) ? 1U : 0U;
}

__STATIC_FORCEINLINE void PIN_SWCLK_TCK_SET(void) {
    PIN_SET(BOARD_SWD_PORT, BOARD_SWCLK_PIN);
}

__STATIC_FORCEINLINE void PIN_SWCLK_TCK_CLR(void) {
    PIN_CLR(BOARD_SWD_PORT, BOARD_SWCLK_PIN);
}

/* JTAG 里 TCK 与 SWCLK 是同一根线：别名由 cherrydap 的 JTAG_DP.c 自己定义
 * （`#define PIN_TCK_SET PIN_SWCLK_TCK_SET`），这里不要再定义一遍。 */

/* ------------------------------ SWDIO/TMS ------------------------------ */

__STATIC_FORCEINLINE uint32_t PIN_SWDIO_TMS_IN(void) {
    return PIN_READ(BOARD_SWD_PORT, BOARD_SWDIO_IN_PIN) ? 1U : 0U;
}

__STATIC_FORCEINLINE void PIN_SWDIO_TMS_SET(void) {
    PIN_SET(BOARD_SWD_PORT, BOARD_SWDIO_OUT_PIN);
}

__STATIC_FORCEINLINE void PIN_SWDIO_TMS_CLR(void) {
    PIN_CLR(BOARD_SWD_PORT, BOARD_SWDIO_OUT_PIN);
}

__STATIC_FORCEINLINE uint32_t PIN_SWDIO_IN(void) {
    return PIN_READ(BOARD_SWD_PORT, BOARD_SWDIO_IN_PIN) ? 1U : 0U;
}

__STATIC_FORCEINLINE void PIN_SWDIO_OUT(uint32_t bit) {
    if ((bit & 1U) != 0U) {
        PIN_SET(BOARD_SWD_PORT, BOARD_SWDIO_OUT_PIN);
    } else {
        PIN_CLR(BOARD_SWD_PORT, BOARD_SWDIO_OUT_PIN);
    }
}

/* SWDIO 方向切换：改 cfgr 的 iomc 字段（输出=1，输入=0）。
 * 读相位（奇偶校验回读、ACK）必须放开输出，否则本板 MISO/MOSI 都接在同一根
 * SWDIO 上，会和目标互相打。 */
__STATIC_FORCEINLINE void swdio_dir_output(void) {
    BOARD_SWD_PORT->cfgr = (BOARD_SWD_PORT->cfgr & ~SWDIO_CFGR_MASK) |
                           ((uint32_t)GPIO_MODE_OUTPUT << SWDIO_CFGR_SHIFT);
}

__STATIC_FORCEINLINE void swdio_dir_input(void) {
    BOARD_SWD_PORT->cfgr &= ~SWDIO_CFGR_MASK;
    BOARD_SWD_PORT->pull = (BOARD_SWD_PORT->pull & ~(3UL << SWDIO_CFGR_SHIFT)) |
                           ((uint32_t)GPIO_PULL_UP << SWDIO_CFGR_SHIFT);
}

__STATIC_FORCEINLINE void PIN_SWDIO_OUT_ENABLE(void) {
    swdio_dir_output();
}

__STATIC_FORCEINLINE void PIN_SWDIO_OUT_DISABLE(void) {
    swdio_dir_input();
}

/* JTAG 里 TMS 与 SWDIO 是同一根线：别名同样由 JTAG_DP.c 自己定义。 */

/* -------------------------------- TDI/TDO ------------------------------ */

__STATIC_FORCEINLINE uint32_t PIN_TDI_IN(void) {
    return PIN_READ(BOARD_JTAG_PORT, BOARD_TDI_PIN) ? 1U : 0U;
}

__STATIC_FORCEINLINE void PIN_TDI_OUT(uint32_t bit) {
    if ((bit & 1U) != 0U) {
        PIN_SET(BOARD_JTAG_PORT, BOARD_TDI_PIN);
    } else {
        PIN_CLR(BOARD_JTAG_PORT, BOARD_TDI_PIN);
    }
}

__STATIC_FORCEINLINE uint32_t PIN_TDO_IN(void) {
    return PIN_READ(BOARD_JTAG_PORT, BOARD_TDO_PIN) ? 1U : 0U;
}

/* ------------------------------ 其它 ---------------------------------- */



#define LED_CONNECTED_OUT(bit) drv_led_set_dap_connected((bit) != 0U)
#define LED_RUNNING_OUT(bit)   ((void)(bit))

/* ================================= 时钟 ================================= */

#define TIMESTAMP_GET() (0U)

/* ============================== 初始化/复位 ============================= */

__STATIC_INLINE void DAP_SETUP(void) {
    crm_periph_clock_enable(BOARD_SWD_GPIO_CLK, TRUE);
    crm_periph_clock_enable(BOARD_JTAG_GPIO_CLK, TRUE);

    LED_CONNECTED_OUT(0);
    PORT_OFF();
}

/* 复位目标：拉低 nRESET 20 ms 再释放。返回 1 表示支持该操作（DAP.c 会把它
 * 直接填进应答包，所以必须是 uint8_t 而不是 void）。 */
__STATIC_INLINE uint8_t RESET_TARGET(void) {
    PIN_nRESET_OUT(0U);
    Delayms(20U);
    PIN_nRESET_OUT(1U);
    return 1U;
}

#ifdef __cplusplus
}
#endif
