/*
 * 主机测试用的 CherryUSB 垫片：bootuf2.c 只用到 usbd_core.h 里的日志宏，
 * 这里给最小替代，让同一份 bootuf2.c 能在 PC 上编译、跑通 UF2 解析流程。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define USB_LOG_DBG(...) \
    do {                 \
    } while (0)
#define USB_LOG_INFO(...) \
    do {                  \
    } while (0)
#define USB_LOG_ERR(...) \
    do {                 \
    } while (0)
