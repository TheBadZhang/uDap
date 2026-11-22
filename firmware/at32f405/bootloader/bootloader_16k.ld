/*
*****************************************************************************
**
**  File        : bootloader_16k.ld
**
**  Abstract    : uDap（AT32F405KBU7_4，128KB FLASH / 70KB RAM）bootloader
**                链接脚本：占用 FLASH 起始 16 KiB（0x08000000..0x08003FFF），
**                应用固定链接在 0x08004000。
**
**  Note        : 1) .noinit 段被钉在 RAM 绝对地址 0x20010000，与应用
**                   链接脚本里的同一个地址重合 —— 这是 g_udap_boot_magic
**                   跨软复位传递的前提，两边必须保持一致；
**                2) 链接时会检查 .bss 不能越过 0x20010000。
**
**  Target      : Artery AT32F405
**  Environment : arm-none-eabi-gcc
**
*****************************************************************************
*/

/* Entry Point */
ENTRY(Reset_Handler)

/* Highest address of the user mode stack */
_estack = 0x20011800;    /* end of RAM = 0x20000000 + 70K */

/* Generate a link error if heap and stack don't fit into RAM */
_Min_Heap_Size  = 0x200;   /* required amount of heap  */
_Min_Stack_Size = 0x400;   /* required amount of stack */

/* .noinit（跨复位魔术字）两端固件共享的固定 RAM 地址 */
_NOINIT_BASE = 0x20010000;

/* Specify the memory areas */
MEMORY
{
FLASH (rx)      : ORIGIN = 0x08000000, LENGTH = 16K
RAM (xrw)       : ORIGIN = 0x20000000, LENGTH = 70K
}

/* Define output sections */
SECTIONS
{
  /* The startup code goes first into FLASH */
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector)) /* Startup code */
    . = ALIGN(4);
  } >FLASH

  /* The program code and other data goes into FLASH */
  .text :
  {
    . = ALIGN(4);
    *(.text)           /* .text sections (code) */
    *(.text*)          /* .text* sections (code) */
    *(.glue_7)         /* glue arm to thumb code */
    *(.glue_7t)        /* glue thumb to arm code */
    *(.eh_frame)

    KEEP (*(.init))
    KEEP (*(.fini))

    . = ALIGN(4);
    _etext = .;        /* define a global symbols at end of code */
  } >FLASH

  /* Constant data goes into FLASH */
  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)         /* .rodata sections (constants, strings, etc.) */
    *(.rodata*)        /* .rodata* sections (constants, strings, etc.) */
    . = ALIGN(4);
  } >FLASH

  .ARM.extab   : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH
  .ARM : {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } >FLASH

  .preinit_array     :
  {
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
  } >FLASH
  .init_array :
  {
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
  } >FLASH
  .fini_array :
  {
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array*))
    PROVIDE_HIDDEN (__fini_array_end = .);
  } >FLASH

  /* used by the startup to initialize data */
  _sidata = LOADADDR(.data);

  /* Initialized data sections goes into RAM, load LMA copy after code */
  .data :
  {
    . = ALIGN(4);
    _sdata = .;        /* create a global symbol at data start */
    *(.data)           /* .data sections */
    *(.data*)          /* .data* sections */

    . = ALIGN(4);
    _edata = .;        /* define a global symbol at data end */
  } >RAM AT> FLASH

  /* Uninitialized data section */
  . = ALIGN(4);
  .bss :
  {
    /* This is used by the startup in order to initialize the .bss secion */
    _sbss = .;         /* define a global symbol at bss start */
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)

    . = ALIGN(4);
    _ebss = .;         /* define a global symbol at bss end */
    __bss_end__ = _ebss;
  } >RAM

  /* 复位不清零的变量（跨软复位传递状态）：钉在固定地址 0x20010000，
     * 应用链接脚本里的 .noinit 用同一个地址，两个固件才能看到同一块 RAM。
     * 启动文件只清 _sbss.._ebss，本段必须放在 .bss 之后。
     * 注意：这里必须用「输出段显式地址」的写法；在段内写 `. = _NOINIT_BASE;`
     * 会让 ld 的 RAM 占用统计错乱（GCC 15.2.1 实测）。 */
  .noinit _NOINIT_BASE (NOLOAD) :
  {
    KEEP(*(.noinit))
    KEEP(*(.noinit*))
  } >RAM

  /* User_heap_stack section, used to check that there is enough RAM left */
  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >RAM

  /* Remove information from the standard libraries */
  /DISCARD/ :
  {
    libc.a ( * )
    libm.a ( * )
    libgcc.a ( * )
  }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}

/* .bss 不能长到 .noinit 的固定地址上 */
ASSERT(_ebss <= _NOINIT_BASE, "bootloader: .bss 越过了 .noinit 固定地址 0x20010000")
ASSERT(_NOINIT_BASE + 4 <= _estack, "bootloader: RAM 布局错误（_estack 太小）")
