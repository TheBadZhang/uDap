#pragma once

#include <stdint.h>

#include <ch32x035.h>

// 引脚方案（SWD_PIN_VARIANT_NAME = pa5_pa7 / pa5_pa6 / pa2_pa3），实现里的符号用它加后缀
#include "swd_pin_map.h"

/*
 * SWD 物理层里「相位级」的那几个函数：端口生命周期 + 三段位时序汇编。
 *
 * 三层划分：
 *   src/swd_pin_map.h        引脚本身（哪个 bit、哪个 CFGLR 字段）与每次调用的
 *                            inline 原语（CLK/DIO 电平、DIO 方向），换板只改这里
 *   src/dap/swd_gpio.h       本文件：相位级函数的**直接声明**
 *   src/dap/swd_gpio_x035.c  芯片内置实现（CH32X035 GPIO + 三段位时序汇编）
 *
 * ---------------------------------------------------------------------------
 * 为什么**没有**函数指针表 / swd_gpio_bind()
 * ---------------------------------------------------------------------------
 * 以前这里有一张 `swd_gpio_if_t` + `swd_gpio_bind()`，想在运行期换实现。但：
 *   1. 表里的成员都是函数指针，编译器**永远内联不了** —— 即便 LTO 把调用目标
 *      常量传播成直接调用，每次仍是「auipc+jalr → 函数体 → ret」；
 *   2. 表里这些函数本来就跟引脚方案编译期绑死（汇编里的立即数全是
 *      SWD_PIN_VARIANT 推出来的），所谓「运行期换实现」根本用不上；
 *   3. 每比特一次的边沿级原语（CLK/DIO 置清、DIO 方向切换）从一开始就必须是
 *      内联宏（见 src/swd_pin_map.h），本来也没进表。
 * 所以现在直接按名字调用：符号由 SWD_GPIO_IMPL() 加上引脚方案后缀，一处编译、
 * 一处绑定，没有任何指针访问。
 *
 * 换芯片/换引脚方案时的动作：
 *   - 换脚：只改 src/swd_pin_map.h 的 SWD_PIN_VARIANT；
 *   - 换实现（SPI 转 SWD、外挂 MCU…）：照本文件改声明、改 swd_gpio_x035.c
 *     的宏定义，两个使用方（sw_dp.c / DAP_config.h）只认下面的宏名。
 */

/* ---------------------------------------------------------------------------
 * 符号命名：所有实现里的符号都带**引脚方案后缀**
 * ---------------------------------------------------------------------------
 * 例如 SWD_PIN_VARIANT = SWD_PIN_VARIANT_PA5_PA6 时：
 *
 *     swd_fast_packet_request_pa5_pa6
 *     swd_fast_read_data_pa5_pa6
 *     swd_fast_write_data_pa5_pa6
 *     x035_port_setup_pa5_pa6
 *
 * 好处：map / nm / 调试器里一眼能看出这份固件编的是哪套脚。
 */
#define SWD_GPIO_CAT_(a, b) a##b
#define SWD_GPIO_CAT(a, b) SWD_GPIO_CAT_(a, b)

/* 给符号加本构建的引脚方案后缀：SWD_GPIO_IMPL(swd_fast_write_data)
 *   -> swd_fast_write_data_pa5_pa6 */
#define SWD_GPIO_IMPL(base) SWD_GPIO_CAT(SWD_GPIO_CAT(base, _), SWD_PIN_VARIANT_NAME)

/* 三段位时序（几百字节汇编）**必须**各自成函数、不许内联：它们被 SWD_Transfer
 * 里各调一次，内联后 .highcode.dap 会凭空长出 1.6 KB —— 那段是**常驻 RAM**，
 * 双模式固件根本装不下。关掉内联每条相位只多一次 call/ret，相对 32 拍位时序
 * 可以忽略（实测吞吐在噪声内）。 */
#define SWD_GPIO_NOINLINE __attribute__((noinline))

/* ---------------------------------------------------------------------------
 * 相位级函数（实现在 src/dap/swd_gpio_x035.c）
 * ---------------------------------------------------------------------------
 * port_setup / port_off：每次 DAP 连接/断开各一次
 *   —— CLK/DIO 设为推挽输出、DIO 空闲高 / 两脚释放为浮空输入（不驱动目标）
 * packet_request：8 拍的请求头（start/APnDP/RnW/A2/A3/parity/stop）
 * read_data     ：32 个数据位，顺带算出 CRC 用的奇偶位
 * write_data    ：32 个数据位
 * 后三者自己负责位时序（含 SWCLK 占空比），调用方不插手。
 */
void SWD_GPIO_IMPL(x035_port_setup)(void);
void SWD_GPIO_IMPL(x035_port_off)(void);
void SWD_GPIO_IMPL(swd_fast_packet_request)(uint32_t request);
void SWD_GPIO_IMPL(swd_fast_read_data)(uint32_t *value, uint32_t *parity);
void SWD_GPIO_IMPL(swd_fast_write_data)(uint32_t value);

/* 使用方只认这几个名字（sw_dp.c / DAP_config.h）—— 换实现时改上面的声明即可。
 * 宏名与真实函数隔开一层，是不想让调用点出现「带引脚后缀」的长名字。 */
#define swd_gpio_port_setup()        SWD_GPIO_IMPL(x035_port_setup)()
#define swd_gpio_port_off()          SWD_GPIO_IMPL(x035_port_off)()
#define swd_gpio_packet_request(req) SWD_GPIO_IMPL(swd_fast_packet_request)((req))
#define swd_gpio_read_data(v, p)     SWD_GPIO_IMPL(swd_fast_read_data)((v), (p))
#define swd_gpio_write_data(val)     SWD_GPIO_IMPL(swd_fast_write_data)((val))
