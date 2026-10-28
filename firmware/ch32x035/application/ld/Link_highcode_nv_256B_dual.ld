/*
 * 本工程唯一的链接脚本（双模式固件专用）。
 *
 * 把「只有某一个模式用得到」的代码与数据放进 ld 的 OVERLAY 段，让两种模式
 * **共用同一段 RAM 地址**。
 *
 * 依据：CMSIS-DAP 与 WCH-Link 都要占用 PA2=SWCLK/DCK、PA3=SWDIO/DIO，且 USB
 * 身份不同（0d28:0204 vs 1a86:8010），所以不可能同时运行；实测全编译时
 * Flash 够（约 51 KiB / 63232 B），但 RAM 需要约 25.3 KiB 而只有 20 KiB。
 * 两者既然不同时运行，各自私有的那部分 RAM 就可以重叠。
 *
 * 段划分（重构后：**只有数据**还参与 overlay）：
 *   .highcode            共用（USB 端口层热路径）+ CMSIS-DAP 私有热路径
 *                        （.highcode.dap），启动时一次性从 Flash 拷入 RAM
 *   .highcode.dap        仅 CMSIS-DAP：sw_dp.c / dap_main.c 的热路径（并进 .highcode）
 *   .text.DAP_ProcessCommand  仅 CMSIS-DAP（CherryDAP 的命令分发）—— **留在 Flash**：
 *                        收进 RAM 会多占 2.9 KiB 并让 RAM 溢出（见 .highcode 处说明）
 *   .overlay.dap         仅 CMSIS-DAP：USB 收发缓冲（RAM，overlay）
 *   .highcode.wchlink    仅 WCH-Link：rvswd_phy_gpio.c 的位翻转
 *                        **链接进 .text，直接在 Flash 上执行（XIP）**
 *   .overlay.wchlink     仅 WCH-Link：会话/传输状态与大缓冲（RAM，overlay）
 *
 * 为什么只有数据留在 overlay：
 *   1. 数据不能放 Flash（要写），而两模式的数据互斥 —— 重叠才省 RAM。实测
 *      WCH-Link 数据 6.0 KiB vs CMSIS-DAP 数据 1.6 KiB，窗口取较大的 6.0 KiB。
 *   2. 代码不需要复制：DAP 热路径（2.4 KiB）并进常驻 RAM 的 .highcode，由
 *      startup_ch32x035_highcode.S 在**启动时**拷一次；WCH-Link 热路径
 *      （2.0 KiB）改为在 Flash 上执行。代价是 WCH-Link 每次取指多 2 个等待
 *      周期（48 MHz 下 FLASH_ACTLR_LATENCY_2），换来省掉运行期代码拷贝。
 *
 * 于是运行期只剩**清零数据段**（没有代码拷贝）：由 src/main.c 在进入某模式时执行
 *   _xxx_overlay_bss_start.._xxx_overlay_bss_end = 0
 * OVERLAY 取两者较大的作为窗口。两段里只放**零初始化数据**，所以代码把它们
 * 放在 `.bss.overlay.*` 名下 —— 那是 SHT_NOBITS 段（同 .bss），**不产生 Flash
 * 内容**，objcopy 也不会把它算进 .bin。因此这里不给 AT(...)：
 *   - 段是 NOBITS 时 LMA 无所谓，objcopy 只按 PROGBITS 段生成字节；
 *   - 反过来，若哪天有人往 overlay 里放带初值的变量，段就变成 PROGBITS，
 *     LMA=VMA 会落在 RAM 地址上把 .bin 撑爆 —— 那种错误很容易被发现，
 *     比「悄悄多占 8.2 KiB Flash」更好。
 *
 * 注意 .bss 在本脚本里去掉了 `AT > FLASH`：原来那会让 NOBITS 的 .bss 在
 * Flash 地址空间里白占一份等长区间（实测 0x31ac ≈ 12.7 KiB），而 overlay 的
 * LMA 需要跟在 .data 之后，会被它挤到 Flash 区域之外。.bss 不需要 LMA。
 */

ENTRY(_start)

__stack_size = 1024;

PROVIDE(_stack_size = __stack_size);

MEMORY
{
    /* 双模式固件约 56 KiB，62 KiB Flash 里放不下「UF2 bootloader + 它」
     * （bootloader 20 KiB），所以双模式**不支持 UF2 拖拽升级**，只能继续用
     * SWD 烧录，布局保持应用独占 0x0..0xF6FF。见 xmake.lua 的 bootloader 选项。 */
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 62K - 256
    RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 20K
}

