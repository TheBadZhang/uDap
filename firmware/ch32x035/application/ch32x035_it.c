/********************************** (C) COPYRIGHT *******************************
 * File Name          : ch32x035_it.c
 * Author             : WCH
 * Version            : V1.0.0
 * Date               : 2024/10/28
 * Description        : Main Interrupt Service Routines.
*********************************************************************************
* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for 
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/
#include "ch32x035_it.h"

#include "boot_info.h"

void NMI_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void HardFault_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));

/*********************************************************************
 * @fn      NMI_Handler
 *
 * @brief   This function handles NMI exception.
 *
 * @return  none
 */
void NMI_Handler(void)
{
    while (1)
  {
  }
}

/*********************************************************************
 * @fn      HardFault_Handler
 *
 * @brief   This function handles Hard Fault exception.
 *
 * @return  none
 */
void HardFault_Handler(void)
{
  // 复位前把「崩在哪」写进 RAM 交接区（0x20000000 起，软复位不清 SRAM）：
  // bootloader 下次启动会把它打成 `[boot] 上次应用崩溃: mepc=0x... mcause=N`，
  // 配合固件 ELF 就能直接定位到函数/行。没有 bootloader 的布局下这是空操作。
  boot_info_panic(__get_MEPC(), __get_MCAUSE());
  NVIC_SystemReset();
  while (1)
  {
  }
}


