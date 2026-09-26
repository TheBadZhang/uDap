#!/usr/bin/env python3
"""CDC 串口（COM 口）环回吞吐 / 利用率测试。

前提：DAP 的 USART4 引脚 PB0(TX) 与 PB1(RX) 已短接（环回跳线）。此时主机写进
COM 口的字节会经 USB -> DAP -> UART 发出、再被 UART 收回 -> USB 送回主机。

这样一次测试同时经过两个瓶颈：
    PC --USB FS bulk--> DAP --UART(波特率)--> 环回 --UART--> DAP --USB FS bulk--> PC
串口波特率低时瓶颈在 UART；波特率很高时瓶颈转到 USB 全速批量端点。

用法：
    python tools/com_loopback_speed.py                      # 默认扫一串波特率
    python tools/com_loopback_speed.py --port COM21
    python tools/com_loopback_speed.py --baud 115200,2000000
    python tools/com_loopback_speed.py --seconds 5          # 每档跑 5 秒
    python tools/com_loopback_speed.py --csv result.csv

注意：
    - 固件对 SET_LINE_CODING 有 2 Mbaud 上限（见 dap_main.c 的 CDC_MAX_BAUDRATE），
      超过会被 STALL，主机侧表现为打开/设置串口失败。脚本会把这种波特率标为「拒绝」。
    - 载荷大小默认按波特率自动缩放（见 --seconds），低波特率下不会跑到天荒地老。
    - 收发必须并发：固件 TX FIFO 只有 1 KiB，边写边读才不会撑爆，也才不会
      在被背压挡住时空等。
"""

import argparse
import statistics
import sys
import threading
import time

import serial
import serial.tools.list_ports as list_ports

# 本板 DAP 的 USB 标识
DAP_VID = 0x0D28
DAP_PID = 0x0204

# USB 全速（12 Mbps）批量端点的载荷上限：每 1 ms 帧最多 19 个 64 字节包。
USB_FS_BULK_BYTES_PER_S = 19 * 64 * 1000  # 1 216 000 B/s

# 8N1 每字节占 10 个位时间（1 起始 + 8 数据 + 1 停止）
BITS_PER_BYTE = 10.0

DEFAULT_BAUDS = [9600, 115200, 921600, 1000000, 1500000, 2000000]


def find_dap_port() -> str | None:
    """按 VID/PID 定位 DAP 的 CDC 口。"""
    for p in list_ports.comports():
        if p.vid == DAP_VID and p.pid == DAP_PID:
            return p.device
    return None


def payload_bytes(size: int, seed: int = 0x1234) -> bytes:
    """构造伪随机载荷：用于检出丢字节/错序/错位，不是全 0 也不算重复模式。"""
    data = bytearray(size)
    value = seed
    for i in range(size):
        value ^= (value << 13) & 0xFFFFFFFF
        value ^= value >> 17
        value ^= (value << 5) & 0xFFFFFFFF
        data[i] = value & 0xFF
    return bytes(data)


def auto_size(baud: int, seconds: float) -> int:
    """按波特率把一个「跑 seconds 秒」的载荷大小换算出来（取 4 KiB 的整数倍）。"""
    raw = int(baud / BITS_PER_BYTE * seconds)
    size = max(4096, raw)
    size = (size + 4095) // 4096 * 4096
    return size


class Transfer:
    """一次环回：写线程发全量载荷，读线程并发收全量。"""

    def __init__(self, ser: serial.Serial, data: bytes, timeout: float):
        self.ser = ser
        self.data = data
        self.timeout = timeout

        self.written = 0
        self.received = bytearray()
        self.write_error: Exception | None = None
        self.read_error: Exception | None = None
        self.t_start = 0.0
        self.t_write_done = 0.0
        self.t_end = 0.0

    def _writer(self):
        try:
            self.t_start = time.perf_counter()
            self.ser.write(self.data)
            self.written = len(self.data)
            self.t_write_done = time.perf_counter()
        except Exception as exc:  # noqa: BLE001 - 如实上报给调用者
            self.write_error = exc
            self.t_write_done = time.perf_counter()

    def _reader(self):
        want = len(self.data)
        deadline = time.perf_counter() + self.timeout
        try:
            while len(self.received) < want:
                if time.perf_counter() > deadline:
                    self.read_error = TimeoutError(
                        f"超时：只收到 {len(self.received)}/{want} 字节"
                    )
                    break
                chunk = self.ser.read(min(65536, want - len(self.received)))
                if chunk:
                    self.received += chunk
                    deadline = time.perf_counter() + self.timeout
            self.t_end = time.perf_counter()
        except Exception as exc:  # noqa: BLE001
            self.read_error = exc
            self.t_end = time.perf_counter()

    def run(self):
        rt = threading.Thread(target=self._reader, daemon=True)
        wt = threading.Thread(target=self._writer, daemon=True)
        rt.start()
        # 先让读线程挂上去，再开写，避免开头几个字节没人收
        time.sleep(0.01)
        wt.start()
        wt.join()
        rt.join(timeout=self.timeout)
        return self

    @property
    def elapsed(self) -> float:
        if self.t_start == 0.0:
            return 0.0
        return self.t_end - self.t_start

    @property
    def ok(self) -> bool:
        return (
            self.write_error is None
            and self.read_error is None
            and self.written == len(self.data)
            and bytes(self.received) == self.data
        )


