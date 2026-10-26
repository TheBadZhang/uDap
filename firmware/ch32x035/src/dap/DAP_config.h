#pragma once

#include <stdint.h>

#include <ch32x035.h>

// SWD 引脚映射（SWCLK/SWDIO 的引脚号、CFGLR 移位、nRESET 是否启用）。
// 同一个头文件也被 src/wchlink/transport/rvswd_phy_gpio.c 使用 ——
// CMSIS-DAP 与 WCH-Link 用的是板上同一对引脚。
#include "swd_pin_map.h"

// SWD 物理层接口（GPIO 描述 + 可替换的实现表）。
// 本文件里的边沿级原语是编译期展开的；相位级操作（端口配置、三段位时序）
// 走这里的函数表，换驱动只改绑定，不用改本文件。
#include "swd_gpio.h"

// 板级引脚（状态灯 PB11/PB12 等）
#include "board_pin_map.h"

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
// DAP 协议包大小（DAP_Info 上报给主机，决定主机一次能发/收多少字节）。
//
// ⚠️ 在这颗芯片上**必须等于 USB 端点 MPS（64）**，不能放大。
// 实测把本值改成 256 后，主机（OpenOCD 的 write_memory）会把 1024 个 word 的
// block 写一次性发过来（4101 字节），而 CH32X035 的 USBFS **只上报每个包的长度、
// 不提供本次传输的总长度**，固件只能按「凑满请求长度或收到短包」判断一条命令结束
// —— 于是长命令被切成几段当成多条命令解析，响应错位，报
//     CMSIS-DAP command mismatch. Sent 0x3 received 0x5
// 所以这里保持 64：主机遵守它，一条命令就是一个包。
//
// 想提高 DAP_TransferBlock 的批量只能靠减少「每条命令的往返延迟」，那是主机的
// 事（见 tools/speed.ps1 的实测对比：pyOCD 读 216 KiB/s vs 定制 OpenOCD 21 KiB/s）。
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

// SWCLK = PA5，SWDIO = PA6（引脚号见 src/swd_pin_map.h 的 SWD_PIN_VARIANT）
//
// ⚠️ 这一层是**每比特**调用的边沿级原语，必须是编译期展开的 inline：
// 快路径的一个 SWCLK 周期就等于两条 store 之间的指令数，中间插一次间接调用
// 会直接破坏标定好的占空比。所以它们**只做转发**，寄存器写法全部在板级层
// src/swd_pin_map.h 的 board_swd_* 里（那里也是 always_inline，展开后指令相同）；
// 这些名字本身是 CMSIS-DAP 的 DAP_config.h 契约 —— 第三方 DAP.c 直接引用，
// 不能改名也不能搬离本文件（见 src/dap/swd_gpio.h 的三层说明）。
__STATIC_FORCEINLINE uint32_t PIN_SWCLK_TCK_IN(void) { return board_swd_clk_level(); }
__STATIC_FORCEINLINE void PIN_SWCLK_TCK_SET(void) { board_swd_clk_set(); }
__STATIC_FORCEINLINE void PIN_SWCLK_TCK_CLR(void) { board_swd_clk_clr(); }
__STATIC_FORCEINLINE uint32_t PIN_SWDIO_TMS_IN(void) { return board_swd_dio_level(); }
__STATIC_FORCEINLINE void PIN_SWDIO_TMS_SET(void) { board_swd_dio_set(); }
__STATIC_FORCEINLINE void PIN_SWDIO_TMS_CLR(void) { board_swd_dio_clr(); }
#define PIN_SWDIO_IN() (board_swd_dio_level() != 0U)

// DIO 方向切换：每次事务的 turnaround 一次，走接口表（可被别的实现替换）
__STATIC_FORCEINLINE void PIN_SWDIO_OUT_ENABLE(void) { swd_gpio_if()->dio_out_enable(); }
__STATIC_FORCEINLINE void PIN_SWDIO_OUT_DISABLE(void) { swd_gpio_if()->dio_out_disable(); }

__STATIC_FORCEINLINE uint32_t PIN_TDI_IN(void) { return 0U; }
__STATIC_FORCEINLINE void PIN_TDI_OUT(uint32_t bit) { (void)bit; }
__STATIC_FORCEINLINE uint32_t PIN_TDO_IN(void) { return 0U; }
__STATIC_FORCEINLINE uint32_t PIN_nTRST_IN(void) { return 1U; }
__STATIC_FORCEINLINE void PIN_nTRST_OUT(uint32_t bit) { (void)bit; }

// nRESET（目标复位，低有效）是否启用由 src/swd_pin_map.h 决定：
// 本板 /NRST 没有连到 MCU，PA5 又让给了 SWCLK，所以 SWD_NRESET_ENABLE = 0，
// 三个操作全部退化为空操作（详见该头文件的说明）。
#if SWD_NRESET_ENABLE

// nRESET 在 CFGLR 中占第 SWD_NRESET_CFGLR_SHIFT 起的 4 位（每脚 4 位）
#define PIN_nRESET_PIN   SWD_NRESET_PIN
#define PIN_nRESET_SHIFT SWD_NRESET_CFGLR_SHIFT

