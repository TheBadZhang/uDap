#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <ch32x035.h>

// 引脚方案（SWD_PIN_VARIANT_NAME = pa5_pa6 / pa2_pa3），实现里的符号用它加后缀
#include "swd_pin_map.h"

/*
 * SWD 物理层接口（PHY interface）。
 *
 * 三层划分：
 *   src/swd_pin_map.h        引脚本身（哪个 bit、哪个 CFGLR 字段），换板只改这里
 *   src/dap/swd_gpio.h       本文件：GPIO 描述 + 实现接口 + 绑定/查询
 *   src/dap/swd_gpio_x035.c  芯片内置实现（CH32X035 GPIO + 三段位时序汇编）
 *
 * 换驱动只做两件事：
 *   1. 写一份 `const swd_gpio_if_t`（比如 SPI 转 SWD、外挂 MCU、别的端口）；
 *   2. 启动时 `swd_gpio_bind(&你的实现)`，或者要固定成某一份就直接改
 *      `swd_gpio_current` 的初值。sw_dp.c / DAP_config.h 一行都不用动。
 *
 * ---------------------------------------------------------------------------
 * ⚠️ 接口粒度：只放「相位级」操作，不放「边沿级」原语
 * ---------------------------------------------------------------------------
 * 这里的函数都是**每次 SWD 事务调用几次**（一次事务有几十上百个比特），所以
 * 一次间接调用的开销可以忽略。
 *
 * 每比特一次的操作（CLK 置/清、DIO 置/清/读、DIO 切输入输出方向）**故意不进
 * 接口表**：快路径的一个 SWCLK 周期就等于「两条 store 之间的指令数」，中间多
 * 一次间接调用会直接把慢路径的顶速拉低约 20%，也会破坏 sw_dp.c 里标定过的
 * 占空比。它们由所选实现的内联宏提供（见 src/swd_pin_map.h + DAP_config.h），
 * 属于编译期绑定。
 */

/* 一组 SWD 引脚（CLK 输出 + DIO 双向）的描述。
 * 数据来自 src/swd_pin_map.h，因此换板只需改那一个头文件。 */
typedef struct {
    const char *name;   /* 实现名，便于诊断/日志 */
    GPIO_TypeDef *port; /* 两个脚必须同属一个 port（快路径要合并写 BSHR） */
    uint32_t clk_pin;   /* 引脚掩码，如 GPIO_Pin_5 */
    uint32_t dio_pin;
    uint8_t clk_shift;  /* CFGLR 里该脚的字段移位 = bit * 4 */
    uint8_t dio_shift;
} swd_gpio_desc_t;

/* SWD 物理层的一份实现 */
typedef struct {
    const swd_gpio_desc_t *desc;

    /* 端口生命周期（每次 DAP 连接/断开各一次） */
    void (*port_setup)(void); /* CLK/DIO 设为输出、DIO 空闲高 */
    void (*port_off)(void);   /* 两脚释放为浮空输入（不驱动目标） */

    /* DIO 方向切换（每次事务的 turnaround 一次） */
    void (*dio_out_enable)(void);
    void (*dio_out_disable)(void);

    /*
     * 数据相位的批量实现（快路径）。**三者要么全给，要么全为 NULL**：
     * 任一为 NULL 时 SWD_Transfer 会整体走慢路径，不会半快半慢。
     *   packet_request: 8 拍的请求头（start/APnDP/RnW/A2/A3/parity/stop）
     *   read_data     : 32 个数据位，顺带算出 CRC 用的奇偶位
     *   write_data    : 32 个数据位
     * 这些实现自己负责位时序（含 SWCLK 占空比），接口不插手。
     */
    void (*packet_request)(uint32_t request);
    void (*read_data)(uint32_t *value, uint32_t *parity);
    void (*write_data)(uint32_t value);
} swd_gpio_if_t;

/* ---------------------------------------------------------------------------
 * 符号命名：所有实现里的符号都带**引脚方案后缀**
 * ---------------------------------------------------------------------------
 * swd_gpio_x035.c 里的三个位时序函数与实现表都由引脚方案名拼出来，
 * 例如 SWD_PIN_VARIANT = SWD_PIN_VARIANT_PA5_PA6 时：
 *
 *     swd_fast_packet_request_pa5_pa6
 *     swd_fast_read_data_pa5_pa6
 *     swd_fast_write_data_pa5_pa6
 *     swd_gpio_if_x035_pa5_pa6
 *
 * 好处：map / nm / 调试器里一眼能看出这份固件编的是哪套脚；将来两套方案
 * 各编一份实现（两份 TU，各自 #define SWD_PIN_VARIANT 后包含同一份源码）
 * 符号也不会冲突，可以在运行期用 swd_gpio_bind() 选。
 */
#define SWD_GPIO_CAT_(a, b) a##b
#define SWD_GPIO_CAT(a, b) SWD_GPIO_CAT_(a, b)

/* 给符号加本构建的引脚方案后缀：SWD_GPIO_IMPL(swd_fast_write_data)
 *   -> swd_fast_write_data_pa5_pa6 */
#define SWD_GPIO_IMPL(base) SWD_GPIO_CAT(SWD_GPIO_CAT(base, _), SWD_PIN_VARIANT_NAME)

/* 每套引脚方案各一份实现表（都声明，实际只有被编译的那份有定义） */
extern const swd_gpio_if_t swd_gpio_if_x035_pa5_pa7;
extern const swd_gpio_if_t swd_gpio_if_x035_pa5_pa6;
extern const swd_gpio_if_t swd_gpio_if_x035_pa2_pa3;

/* 本构建选中的实现：由 src/swd_pin_map.h 的 SWD_PIN_VARIANT 决定 */
#define swd_gpio_if_x035 SWD_GPIO_CAT(swd_gpio_if_x035_, SWD_PIN_VARIANT_NAME)

/* 当前实现（默认指向芯片内置实现，可在启动时改绑） */
extern const swd_gpio_if_t *swd_gpio_current;

/* 取当前实现。热路径上只是读一个全局指针，开销可忽略。 */
static inline __attribute__((always_inline)) const swd_gpio_if_t *swd_gpio_if(void) {
    return swd_gpio_current;
}

/* 换绑实现。传 NULL 恢复默认（芯片内置实现）。
 * 必须在任何 SWD 操作之前调用（DAP_Setup() 之前）。 */
void swd_gpio_bind(const swd_gpio_if_t *iface);

/* 该实现是否具备快路径三段位时序（全给才算有） */
static inline __attribute__((always_inline)) bool swd_gpio_has_fast_path(const swd_gpio_if_t *iface) {
    return iface->packet_request != NULL && iface->read_data != NULL && iface->write_data != NULL;
}
