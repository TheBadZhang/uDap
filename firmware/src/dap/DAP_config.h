#pragma once

#include <stdint.h>

#include <ch32x035.h>

#ifndef __STATIC_INLINE
#define __STATIC_INLINE static inline
#endif
#ifndef __STATIC_FORCEINLINE
#define __STATIC_FORCEINLINE __attribute__((always_inline)) static inline
#endif
#ifndef __WEAK
#define __WEAK __attribute__((weak))
#endif

#define CPU_CLOCK               48000000U
#define IO_PORT_WRITE_CYCLES    2U

// 快路径（SWD_TransferFast）的启用阈值。
//
// 判定点在 cherrydap 的 Set_Clock_Delay()（DAP/Source/DAP.c，__WEAK）：
//     if (clock >= MAX_SWJ_CLOCK(DELAY_FAST_CYCLES)) -> fast_clock = 1
//     MAX_SWJ_CLOCK(n) = (CPU_CLOCK / 2) / (IO_PORT_WRITE_CYCLES + n)
// 代入本文件的值与下面的 DELAY_FAST_CYCLES = 2U：
//     (48 MHz / 2) / (2 + 2) = 6 MHz
// 即**请求时钟 >= 6 MHz 就走快路径**（上游默认 DELAY_FAST_CYCLES = 0，阈值 12 MHz）。
//
// 为什么降到 6 MHz：快路径的实际 SWCLK 由固定指令数决定（约 6 MHz），
// **与请求值无关** —— 请求 6 MHz 与请求 12 MHz 拿到的是同一个速率。阈值留在
// 12 MHz 的唯一后果，是让主机填 6 ~ 11.5 MHz 时白跑慢路径（实测慢路径 56 KiB/s
// vs 快路径 129 KiB/s，见仓库根 readme.md 第 6.1 节）。6 MHz 以下仍走慢路径，
// 保留可调延时，供长线 / 慢目标使用。
//
// 改阈值只需改这个数（阈值 = 24 MHz / (2 + 本值)）：
//     0 -> 12 MHz（上游默认）    2 -> 6 MHz（本工程）    4 -> 4 MHz
//
// ⚠️ DELAY_FAST_CYCLES 在 DAP.h 里还有一个用途：决定 PIN_DELAY_FAST() 补几个 NOP。
// 在本工程里**不影响时序** —— sw_dp.c 已把 PIN_DELAY_FAST 重定义成固定一个
// __NOP()，且上游唯二使用它的 SW_DP.c / JTAG_DP.c 并未参与编译（见 xmake.lua）。
// 也就是说这个宏在本工程里只当阈值旋钮用。
#ifndef DELAY_FAST_CYCLES
#define DELAY_FAST_CYCLES 2U
#endif

// 慢路径（SWD_TransferSlow）的单次位延时
//
#ifndef DELAY_SLOW_CYCLES
#define DELAY_SLOW_CYCLES 8U
#endif

__STATIC_FORCEINLINE void x035_pin_delay_slow(uint32_t delay) {
  uint32_t count = delay;
  while (--count) {
    __NOP();
  }
}


#define DAP_SWD                 1
#define DAP_JTAG                0
#define DAP_JTAG_DEV_CNT        1U
#define DAP_DEFAULT_PORT        1U
#define DAP_DEFAULT_SWJ_CLOCK   1000000U
#define DAP_PACKET_SIZE         64U
#define DAP_PACKET_COUNT        8U
#define SWO_UART                0
#define SWO_MANCHESTER          0
#define SWO_STREAM              0
#define SWO_BUFFER_SIZE         256U
#define TIMESTAMP_CLOCK         0U
#define DAP_UART                0
#define DAP_UART_USB_COM_PORT   1
#define DAP_UART_DRIVER         0
#define DAP_UART_RX_BUFFER_SIZE 64U
#define DAP_UART_TX_BUFFER_SIZE 64U
#define DAP_FW_VER              "0.1.0"

__STATIC_INLINE uint8_t DAP_GetVendorString(char *str) {
    (void)str;
    return 0U;
}
__STATIC_INLINE uint8_t DAP_GetProductString(char *str) {
    (void)str;
    return 0U;
}

// 序列号由 src/dap/dap_main.c 提供：USB 描述符的 iSerialNumber 与这里的
// DAP_GetSerNum 命令共用同一份字符串，避免两条路径报告不同的序列号
// （后者只在主机主动查询 DAP_ID_SER_NUM 时才用到，如 pyOCD 的探针列表）。
//
// 声明放在本文件而不是 dap_main.h：dap_main.h 会 include 本文件，反向包含会成环。
extern const char *udap_serial_number(void);

// 返回写入的字符数（不含结束符）。CMSIS-DAP 规定字符串最长 32 字符，
// 而 DAP_Info 的响应缓冲就是 DAP_PACKET_SIZE(64)，所以 32 是安全上限。
__STATIC_INLINE uint8_t DAP_GetSerNumString(char *str) {
    const char *sn = udap_serial_number();
    uint8_t n = 0U;

    if (sn != NULL) {
        while (sn[n] != '\0' && n < 32U) {
            str[n] = sn[n];
            n++;
        }
    }
    return n;
}

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
    (void)str;
    return 0U;
}