def measure(port: str, baud: int, size: int, timeout: float) -> dict:
    """在指定波特率下做一次环回，返回统计字典。"""
    result = {
        "baud": baud,
        "size": size,
        "ok": False,
        "note": "",
        "seconds": None,
        "actual": None,
        "line": baud / BITS_PER_BYTE,
        "util": None,
        "usb_util": None,
        "bytes_out": 0,
        "bytes_in": 0,
    }

    try:
        ser = serial.Serial(
            port,
            baudrate=baud,
            timeout=0.2,
            write_timeout=timeout,
            rtscts=False,
            dsrdtr=False,
            xonxoff=False,
        )
    except Exception as exc:  # noqa: BLE001 - 波特率被拒时 Windows 会在这里抛错
        result["note"] = f"打开失败（波特率被拒？）：{exc}"
        return result

    try:
        # 让设备的 SET_LINE_CODING 真正生效，并清掉切换瞬间的残byte
        time.sleep(0.2)
        ser.reset_input_buffer()
        ser.reset_output_buffer()
        time.sleep(0.05)

        data = payload_bytes(size)
        xfer = Transfer(ser, data, timeout).run()

        result["bytes_out"] = xfer.written
        result["bytes_in"] = len(xfer.received)

        if xfer.elapsed > 0:
            result["seconds"] = xfer.elapsed
            result["actual"] = len(xfer.received) / xfer.elapsed
            result["util"] = result["actual"] / result["line"] * 100.0
            result["usb_util"] = (
                2.0 * len(xfer.received) / xfer.elapsed / USB_FS_BULK_BYTES_PER_S * 100.0
            )

        result["ok"] = xfer.ok

        if xfer.write_error is not None:
            result["note"] = f"写失败：{xfer.write_error}"
        elif xfer.read_error is not None:
            result["note"] = str(xfer.read_error)
        elif not xfer.ok:
            got = bytes(xfer.received)
            n = min(len(got), len(data))
            bad = next((i for i in range(n) if got[i] != data[i]), n)
            result["note"] = f"数据不符（首个不同位置 {bad}，收 {len(got)}/{len(data)}）"
    finally:
        ser.close()

    return result