// nRESET 采用「开漏模拟」，这是 CMSIS-DAP 推荐的接法：
//   释放（bit=1）：切为输入 + 内部上拉（高阻）。电平交给目标板自己的上拉 /
//                  复位电路决定，DAP 不会与目标侧的复位驱动对顶；此时读 INDR
//                  得到的是真实线电平，主机才看得出目标是否正把复位拉住。
//   复位（bit=0）：推挽输出拉低。
// 两条路径都「先设好输出数据、再改 CFGLR」，避免切换瞬间输出旧电平产生毛刺。
__STATIC_FORCEINLINE uint32_t PIN_nRESET_IN(void) {
    return (SWD_PORT->INDR & PIN_nRESET_PIN) != 0U;
}

__STATIC_FORCEINLINE void PIN_nRESET_OUT(uint32_t bit) {
    if (bit != 0U) {
        SWD_PORT->BSHR = PIN_nRESET_PIN;  // ODR 置 1；下面选输入模式时即取上拉
        SWD_PORT->CFGLR = (SWD_PORT->CFGLR & ~(0xFU << PIN_nRESET_SHIFT)) | (0x8U << PIN_nRESET_SHIFT);
    } else {
        SWD_PORT->BCR = PIN_nRESET_PIN;   // 先让输出数据为低
        SWD_PORT->CFGLR = (SWD_PORT->CFGLR & ~(0xFU << PIN_nRESET_SHIFT)) | (0x1U << PIN_nRESET_SHIFT);
    }
    board_swd_cfglr_resync(); /* 动过 CFGLR，SWD 的影子要重建 */
}

#else /* !SWD_NRESET_ENABLE */

// 没有 nRESET 引脚：读回恒为「未被拉低」，写操作忽略。
__STATIC_FORCEINLINE uint32_t PIN_nRESET_IN(void) { return 1U; }
__STATIC_FORCEINLINE void PIN_nRESET_OUT(uint32_t bit) { (void)bit; }

#endif /* SWD_NRESET_ENABLE */

__STATIC_INLINE void PORT_JTAG_SETUP(void) {}

// 端口生命周期交给当前实现（默认芯片内置实现，见 src/dap/swd_gpio.h）。
// 这两支只在 DAP 连接/断开时各调一次，走函数表不影响任何时序。
__STATIC_INLINE void PORT_SWD_SETUP(void) {
    swd_gpio_if()->port_setup();
    // 连接时释放目标复位（CMSIS-DAP 规定 nRESET 初始为高；未接引脚时为空操作）
    PIN_nRESET_OUT(1U);
}

__STATIC_INLINE void PORT_OFF(void) {
    // DAP 端口关闭：两脚释放为浮空输入（由实现决定具体寄存器写法）
    swd_gpio_if()->port_off();
    // 同时释放 nRESET：断开后不能把目标一直摁在复位里
    PIN_nRESET_OUT(1U);
}

//**************************************************************************************************
// 状态指示灯
//
// 本板有两颗灯，都是**低电平点亮**（引脚见 src/board_pin_map.h）：
//   PB11 = DAP 指示灯：CMSIS-DAP 的 Connect LED，主机（调试器软件）连接到 DAP
//          时点亮，由本文件的 LED_CONNECTED_OUT() 驱动。
//   PB12 = UART 指示灯：亮 = 双串口模式，灭 = 串口1 + DTR/RTS 模式，
//          由 src/drv/drv_uart.c 的模式小节驱动（不在这里，避免两处抢同一个引脚）。
//
// 由 DAP.c 的 DAP_HostStatus(ID_DAP_HostStatus=0x01) 驱动：
//   request[0] = DAP_DEBUGGER_CONNECTED(0) -> LED_CONNECTED_OUT(request[1])
//   request[0] = DAP_TARGET_RUNNING(1)     -> LED_RUNNING_OUT(request[1])
// pyOCD 在 connect() 置 CONNECTED=1、disconnect() 置 0；
// 它从不把 TARGET_RUNNING 置 1，故本板不为 Running 单独点灯。
#define LED_CONNECTED_PORT        BOARD_LED_PORT
#define LED_CONNECTED_PIN         BOARD_LED_DAP_PIN
// 1 = 高电平点亮，0 = 低电平点亮（跟随 board_pin_map.h 的 BOARD_LED_ACTIVE_LOW）
#define LED_CONNECTED_ACTIVE_HIGH (BOARD_LED_ACTIVE_LOW ? 0 : 1)

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
    // GPIOB 给状态灯（PB12）用，GPIOA 给 SWD 用（SWCLK = PA5 / SWDIO = PA6）
    RCC_APB2PeriphClockCmd(SWD_PORT_CLOCK | RCC_APB2Periph_GPIOB, ENABLE);

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

// 设备专用复位序列：拉低 nRESET 保持 20 ms 再释放。
// 返回 1 表示已实现 —— 主机（OpenOCD / pyocd）执行 reset 时会优先走这里，
// 而不是只靠 SWJ_Pins 逐次拉线。
//
// 本板没有 nRESET 引脚（见 src/swd_pin_map.h），返回 0 = 「未执行厂商复位」，
// 主机随即退回 SWD 的 SYSRESETREQ，功能上仍能复位目标。
__STATIC_INLINE uint8_t RESET_TARGET(void) {
#if SWD_NRESET_ENABLE
    PIN_nRESET_OUT(0U);
    Delayms(20U);
    PIN_nRESET_OUT(1U);
    return 1U;
#else
    return 0U;
#endif
}