// PA2 为 SWCLK，PA3 为 SWDIO，PA5 为 nRESET（低有效）
// BSHR 低 16 位将输出置高，BCR 将输出拉低
__STATIC_FORCEINLINE uint32_t PIN_SWCLK_TCK_IN(void) { return (GPIOA->INDR & GPIO_Pin_2) != 0U; }
__STATIC_FORCEINLINE void PIN_SWCLK_TCK_SET(void) { GPIOA->BSHR = GPIO_Pin_2; }
__STATIC_FORCEINLINE void PIN_SWCLK_TCK_CLR(void) { GPIOA->BCR = GPIO_Pin_2; }
__STATIC_FORCEINLINE uint32_t PIN_SWDIO_TMS_IN(void) { return (GPIOA->INDR & GPIO_Pin_3) != 0U; }
__STATIC_FORCEINLINE void PIN_SWDIO_TMS_SET(void) { GPIOA->BSHR = GPIO_Pin_3; }
__STATIC_FORCEINLINE void PIN_SWDIO_TMS_CLR(void) { GPIOA->BCR = GPIO_Pin_3; }
#define PIN_SWDIO_IN() ((GPIOA->INDR & GPIO_Pin_3) != 0U)
// 在 BSHR 的置位与复位半字间选择，避免每个数据位都执行条件分支
#define PIN_SWDIO_OUT(bit) {GPIOA->BSHR = (GPIO_Pin_3 << 16) >> ((bit & 1U) << 4);}
// 设置 SWDIO 数据，并将 SWCLK 原子拉低，为下一个 SWD 数据位准备
__STATIC_FORCEINLINE void PIN_SWDIO_OUT_SWCLK_CLR(uint32_t bit) {
    // 同时设置 PA3 数据并拉低 PA2，bit 为 0 时选择 BSHR 的复位半字
    GPIOA->BSHR = ((GPIO_Pin_3 << 16) >> ((bit & 1U) << 4));
}
// CFGLR 为 PA0 至 PA7 每个引脚分配 4 位，0x1 为推挽输出，0x4 为浮空输入
__STATIC_FORCEINLINE void PIN_SWDIO_OUT_ENABLE(void) {
    // PA3 的 12 至 15 位，从浮空输入切换为推挽输出
    GPIOA->CFGLR = (GPIOA->CFGLR & ~(0xFU << 12)) | (0x1U << 12);
}
__STATIC_FORCEINLINE void PIN_SWDIO_OUT_DISABLE(void) {
    // PA3 的 12 至 15 位，释放 SWDIO 供目标驱动
    GPIOA->CFGLR = (GPIOA->CFGLR & ~(0xFU << 12)) | (0x4U << 12);
}

__STATIC_FORCEINLINE uint32_t PIN_TDI_IN(void) { return 0U; }
__STATIC_FORCEINLINE void PIN_TDI_OUT(uint32_t bit) { (void)bit; }
__STATIC_FORCEINLINE uint32_t PIN_TDO_IN(void) { return 0U; }
__STATIC_FORCEINLINE uint32_t PIN_nTRST_IN(void) { return 1U; }
__STATIC_FORCEINLINE void PIN_nTRST_OUT(uint32_t bit) { (void)bit; }

// PA5 为 nRESET，低电平有效。它在 CFGLR 中占第 20 至 23 位（每脚 4 位）
#define PIN_nRESET_PIN   GPIO_Pin_5
#define PIN_nRESET_SHIFT 20U

// nRESET 采用「开漏模拟」，这是 CMSIS-DAP 推荐的接法：
//   释放（bit=1）：切为输入 + 内部上拉（高阻）。电平交给目标板自己的上拉 /
//                  复位电路决定，DAP 不会与目标侧的复位驱动对顶；此时读 INDR
//                  得到的是真实线电平，主机才看得出目标是否正把复位拉住。
//   复位（bit=0）：推挽输出拉低。
// 两条路径都「先设好输出数据、再改 CFGLR」，避免切换瞬间输出旧电平产生毛刺。
__STATIC_FORCEINLINE uint32_t PIN_nRESET_IN(void) {
    return (GPIOA->INDR & PIN_nRESET_PIN) != 0U;
}

__STATIC_FORCEINLINE void PIN_nRESET_OUT(uint32_t bit) {
    if (bit != 0U) {
        GPIOA->BSHR = PIN_nRESET_PIN;  // ODR 置 1；下面选输入模式时即取上拉
        GPIOA->CFGLR = (GPIOA->CFGLR & ~(0xFU << PIN_nRESET_SHIFT)) | (0x8U << PIN_nRESET_SHIFT);
    } else {
        GPIOA->BCR = PIN_nRESET_PIN;   // 先让输出数据为低
        GPIOA->CFGLR = (GPIOA->CFGLR & ~(0xFU << PIN_nRESET_SHIFT)) | (0x1U << PIN_nRESET_SHIFT);
    }
}

