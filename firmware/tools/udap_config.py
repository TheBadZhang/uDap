#!/usr/bin/env python3
"""uDap 私有 vendor command 的上位机工具（CMSIS-DAP 接口，裸批量端点）。

协议见固件里的 application/dap/udap_vendor.h：

    请求 [0x80, 子命令, 参数...]      应答 [0x80, 状态, 数据...]

用法（先接上 uDap 的 USB，不需要装驱动，WinUSB 由固件的 MS OS 2.0 描述符自动匹配）：

    python udap_config.py info                # 读固件版本 / 三个模式 / 能力位
    python udap_config.py uid                 # 读芯片 UID（16 位十六进制）
    python udap_config.py uart flow           # 串口 1 + DTR/RTS
    python udap_config.py uart dual           # 双串口（会重新枚举 USB）
    python udap_config.py led 0               # LED 模式 0..3（见下）
    python udap_config.py debugger dap        # CMSIS-DAP
    python udap_config.py debugger wchlink    # WCH-Link（需要 -Wchlink y 编入）
    python udap_config.py bootloader          # 进 UF2 bootloader（之后拖 firmware.uf2）
    python udap_config.py reset               # 软复位

LED 模式：
    0 = PB11 DAP 连接指示 / PB12 双串口指示（默认）
    1 = PB11 COM1 活动 / PB12 COM2 活动
    2 = PB11 DAP 连接指示 / PB12 COM1 活动
    3 = 两颗都灭

注意：uart / debugger / bootloader / reset 会让设备重新枚举或重启，命令的应答
仍会先发出（固件把动作延后约 30 ms 执行），随后设备从总线上消失，属正常现象。

双模式固件（CMSIS-DAP + WCH-Link，`-Wchlink y`）串口固定为单串口，`uart` 子命令
会返回「本固件不支持」；那个构建里双击 PB3 就是切调试器模式（不用命令行）。
"""

import argparse
import sys

import libusb_package
import usb.backend.libusb1
import usb.core
import usb.util

# CMSIS-DAP 模式（应用固件）
VID, PID = 0x0D28, 0x0204
# UF2 bootloader 模式（拖拽升级用的 U 盘）
BOOT_VID, BOOT_PID = 0x1209, 0x7544

VENDOR_CMD = 0x80  # ID_DAP_Vendor0

SUB_GET_INFO = 0x00
SUB_SET_UART_MODE = 0x01
SUB_SET_LED_MODE = 0x02
SUB_ENTER_UF2 = 0x03
SUB_SET_DEBUGGER_MODE = 0x04
SUB_RESET = 0x05
SUB_GET_UID = 0x06

STATUS_TEXT = {
    0x00: "成功",
    0x01: "参数越界",
    0x02: "本固件不支持",
    0x03: "忙（已有动作排队）",
    0x04: "选项字擦写失败（设置没存下来）",
}

UART_MODES = {
    "flow": 0,
    "dual": 1,
}
UART_NAMES = {0: "串口1 + DTR/RTS 流控", 1: "双串口"}

LED_MODES = {
    "status": 0,
    "activity": 1,
    "mixed": 2,
    "off": 3,
}
LED_NAMES = {
    0: "DAP 连接 / 双串口",
    1: "COM1 活动 / COM2 活动",
    2: "DAP 连接 / COM1 活动",
    3: "全灭",
}

DEBUGGER_MODES = {
    "dap": 0,
    "wchlink": 1,
}
DEBUGGER_NAMES = {0: "CMSIS-DAP", 1: "WCH-Link"}

CAP_BITS = [
    (0, "双串口"),
    (1, "WCH-Link"),
    (2, "UF2 bootloader"),
    (3, "LED 模式"),
]


def backend():
    return usb.backend.libusb1.get_backend(
        find_library=lambda name: libusb_package.find_library(name)
    )


def open_cmsis_dap():
    """找到 CMSIS-DAP 设备的接口 0，返回 (dev, ep_out, ep_in)。"""
    dev = usb.core.find(idVendor=VID, idProduct=PID, backend=backend())
    if dev is None:
        if usb.core.find(idVendor=BOOT_VID, idProduct=BOOT_PID, backend=backend()) is not None:
            raise SystemExit(
                "只找到 UF2 bootloader（%04X:%04X）。它是个 U 盘：把 firmware.uf2 "
                "拖进去即可升级；要回到应用固件，不按键拔插一次即可。"
                % (BOOT_VID, BOOT_PID)
            )
        raise SystemExit(
            "找不到 uDap（%04X:%04X）。确认已插上、且运行的是应用固件而不是 bootloader。"
            % (VID, PID)
        )

    cfg = dev.get_active_configuration()
    intf = cfg[(0, 0)]
    ep_out = ep_in = None
    for ep in intf:
        if usb.util.endpoint_direction(ep.bEndpointAddress) == usb.util.ENDPOINT_OUT:
            ep_out = ep
        else:
            ep_in = ep
    if ep_out is None or ep_in is None:
        raise SystemExit("接口 0 上没有双向批量端点，不像是 CMSIS-DAP 接口")

    usb.util.claim_interface(dev, intf.bInterfaceNumber)
    return dev, ep_out, ep_in


