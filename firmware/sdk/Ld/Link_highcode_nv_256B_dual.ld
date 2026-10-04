/*
 * 双模式固件专用链接脚本（--dap=y --wchlink=y）。
 *
 * 与 Link_highcode_nv_256B.ld 的差异只有一处：把「只有某一个模式用得到」的
 * 代码与数据放进 ld 的 OVERLAY 段，让两种模式**共用同一段 RAM 地址**。
 *
 * 依据：CMSIS-DAP 与 WCH-Link 都要占用 PA2=SWCLK/DCK、PA3=SWDIO/DIO，且 USB
 * 身份不同（0d28:0204 vs 1a86:8010），所以不可能同时运行；实测双模式全编译时
 * Flash 够（约 51 KiB / 63232 B），但 RAM 需要约 25.3 KiB 而只有 20 KiB。
 * 两者既然不同时运行，各自私有的那部分 RAM 就可以重叠。
 *
 * 段划分：
 *   .highcode            共用（USB 端口层热路径），照原样在启动时从 Flash 拷入 RAM
 *   .highcode.dap        仅 CMSIS-DAP：sw_dp.c / dap_main.c 的热路径
 *   .text.DAP_ProcessCommand  仅 CMSIS-DAP（CherryDAP 的命令分发）
 *   .overlay.dap         仅 CMSIS-DAP：USB 收发缓冲
 *   .highcode.wchlink    仅 WCH-Link：rvswd_phy_gpio.c 的位翻转
 *   .overlay.wchlink     仅 WCH-Link：会话/传输状态与大缓冲
 *
 * OVERLAY 把 dap_overlay 与 wchlink_overlay 放在同一段 VMA（取两者较大的），
 * 并为每段生成 __load_start_<name> / __load_stop_<name> 作为 Flash 侧 LMA。
 * 由于启动时无法同时初始化两份内容，**由 src/main.c 在进入各自模式时**：
 *   1. 把该模式的代码段从 __load_start_ 拷到 _xxx_overlay_code_start；
 *   2. 把该模式的 BSS（_xxx_overlay_bss_start.._end）清零。
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
    /* Reserve last 256B for NV settings */
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
        /* 只收共用的 .highcode；.highcode.dap / .highcode.wchlink 走下面的 OVERLAY */
        *(.highcode);
        . = ALIGN(4);
        PROVIDE(_highcode_vma_end = .);
    } > RAM AT > FLASH

    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text.*)
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
     * ===== 双模式 overlay：两种模式各自的私有代码与数据共用这段地址 =====
     * LMA 紧跟在 .data 的 LMA 之后；两段取较大的作为窗口大小。
     */
    __dual_overlay_lma = LOADADDR(.data) + SIZEOF(.data);

    OVERLAY : NOCROSSREFS AT (__dual_overlay_lma)
    {
        dap_overlay
        {
            . = ALIGN(4);
            _dap_overlay_code_start = .;
            *(.highcode.dap)
            *(.text.DAP_ProcessCommand)
            . = ALIGN(4);
            _dap_overlay_code_end = .;

            _dap_overlay_bss_start = .;
            *(.overlay.dap)
            . = ALIGN(4);
            _dap_overlay_bss_end = .;
        }

        wchlink_overlay
        {
            . = ALIGN(4);
            _wchlink_overlay_code_start = .;
            *(.highcode.wchlink)
            . = ALIGN(4);
            _wchlink_overlay_code_end = .;

            _wchlink_overlay_bss_start = .;
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
}
