#pragma once

#include <stdint.h>

/*
 * 1 ms 系统时基。
 *
 * sdk/bsp/bsp_delay.c 里的 bsp_delay_us/ms 用的是「SysTick 自由跑 + 读 VAL 做差」
 * 的忙等方式：它把 SysTick 配成 1 ms 周期但**不开中断**，所以只能延时、不能报时。
 * 而 LED 活动指示（脉冲多长）、vendor command 的延迟动作（应答发完再执行）都
 * 需要一个单调递增的毫秒计数，所以这里额外开 SysTick 的 TICKINT。
 *
 * 两者并不冲突：SysTick 的周期都是 1 ms，bsp_delay_us 靠 VAL 递减计“还差多少
 * 微秒”，每毫秒被重装一次对它来说是正常回绕（它本来就做了回绕补偿）。
 *
 * 注意：SysTick_Handler 在启动文件里是 weak 的，这里提供强符号。若以后某个
 * 应用想自己处理 SysTick，请改成调用 bsp_time_tick()。
 */

/* 配置 SysTick 为 1 ms 中断并使能（在 bsp_delay_init 之后调用） */
void bsp_time_init(void);

/* 上电以来的毫秒数（32 位，49 天回绕一次） */
uint32_t bsp_time_ms(void);

/* 判断是否已到达 deadline（自动处理 32 位回绕） */
static inline int bsp_time_reached(uint32_t now, uint32_t deadline) {
    return (int32_t)(now - deadline) >= 0;
}