def fmt(value, spec: str, dash: str = "—") -> str:
    if value is None:
        return dash
    return format(value, spec)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="CDC 串口环回吞吐 / 利用率测试（需 PB0-PB1 环回跳线）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--port", help="COM 口，默认按 VID:PID 自动查找")
    parser.add_argument(
        "--baud",
        default=",".join(str(b) for b in DEFAULT_BAUDS),
        help="波特率列表，逗号分隔（默认 %(default)s）",
    )
    parser.add_argument(
        "--seconds",
        type=float,
        default=3.0,
        help="每档按此目标时长自动换算载荷大小（默认 %(default)s）",
    )
    parser.add_argument("--bytes", type=int, help="固定载荷大小（覆盖 --seconds）")
    parser.add_argument("--repeats", type=int, default=1, help="每档重复次数，取中位（默认 1）")
    parser.add_argument("--csv", help="结果写入 CSV")
    args = parser.parse_args()

    port = args.port or find_dap_port()
    if not port:
        print("找不到 DAP 的 CDC 口，请用 --port 指定", file=sys.stderr)
        return 1

    try:
        bauds = [int(b.strip()) for b in args.baud.split(",") if b.strip()]
    except ValueError:
        print("--baud 需为逗号分隔的整数", file=sys.stderr)
        return 1
    if not bauds:
        print("--baud 为空", file=sys.stderr)
        return 1

    print()
    print(f"端口    : {port}")
    print(f"每档时长: {args.seconds:g} s（载荷按波特率自动缩放）")
    print(f"重复    : {args.repeats}")
    print()

    rows = []
    for baud in bauds:
        size = args.bytes if args.bytes else auto_size(baud, args.seconds)
        timeout = max(5.0, size / (baud / BITS_PER_BYTE) * 5.0 + 3.0)

        runs = []
        print(f"  {baud:>8} baud  载荷 {size} B")
        for r in range(args.repeats):
            res = measure(port, baud, size, timeout)
            runs.append(res)
            mark = "OK  " if res["ok"] else "FAIL"
            note = f"  {res['note']}" if res["note"] else ""
            tag = f"  #{r + 1}" if args.repeats > 1 else ""
            print(
                f"      {mark} 实收 {res['bytes_in']:>7} B  "
                f"{fmt(res['seconds'], '8.3f')} s  "
                f"{fmt(res['actual'], '9.1f')} B/s  "
                f"利用率 {fmt(res['util'], '6.1f')}%{note}{tag}"
            )

        good = [r for r in runs if r["ok"] and r["actual"]]
        if not good:
            failed = dict(runs[-1])
            failed["actual"] = None
            failed["util"] = None
            failed["usb_util"] = None
            failed["seconds"] = None
            rows.append(failed)
            continue

        actuals = [r["actual"] for r in good]
        utils = [r["util"] for r in good]
        usbs = [r["usb_util"] for r in good]
        rows.append(
            {
                "baud": baud,
                "size": size,
                "actual": statistics.median(actuals),
                "util": statistics.median(utils),
                "usb_util": statistics.median(usbs),
                "seconds": statistics.median([r["seconds"] for r in good]),
                "ok": True,
                "note": "",
            }
        )

    # ---------- 汇总表 ----------
    print()
    width = 100
    print("=" * width)
    print(
        f"  {'波特率':>9} {'载荷':>8} {'耗时':>8} {'实际速度':>11} {'线速率':>11} "
        f"{'UART利用率':>10} {'USB利用率':>9} {'状态':>5}"
    )
    print("-" * width)
    for row in rows:
        line_rate = row["baud"] / BITS_PER_BYTE
        if row.get("actual") is None:
            print(
                f"  {row['baud']:>9} {row['size']:>8} {'—':>8} {'—':>11} "
                f"{line_rate:>11.0f} {'—':>10} {'—':>9} {'失败':>5}"
            )
            continue
        print(
            f"  {row['baud']:>9} {row['size']:>8} "
            f"{row['seconds']:>8.3f} "
            f"{row['actual'] / 1024:>8.1f} KiB/s "
            f"{line_rate / 1024:>8.1f} KiB/s "
            f"{row['util']:>9.1f}% {row['usb_util']:>8.1f}% {'OK':>5}"
        )
    print("=" * width)
    print()
    print("说明：")
    print("  - UART 线速率 = 波特率 / 10（8N1：1 起始 + 8 数据 + 1 停止）。")
    print("  - UART 利用率 = 实际速度 / 线速率，反映 UART 侧被榨干的程度。")
    print(f"  - USB 利用率 = 双向字节 / USB 全速批量上限（{USB_FS_BULK_BYTES_PER_S/1e6:.3f} MB/s）。")
    print("  - 波特率越高，瓶颈会从 UART 转到 USB；两列利用率谁先接近 100% 谁就是瓶颈。")
    print()

    if args.csv:
        import csv

        with open(args.csv, "w", newline="", encoding="utf-8") as fh:
            writer = csv.writer(fh)
            writer.writerow(
                ["baud", "size_bytes", "seconds", "actual_Bps", "line_rate_Bps",
                 "uart_util_pct", "usb_util_pct", "ok", "note"]
            )
            for row in rows:
                writer.writerow([
                    row["baud"], row["size"],
                    f"{row['seconds']:.6f}" if row.get("seconds") else "",
                    f"{row['actual']:.1f}" if row.get("actual") else "",
                    f"{row['baud'] / BITS_PER_BYTE:.1f}",
                    f"{row['util']:.2f}" if row.get("util") else "",
                    f"{row['usb_util']:.2f}" if row.get("usb_util") else "",
                    "1" if row.get("ok") else "0",
                    row.get("note", ""),
                ])
        print(f"已写入 {args.csv}")

    return 0 if all(r.get("ok") for r in rows) else 2


if __name__ == "__main__":
    sys.exit(main())