SECTIONS
{
    .init :
    {
        _sinit = .;
        . = ALIGN(4);
        KEEP(*(SORT_NONE(.init)))
        . = ALIGN(4);
        _einit = .;
    } > FLASH AT > FLASH

    .vector :
    {
        *(.vector);
        . = ALIGN(64);
    } > FLASH AT > FLASH

    .highcodelalign :
    {
        . = ALIGN(4);
        PROVIDE(_highcode_lma = .);
    } > FLASH AT > FLASH

    .highcode :
    {
        . = ALIGN(4);
        PROVIDE(_highcode_vma_start = .);
        /* 常驻 RAM 的热路径，启动时由 startup_ch32x035_highcode.S 一次性拷入：
         *   .highcode       共用（USB 端口层）
         *   .highcode.dap   仅 CMSIS-DAP（sw_dp.c / dap_main.c）
         * 两者不再参与运行期 overlay，所以切进 DAP 模式时无需任何代码拷贝。
         *
         * 注意 .text.DAP_ProcessCommand（CherryDAP 的命令分发，约 2.9 KiB）**不**收进来：
         * 它由下面 .text 的 *(.text.*) 收走，留在 Flash。收进 RAM 会多占 2.9 KiB，
         * 实测 RAM 会到 20.50 KiB（> 20 KiB 上限）且 .bss 会压到 .stack 上。 */
        *(.highcode);
        *(.highcode.dap)
        . = ALIGN(4);
        PROVIDE(_highcode_vma_end = .);
    } > RAM AT > FLASH

    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text.*)
        /* WCH-Link 私有热路径：直接在 Flash 上执行（XIP，2 个等待周期）。
         * 放在这里 = 不占 RAM、也不需要运行期拷贝；代价见文件头的说明。 */
        *(.highcode.wchlink)
        *(.rodata)
        *(.rodata*)
        *(.gnu.linkonce.t.*)
        . = ALIGN(4);
    } > FLASH AT > FLASH

    .fini :
    {
        KEEP(*(SORT_NONE(.fini)))
        . = ALIGN(4);
    } > FLASH AT > FLASH

    PROVIDE(_etext = .);
    PROVIDE(_eitcm = .);

    .preinit_array :
    {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    } > FLASH AT > FLASH

    .init_array :
    {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o) .ctors))
        PROVIDE_HIDDEN(__init_array_end = .);
    } > FLASH AT > FLASH

    .fini_array :
    {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP(*(.fini_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o) .dtors))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } > FLASH AT > FLASH

    .ctors :
    {
        /* gcc uses crtbegin.o to find the start of the constructors */
        KEEP(*crtbegin.o(.ctors))
        KEEP(*crtbegin?.o(.ctors))
        KEEP(*(EXCLUDE_FILE (*crtend.o *crtend?.o) .ctors))
        KEEP(*(SORT(.ctors.*)))
        KEEP(*(.ctors))
    } > FLASH AT > FLASH

    .dtors :
    {
        KEEP(*crtbegin.o(.dtors))
        KEEP(*crtbegin?.o(.dtors))
        KEEP(*(EXCLUDE_FILE (*crtend.o *crtend?.o) .dtors))
        KEEP(*(SORT(.dtors.*)))
        KEEP(*(.dtors))
    } > FLASH AT > FLASH

    .dalign :
    {
        . = ALIGN(4);
        PROVIDE(_data_vma = .);
    } > RAM AT > FLASH

    .dlalign :
    {
        . = ALIGN(4);
        PROVIDE(_data_lma = .);
    } > FLASH AT > FLASH

    .data :
    {
        *(.gnu.linkonce.r.*)
        *(.data .data.*)
        *(.gnu.linkonce.d.*)
        . = ALIGN(8);
        PROVIDE(__global_pointer$ = . + 0x800);
        *(.sdata .sdata.*)
        *(.sdata2.*)
        *(.gnu.linkonce.s.*)
        . = ALIGN(8);
        *(.srodata.cst16)
        *(.srodata.cst8)
        *(.srodata.cst4)
        *(.srodata.cst2)
        *(.srodata .srodata.*)
        . = ALIGN(4);
        PROVIDE(_edata = .);
    } > RAM AT > FLASH

    /*
     * ===== 双模式 overlay：两种模式各自的私有**数据**共用这段地址 =====
     * 代码已分别并进 .highcode（RAM，启动时拷入）与 .text（Flash XIP），
     * 这里只剩数据；LMA 紧跟在 .data 的 LMA 之后，两段取较大的作为窗口大小。
     */
    OVERLAY : NOCROSSREFS
    {
        dap_overlay
        {
            . = ALIGN(4);
            _dap_overlay_bss_start = .;
            *(.bss.overlay.dap)
            *(.overlay.dap)
            . = ALIGN(4);
            _dap_overlay_bss_end = .;
        }

        wchlink_overlay
        {
            . = ALIGN(4);
            _wchlink_overlay_bss_start = .;
            *(.bss.overlay.wchlink)
            *(.overlay.wchlink)
            . = ALIGN(4);
            _wchlink_overlay_bss_end = .;
        }
    } > RAM

    /* .bss 不需要 Flash 侧的 LMA（原脚本的 `AT > FLASH` 会白占等长 Flash 区间） */
    .bss :
    {
        . = ALIGN(4);
        PROVIDE(_sbss = .);
        *(.sbss*)
        *(.gnu.linkonce.sb.*)
        *(.bss*)
        *(.gnu.linkonce.b.*)
        *(COMMON*)
        . = ALIGN(4);
        PROVIDE(_ebss = .);
    } > RAM

    PROVIDE(_end = _ebss);
    PROVIDE(end = .);

    .stack ORIGIN(RAM) + LENGTH(RAM) - __stack_size :
    {
        PROVIDE(_heap_end = .);
        . = ALIGN(4);
        PROVIDE(_susrstack = .);
        . = . + __stack_size;
        PROVIDE(_eusrstack = .);
    } > RAM

    /* 数据区（含 overlay 窗口）不能顶到栈：栈的 VMA 是写死的
     * ORIGIN(RAM)+LENGTH(RAM)-__stack_size，而 ld 只检查「段是否超出 RAM 区域」，
     * **不检查段与段是否重叠**。所以这里显式断言，把「改大缓冲把栈覆盖掉」
     * 变成链接错误，而不是运行期随机跑飞。 */
    ASSERT(_ebss <= _susrstack,
           ".bss/overlay overflow: data area reaches into the reserved stack")
}
