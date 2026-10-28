/*
 * 带 UF2 bootloader 的单模式应用链接脚本。
 *
 *   0x0000..0x4FFF   bootloader（见 bootloader/src/boot_config.h）
 *   0x5000..0xF6FF   应用（本脚本：62 KiB - 256 B NV - 20 KiB）
 *   0xF700..0xF7FF   预留给 NV 配置页
 *
 * RAM 起始的 16 字节（0x20000000..0x2000000F）留给应用与 bootloader 之间的
 * 启动握手区（见 src/boot_info.h），所以 RAM 区域的 ORIGIN 后移 16 字节。
 * 不预留的话 .highcode 会直接盖住它。
 *
 * 双模式（CMSIS-DAP + WCH-Link）约 56 KiB，放不进 0x5000 起的应用区，
 * 那种构建用的是 Link_highcode_nv_256B_dual.ld（应用从 0x0 起，不带 bootloader）。
 */
ENTRY(_start)

__stack_size = 1024;

PROVIDE(_stack_size = __stack_size);

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00005000, LENGTH = 62K - 256 - 20K
    RAM   (xrw) : ORIGIN = 0x20000010, LENGTH = 20K - 16
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
        *(.highcode);
        *(.highcode.*);
        *(.text.DAP_ProcessCommand);
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
    } > RAM AT > FLASH

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
