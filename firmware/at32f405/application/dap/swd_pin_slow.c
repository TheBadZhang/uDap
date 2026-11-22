/**
  ******************************************************************************
  * @file    swd_pin_slow.c
  * @brief   纯 C 位翻转物理层的引脚写入出口（故意不内联）
  ******************************************************************************
  * 为什么需要这两个函数：
  *
  * chip 的 GPIO 有 `scr` 寄存器，一次 32 位写就能置位/清零任意引脚，比
  * gpio_bits_set()/gpio_bits_reset() 的「读-改-写」快一倍。spi 版和 gpio_fast
  * （RAM 汇编内核）都用它，因为它们自己的时序由硬件移位 / 手写节拍保证。
  *
  * 但 cherrydap 原版 SW_DP.c 是纯 C 宏位翻转，27 MHz 档每个半周期只剩几条指令：
  * 一旦 pin 写被优化成单条 `str`，Flash 取指等待 + USB 中断抖动就会让 SWCLK 的
  * 高低电平比例散掉。实测（开 LTO 后 8 次里 1 次 `Unexpected ACK`）就是这个问题。
  *
  * 所以这里保留一次真实函数调用，把「取指 + 调用 + 库函数内部的读改写」当作半个
  * 周期的固有延时；`noinline` 让 LTO 也无法把它内联掉。稳定优先，速度靠
  * --swd_phy=gpio_fast / spi 去拿。
  ******************************************************************************
  */

#include "at32f402_405.h"

__attribute__((noinline, used)) void swd_pin_set_slow(gpio_type *port, uint16_t pins)
{
    gpio_bits_set(port, pins);
}

__attribute__((noinline, used)) void swd_pin_clear_slow(gpio_type *port, uint16_t pins)
{
    gpio_bits_reset(port, pins);
}