def vendor_cmd(ep_out, ep_in, sub, payload=b"", timeout=1500, expect_reply=True):
    """发一条 vendor command，返回 (status, data)。"""
    ep_out.write(bytes([VENDOR_CMD, sub]) + payload, timeout=timeout)
    if not expect_reply:
        return None, b""

    try:
        resp = bytes(ep_in.read(64, timeout=timeout))
    except usb.core.USBTimeoutError:
        raise SystemExit("设备没有应答（可能已经开始重新枚举，拔插一次再看）")

    if len(resp) < 2 or resp[0] != VENDOR_CMD:
        raise SystemExit("应答格式不对: %s" % resp.hex(" "))
    return resp[1], resp[2:]


def check_status(status, data=b""):
    text = STATUS_TEXT.get(status, "未知状态 0x%02X" % status)
    if status != 0:
        raise SystemExit("命令失败：%s" % text)
    return data


def cmd_info(dev, ep_out, ep_in):
    data = check_status(*vendor_cmd(ep_out, ep_in, SUB_GET_INFO))
    if len(data) < 8:
        raise SystemExit("GET_INFO 应答太短: %s" % data.hex(" "))

    proto, major, minor, patch, uart, led, dbg, caps = data[:8]
    print("设备        : %s" % usb.util.get_string(dev, dev.iProduct))
    print("序列号      : %s" % usb.util.get_string(dev, dev.iSerialNumber))
    print("协议/固件   : v%d / %d.%d.%d" % (proto, major, minor, patch))
    print("串口模式    : %d = %s" % (uart, UART_NAMES.get(uart, "?")))
    print("LED 模式    : %d = %s" % (led, LED_NAMES.get(led, "?")))
    print("调试器模式  : %d = %s" % (dbg, DEBUGGER_NAMES.get(dbg, "?")))
    names = [name for bit, name in CAP_BITS if caps & (1 << bit)]
    print("能力        : 0x%02X (%s)" % (caps, "、".join(names) if names else "无"))


def cmd_uid(ep_out, ep_in):
    data = check_status(*vendor_cmd(ep_out, ep_in, SUB_GET_UID))
    print(data[:16].decode("ascii", "replace"))


def cmd_uart(ep_out, ep_in, mode):
    value = UART_MODES[mode]
    status, data = vendor_cmd(ep_out, ep_in, SUB_SET_UART_MODE, bytes([value]))
    check_status(status, data)
    print("串口模式已切到：%s（USB 会重新枚举，主机侧看到一次插拔）" % UART_NAMES[value])


def cmd_led(ep_out, ep_in, mode):
    value = int(mode)
    status, data = vendor_cmd(ep_out, ep_in, SUB_SET_LED_MODE, bytes([value]))
    check_status(status, data)
    print("LED 模式已切到：%d = %s" % (value, LED_NAMES[value]))


def cmd_debugger(ep_out, ep_in, mode):
    value = DEBUGGER_MODES[mode]
    status, data = vendor_cmd(ep_out, ep_in, SUB_SET_DEBUGGER_MODE, bytes([value]))
    if status == 0x02:
        raise SystemExit("这个固件没有编入 WCH-Link（用 -Wchlink y 重新编译）")
    check_status(status, data)
    print("调试器模式已切到：%s（设备会复位后重新枚举）" % DEBUGGER_NAMES[value])


def cmd_bootloader(ep_out, ep_in):
    status, data = vendor_cmd(ep_out, ep_in, SUB_ENTER_UF2)
    check_status(status, data)
    print("已请求进入 UF2 bootloader：稍等一会儿主机会出现一个 U 盘")
    print("把 build\\release\\firmware.uf2 拖进去即可升级。")


def cmd_reset(ep_out, ep_in):
    status, data = vendor_cmd(ep_out, ep_in, SUB_RESET)
    check_status(status, data)
    print("已请求软复位")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="uDap 私有 vendor command 工具",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    sub = parser.add_subparsers(dest="cmd", required=True)

    sub.add_parser("info", help="读固件版本 / 当前模式 / 能力位")
    sub.add_parser("uid", help="读芯片 UID")

    p = sub.add_parser("uart", help="切换串口模式")
    p.add_argument("mode", choices=sorted(UART_MODES))

    p = sub.add_parser("led", help="切换 LED 模式")
    p.add_argument("mode", type=int, choices=sorted(LED_MODES.values()))

    p = sub.add_parser("debugger", help="切换调试器模式")
    p.add_argument("mode", choices=sorted(DEBUGGER_MODES))

    sub.add_parser("bootloader", help="进入 UF2 bootloader")
    sub.add_parser("reset", help="软复位")

    args = parser.parse_args()

    _dev, ep_out, ep_in = open_cmsis_dap()

    if args.cmd == "info":
        cmd_info(_dev, ep_out, ep_in)
    elif args.cmd == "uid":
        cmd_uid(ep_out, ep_in)
    elif args.cmd == "uart":
        cmd_uart(ep_out, ep_in, args.mode)
    elif args.cmd == "led":
        cmd_led(ep_out, ep_in, args.mode)
    elif args.cmd == "debugger":
        cmd_debugger(ep_out, ep_in, args.mode)
    elif args.cmd == "bootloader":
        cmd_bootloader(ep_out, ep_in)
    elif args.cmd == "reset":
        cmd_reset(ep_out, ep_in)

    return 0


if __name__ == "__main__":
    sys.exit(main())
