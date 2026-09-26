#!/usr/bin/env python3
"""直接对 CMSIS-DAP 接口发原始命令，绕过 OpenOCD / pyocd 的枚举逻辑。

用途：当 pyocd 报「No available debug probes」或 OpenOCD 报
「unable to find a matching CMSIS-DAP device」时，用它区分：

  * 有正常应答 -> 设备侧正常，是主机枚举/驱动那一层的问题
  * 无应答 / 应答错位 -> 固件的 DAP 接口有问题

CMSIS-DAP v2 用裸批量端点（无 HID 报告 ID）：
    请求: [cmd, ...]
    应答: [cmd, len_lo, len_hi, payload...]

DAP_Info 命令码 0x00，请求 [0x00, info_id]，应答 [0x00, info_id, len, data...]
"""

import sys
import time

import libusb_package
import usb.backend.libusb1
import usb.core
import usb.util

VID, PID = 0x0D28, 0x0204

INFO_ITEMS = [
    (0x01, "Vendor"),
    (0x02, "Product"),
    (0x03, "Serial"),
    (0x04, "Packet Count"),
    (0x05, "Packet Size"),
    (0x06, "FW Version"),
    (0xF0, "Capabilities"),
    (0xFE, "Timer"),
]

QUIET = 0.25  # 连续这么久没有新数据就认为一个包收完了


def drain(ep_in, quiet: float = QUIET) -> bytes:
    """排空 IN 端点，读到连续静默为止。"""
    out = bytearray()
    last = time.time()
    while time.time() - last < quiet:
        try:
            chunk = bytes(ep_in.read(64, timeout=50))
        except usb.core.USBTimeoutError:
            continue
        except usb.core.USBError:
            break
        if chunk:
            out += chunk
            last = time.time()
    return bytes(out)


def as_text(data: bytes) -> str:
    try:
        text = data.decode("ascii")
    except UnicodeDecodeError:
        return ""
    if all(ch == "\x00" or 31 < ord(ch) < 127 for ch in text):
        return repr(text)
    return ""


def main() -> int:
    backend = usb.backend.libusb1.get_backend(
        find_library=lambda name: libusb_package.find_library(name)
    )
    dev = usb.core.find(idVendor=VID, idProduct=PID, backend=backend)
    if dev is None:
        print(f"libusb 找不到 {VID:04X}:{PID:04X}")
        return 1

    print(f"设备 bus={dev.bus} addr={dev.address}")
    print(f"  iProduct = {usb.util.get_string(dev, dev.iProduct)!r}")
    print(f"  iManufacturer = {usb.util.get_string(dev, dev.iManufacturer)!r}")
    print(f"  iSerial = {usb.util.get_string(dev, dev.iSerialNumber)!r}")

    cfg = dev.get_active_configuration()
    intf = cfg[(0, 0)]
    print(f"  接口0: class=0x{intf.bInterfaceClass:02X} 端点数={intf.bNumEndpoints}")

    ep_out = ep_in = None
    for ep in intf:
        if usb.util.endpoint_direction(ep.bEndpointAddress) == usb.util.ENDPOINT_OUT:
            ep_out = ep
        else:
            ep_in = ep
    if ep_out is None or ep_in is None:
        print("接口0 上没有双向端点，不可能是 CMSIS-DAP 接口")
        return 1
    print(f"  OUT=0x{ep_out.bEndpointAddress:02X} IN=0x{ep_in.bEndpointAddress:02X}")

    usb.util.claim_interface(dev, 0)
    try:
        stale = drain(ep_in)
        if stale:
            print(f"\n[排空] 丢弃 {len(stale)} 字节残留: {stale.hex(' ')}")

        print("\n=== DAP_Info 逐个测试（每次先排空再发）===")
        framed = 0
        for info_id, name in INFO_ITEMS:
            drain(ep_in)
            try:
                ep_out.write(bytes([0x00, info_id]), timeout=1000)
            except usb.core.USBError as exc:
                print(f"  {name:14s} 写入失败: {exc}")
                continue
            resp = drain(ep_in)
            if not resp:
                print(f"  {name:14s} 无应答（超时）")
                continue
            good = (
                len(resp) >= 3
                and resp[0] == 0x00
                and resp[1] == info_id
                and resp[2] == len(resp) - 3
            )
            framed += 1 if good else 0
            print(f"  {name:14s} rx[{len(resp):2d}] {resp.hex(' '):<48} "
                  f"{'OK ' if good else 'BAD'} {as_text(resp[3:])}")

        print(f"\n分帧正确的 DAP_Info: {framed}/{len(INFO_ITEMS)}")

        print("\n=== 关键命令探测（OpenOCD 开设备时要用）===")
        for payload, name in [
            (bytes([0x02, 0x01]), "DAP_Connect(SWD)"),
            (bytes([0x04, 0x00, 0x00, 0x00, 0x00]), "DAP_TransferConfigure"),
            (bytes([0x03]), "DAP_Disconnect"),
        ]:
            drain(ep_in)
            try:
                ep_out.write(payload, timeout=1000)
            except usb.core.USBError as exc:
                print(f"  {name:26s} 写入失败: {exc}")
                continue
            resp = drain(ep_in)
            if not resp:
                print(f"  {name:26s} 无应答（超时）")
            else:
                print(f"  {name:26s} rx[{len(resp):2d}] {resp.hex(' ')}")
    finally:
        usb.util.release_interface(dev, 0)

    return 0


if __name__ == "__main__":
    sys.exit(main())