__STATIC_INLINE void PORT_JTAG_SETUP(void) {}

__STATIC_INLINE void PORT_SWD_SETUP(void) {
    // PA2 为 SWCLK 输出，PA3 为 SWDIO 输出（读时由传输函数切输入）
    // 每个 PA0 至 PA7 的 CFGLR 字段占用 4 位，PA2 在 8 至 11 位，PA3 在 12 至 15 位
    GPIOA->BSHR = GPIO_Pin_2 | GPIO_Pin_3;
    GPIOA->CFGLR = (GPIOA->CFGLR & ~((0xFU << 8) | (0xFU << 12))) | (0x1U << 8) | (0x1U << 12);
    // 连接时释放目标复位（CMSIS-DAP 规定 nRESET 初始为高）
    PIN_nRESET_OUT(1U);
}

__STATIC_INLINE void PORT_OFF(void) {
    // DAP 端口关闭时，将 PA2、PA3 释放为浮空输入
    GPIOA->CFGLR = (GPIOA->CFGLR & ~((0xFU << 8) | (0xFU << 12))) | (0x4U << 8) | (0x4U << 12);
    // 同时释放 nRESET：断开后不能把目标一直摁在复位里
    PIN_nRESET_OUT(1U);
}

//**************************************************************************************************
// 状态指示灯
//
// 本板只有一颗灯，接在 PB12，**高电平点亮**。
// 它对应 CMSIS-DAP 的 Connect LED：主机（调试器软件）连接到 DAP 时点亮。
//
// 由 DAP.c 的 DAP_HostStatus(ID_DAP_HostStatus=0x01) 驱动：
//   request[0] = DAP_DEBUGGER_CONNECTED(0) -> LED_CONNECTED_OUT(request[1])
//   request[0] = DAP_TARGET_RUNNING(1)     -> LED_RUNNING_OUT(request[1])
// pyOCD 在 connect() 置 CONNECTED=1、disconnect() 置 0；
// 它从不把 TARGET_RUNNING 置 1，故本板不为 Running 单独点灯。
#define LED_CONNECTED_PORT        GPIOB
#define LED_CONNECTED_PIN         GPIO_Pin_12
// 1 = 高电平点亮，0 = 低电平点亮
#define LED_CONNECTED_ACTIVE_HIGH 1U

__STATIC_INLINE void LED_CONNECTED_OUT(uint32_t bit) {
#if LED_CONNECTED_ACTIVE_HIGH
    if (bit != 0U) {
        LED_CONNECTED_PORT->BSHR = LED_CONNECTED_PIN;  // 置高，点亮
    } else {
        LED_CONNECTED_PORT->BCR = LED_CONNECTED_PIN;   // 拉低，熄灭
    }
#else
    if (bit != 0U) {
        LED_CONNECTED_PORT->BCR = LED_CONNECTED_PIN;   // 拉低，点亮
    } else {
        LED_CONNECTED_PORT->BSHR = LED_CONNECTED_PIN;  // 置高，熄灭
    }
#endif
}

// 本板只有一颗灯，未接 Target Running 指示
__STATIC_INLINE void LED_RUNNING_OUT(uint32_t bit) {
    (void)bit;
}

__STATIC_INLINE uint32_t TIMESTAMP_GET(void) {
    return 0U;
}

__STATIC_INLINE void DAP_SETUP(void) {
    // GPIOB 给状态灯（PB12）用，GPIOA 给 SWD / nRESET 用
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB, ENABLE);

    // 先把输出数据寄存器置为「灭」，再切成推挽输出，避免初始化瞬间闪一下。
    // （高电平点亮时「灭」= 低，与复位值一致；低电平点亮时这一步才是必需的）
    LED_CONNECTED_OUT(0U);

    GPIO_InitTypeDef led = {0};
    led.GPIO_Pin = LED_CONNECTED_PIN;
    led.GPIO_Speed = GPIO_Speed_50MHz;
    led.GPIO_Mode = GPIO_Mode_Out_PP;
    // 用库函数而不是裸写 CFGHR：SDK 的 GPIO_Init 对 CFGHR 走静态缓存
    // （CFGHR_tmpB），直接写寄存器会让缓存失步，后续 GPIO_Init 调用会把配置冲掉。
    GPIO_Init(LED_CONNECTED_PORT, &led);

    PORT_OFF();
}

// DAP.c 里定义，这里先声明：本头文件在 dap_main.h 中先于 DAP.h 被包含，
// 不声明会被当成隐式声明报错
extern void Delayms(uint32_t delay);

// 设备专用复位序列：拉低 PA5 保持 20 ms 再释放。
// 返回 1 表示已实现 —— 主机（OpenOCD / pyocd）执行 reset 时会优先走这里，
// 而不是只靠 SWJ_Pins 逐次拉线。
__STATIC_INLINE uint8_t RESET_TARGET(void) {
    PIN_nRESET_OUT(0U);
    Delayms(20U);
    PIN_nRESET_OUT(1U);
    return 1U;
}
