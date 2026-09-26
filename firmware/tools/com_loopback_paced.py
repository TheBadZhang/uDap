#!/usr/bin/env python3
"""CDC 串口环回：高波特率下的「小载荷 / 间隔分块」发送测试。

为什么需要这个脚本
------------------
持续满载收发时，设备侧 RX 方向**没有背压手段**（UART 无 RTS/CTS），波特率一高就
会偶发溢出丢字节（见 com_loopback_speed.py 与仓库 readme 第 6.3 节）。但那是
「持续满载」的问题，不是波特率本身的问题：把发送改成「小块 + 块间留空隙」，
让 RX FIFO 每块之间都被排空，同一波特率就能做到零丢字节。

本脚本用两种发送模式验证这一点，并测出高波特率下究竟能跑到多快：

    sustained  一次性发完整块（旧行为，用于复现丢字节）
    chunked    按 --chunk 分块、块间 --gap-ms 空隙（默认模式）

它同时回答一个硬件问题：**请求的波特率 UART 能不能精确产生**。
CH32X035 的 USART 是 16 倍过采样，BRR = fPCLK / 波特率 且 BRR 必须是整数，
所以实际速率只能落在 fPCLK/BRR 这些离散值上。脚本复刻了 SDK 中 USART_Init()
的 BRR 算法来推算实际值，并标出 BRR < 16（USARTDIV < 1，超出规范）的情况。

前提：DAP 的 PB0(TX) 与 PB1(RX) 已短接（环回跳线）。

用法
----
    # 高波特率 + 分块（4 KiB/块，块间 5 ms）
    python tools/com_loopback_paced.py --baud 2000000,3000000,4000000 --chunk 4096 --gap-ms 5

    # 小载荷（64 B/块，块间 1 ms）
    python tools/com_loopback_paced.py --baud 3000000 --chunk 64 --gap-ms 1 --total 65536

    # 对照：持续满载
    python tools/com_loopback_paced.py --baud 3000000 --mode sustained --total 262144

注意
----
    - 固件现在**不再限制** COM 口波特率（原先 2 Mbaud 的上限已撤除），
      主机设什么就设什么。UART 侧能产生多少、以及会不会丢字节，由本脚本实测回答。
    - 分块模式下每块都要等收齐才发下一块，所以「整体速率」含块间空隙；
      「等效速率」扣除空隙，更接近链路真实能力。
"""

import argparse
import statistics
import sys
import time

import serial
import serial.tools.list_ports as list_ports

# 本板 DAP 的 USB 标识
DAP_VID = 0x0D28
DAP_PID = 0x0204

# USART4 挂 APB1，本工程 APB1 = SYSCLK = 48 MHz
APB_CLOCK = 48_000_000

# 8N1 每字节占 10 个位时间（1 起始 + 8 数据 + 1 停止）
BITS_PER_BYTE = 10.0

USB_FS_BULK_BYTES_PER_S = 19 * 64 * 1000  # 1 216 000 B/s

DEFAULT_BAUDS = [2000000, 2500000, 3000000, 4000000, 6000000]

# BRR 的整数部分小于 1 时（BRR < 16）USARTDIV < 1，超出 16 倍过采样规范。
# 硬件是否还按 BRR 分频工作，只有实测能回答 —— 这里只做标注。
BRR_SPEC_MIN = 16


def find_dap_port() -> str | None:
    for p in list_ports.comports():
        if p.vid == DAP_VID and p.pid == DAP_PID:
            return p.device
    return None


def payload_bytes(size: int, seed: int = 0x1234) -> bytes:
    """伪随机载荷：能检出丢字节 / 错序 / 错位。"""
    data = bytearray(size)
    value = seed
    for i in range(size):
        value ^= (value << 13) & 0xFFFFFFFF
        value ^= value >> 17
        value ^= (value << 5) & 0xFFFFFFFF
        data[i] = value & 0xFF
    return bytes(data)


def sdk_brr(baud: int, apb: int = APB_CLOCK) -> int:
    """复刻 SDK `USART_Init()` 里 BRR 的计算（全整数运算，含其取整方式）。

    SDK 的算法（ch32x035_usart.c）：
        integerdivider = 25 * apb / (4 * baud)      # 即 6.25 * apb / baud
        tmpreg         = (integerdivider / 100) << 4       # 整数部分
        frac           = integerdivider - 100 * 整数部分
        frac           = (frac * 16 + 50) / 100            # 四舍五入到 4 bit
        frac > 15 ? 整数部分 + 1 : BRR = 整数部分<<4 | frac
    """
    if baud <= 0:
        return 0
    integerdivider = (25 * apb) // (4 * baud)
    tmpreg = (integerdivider // 100) << 4
    frac = integerdivider - (100 * (tmpreg >> 4))
    frac = ((frac * 16) + 50) // 100
    if frac > 0xF:
        tmpreg += 1 << 4
    else:
        tmpreg |= frac & 0xF
    return tmpreg


def actual_baud(baud: int, apb: int = APB_CLOCK) -> float | None:
    """UART 实际产生的波特率。

    16 倍过采样下 USARTDIV = BRR/16，故实际波特率 = apb / (16 * BRR/16) = apb / BRR。
    """
    brr = sdk_brr(baud, apb)
    if brr == 0:
        return None
    return apb / brr


def brr_desc(baud: int) -> str:
    brr = sdk_brr(baud)
    if brr == 0:
        return "BRR=0 无效"
    if brr < BRR_SPEC_MIN:
        return f"BRR={brr} (<16 超规范)"
    return f"BRR={brr}"


class PacedResult:
    def __init__(self, total: int):
        self.total = total
        self.out = 0
        self.received = bytearray()
        self.error: str | None = None
        self.elapsed = 0.0
        self.gap_total = 0.0
        self.chunks = 0

    @property
    def transfer_time(self) -> float:
        """扣除块间空隙的纯传输时间。"""
        return max(self.elapsed - self.gap_total, 0.0)

    def check(self, expected: bytes) -> tuple[bool, int]:
        got = bytes(self.received)
        if self.error:
            return False, -1
        if got == expected[: len(got)] and len(got) == len(expected):
            return True, -1
        n = min(len(got), len(expected))
        bad = next((i for i in range(n) if got[i] != expected[i]), n)
        return False, bad


def run_chunked(ser: serial.Serial, data: bytes, chunk: int, gap_s: float, timeout: float) -> PacedResult:
    """分块发送：发一块 -> 收齐这一块 -> 可选空隙 -> 下一块。

    每块都必须收齐才继续，因此 RX FIFO 在块与块之间必定被排空 —— 这正是
    「间隔分包」能消掉溢出丢字节的原因。
    """
    res = PacedResult(len(data))
    want_total = len(data)
    t_start = time.perf_counter()

    while res.out < want_total:
        blk = data[res.out : res.out + len_chunk(res.out, want_total, chunk)]
        try:
            wrote = ser.write(blk)
        except Exception as exc:  # noqa: BLE001
            res.error = f"写失败：{exc}"
            break
        res.out += len(blk)
        if wrote is None:
            wrote = len(blk)

        want = res.out
        deadline = time.perf_counter() + timeout
        while len(res.received) < want:
            if time.perf_counter() > deadline:
                res.error = f"超时：只收到 {len(res.received)}/{want} 字节"
                break
            got = ser.read(min(65536, want - len(res.received)))
            if got:
                res.received += got
                deadline = time.perf_counter() + timeout
        if res.error:
            break

        res.chunks += 1
        if res.out < want_total and gap_s > 0:
            time.sleep(gap_s)
            res.gap_total += gap_s

    res.elapsed = time.perf_counter() - t_start
    return res


def len_chunk(done: int, total: int, chunk: int) -> int:
    return min(chunk, total - done)


def run_sustained(ser: serial.Serial, data: bytes, timeout: float) -> PacedResult:
    """一次性发完（旧行为）。用于复现持续满载下的溢出丢字节。"""
    import threading

    res = PacedResult(len(data))

    def writer():
        try:
            res.out = ser.write(data) or len(data)
        except Exception as exc:  # noqa: BLE001
            res.error = f"写失败：{exc}"

    t_start = time.perf_counter()
    wt = threading.Thread(target=writer, daemon=True)
    wt.start()

    deadline = time.perf_counter() + timeout
    while len(res.received) < len(data):
        if time.perf_counter() > deadline:
            res.error = res.error or f"超时：只收到 {len(res.received)}/{len(data)} 字节"
            break
        got = ser.read(min(65536, len(data) - len(res.received)))
        if got:
            res.received += got
            deadline = time.perf_counter() + timeout
    wt.join(timeout=timeout)
    res.elapsed = time.perf_counter() - t_start
    res.chunks = 1
    return res


def measure(port: str, baud: int, total: int, mode: str, chunk: int,
            gap_s: float, timeout: float) -> dict:
    row = {
        "baud": baud, "total": total, "mode": mode, "chunk": chunk, "gap_ms": gap_s * 1000,
        "ok": False, "note": "", "seconds": None, "avg_bps": None, "eff_bps": None,
        "uart_util": None, "usb_util": None, "chunks": 0, "recv": 0,
        "brr": sdk_brr(baud), "actual_baud": actual_baud(baud),
    }

    try:
        ser = serial.Serial(port, baudrate=baud, timeout=0.2,
                            write_timeout=timeout, rtscts=False, dsrdtr=False, xonxoff=False)
    except Exception as exc:  # noqa: BLE001
        row["note"] = f"打开失败：{exc}"
        return row

    try:
        time.sleep(0.2)
        ser.reset_input_buffer()
        ser.reset_output_buffer()
        time.sleep(0.05)

        data = payload_bytes(total)
        if mode == "chunked":
            res = run_chunked(ser, data, chunk, gap_s, timeout)
        else:
            res = run_sustained(ser, data, timeout)

        row["seconds"] = res.elapsed
        row["chunks"] = res.chunks
        row["recv"] = len(res.received)

        if res.elapsed > 0 and res.received:
            row["avg_bps"] = len(res.received) / res.elapsed
            if res.transfer_time > 0:
                row["eff_bps"] = len(res.received) / res.transfer_time
                row["uart_util"] = row["eff_bps"] / (baud / BITS_PER_BYTE) * 100.0
            row["usb_util"] = (2.0 * len(res.received) / res.elapsed
                               / USB_FS_BULK_BYTES_PER_S * 100.0)

        ok, bad = res.check(data)
        row["ok"] = ok
        if not ok:
            if res.error:
                row["note"] = res.error
            else:
                row["note"] = (f"数据不符（首个不同位置 {bad}，"
                               f"收 {len(res.received)}/{total}）")
    finally:
        ser.close()

    return row


def fmt(v, spec: str, dash: str = "—") -> str:
    return dash if v is None else format(v, spec)


def main() -> int:
    ap = argparse.ArgumentParser(
        description="CDC 环回：高波特率 + 小载荷 / 间隔分块测试（需 PB0-PB1 环回跳线）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--port", help="COM 口，默认按 VID:PID 自动查找")
    ap.add_argument("--baud", default=",".join(str(b) for b in DEFAULT_BAUDS),
                    help="波特率列表，逗号分隔（默认 %(default)s）")
    ap.add_argument("--mode", choices=["chunked", "sustained"], default="chunked",
                    help="chunked=分块+间隔（默认）；sustained=一次性发完")
    ap.add_argument("--chunk", type=int, default=4096, help="分块模式的块大小（默认 %(default)s）")
    ap.add_argument("--gap-ms", type=float, default=5.0, help="块间空隙（默认 %(default)s ms）")
    ap.add_argument("--total", type=int, help="每档总字节数（默认按波特率推算到 ~1.5 s）")
    ap.add_argument("--repeats", type=int, default=1, help="每档重复次数，取中位")
    ap.add_argument("--csv", help="结果写入 CSV")
    args = ap.parse_args()

    port = args.port or find_dap_port()
    if not port:
        print("找不到 DAP 的 CDC 口，请用 --port 指定", file=sys.stderr)
        return 1

    try:
        bauds = [int(b.strip()) for b in args.baud.split(",") if b.strip()]
    except ValueError:
        print("--baud 需为逗号分隔的整数", file=sys.stderr)
        return 1

    print()
    print(f"端口    : {port}")
    print(f"模式    : {args.mode}"
          + (f"（块 {args.chunk} B，间隔 {args.gap_ms:g} ms）" if args.mode == "chunked" else ""))
    print(f"重复    : {args.repeats}")
    print()

    rows = []
    for baud in bauds:
        total = args.total
        if total is None:
            # 按理论可实现速率推算 ~1.5 s 的载荷，且不超过 1 MiB
            rate = actual_baud(baud) or baud
            total = int(rate / BITS_PER_BYTE * 1.5)
            total = max(4096, (total + 4095) // 4096 * 4096)
            total = min(total, 1024 * 1024)

        # 超时要按「最坏情况」给：设备可能实际跑得比请求慢很多
        line = (actual_baud(baud) or baud) / BITS_PER_BYTE
        timeout = max(5.0, total / max(line, 1.0) * 2.0 + total / 1000.0 + 3.0)

        print(f"  {baud:>9} baud  {brr_desc(baud)}  实际≈{fmt(actual_baud(baud), '.0f')} baud  "
              f"总量 {total} B")

        runs = []
        for r in range(args.repeats):
            row = measure(port, baud, total, args.mode, args.chunk,
                          args.gap_ms / 1000.0, timeout)
            runs.append(row)
            mark = "OK  " if row["ok"] else "FAIL"
            note = f"  {row['note']}" if row["note"] else ""
            tag = f"  #{r + 1}" if args.repeats > 1 else ""
            print(f"      {mark} 收 {row['recv']:>7}/{row['total']} B  块 {row['chunks']:>5}  "
                  f"{fmt(row['seconds'], '7.3f')} s  "
                  f"整体 {fmt(row['avg_bps'], '10.1f')} B/s  "
                  f"等效 {fmt(row['eff_bps'], '10.1f')} B/s  "
                  f"利用率 {fmt(row['uart_util'], '6.1f')}%{note}{tag}")

        good = [r for r in runs if r["ok"] and r["avg_bps"]]
        if not good:
            bad = dict(runs[-1])
            for k in ("avg_bps", "eff_bps", "uart_util", "usb_util", "seconds"):
                bad[k] = None
            rows.append(bad)
            continue

        rows.append({
            "baud": baud,
            "brr_ok": True,
            "actual_baud": actual_baud(baud),
            "total": total,
            "mode": args.mode,
            "chunk": args.chunk,
            "gap_ms": args.gap_ms,
            "chunks": good[0]["chunks"],
            "seconds": statistics.median([r["seconds"] for r in good]),
            "avg_bps": statistics.median([r["avg_bps"] for r in good]),
            "eff_bps": statistics.median([r["eff_bps"] for r in good]),
            "uart_util": statistics.median([r["uart_util"] for r in good]),
            "usb_util": statistics.median([r["usb_util"] for r in good]),
            "ok": True,
            "note": "",
            "brr": sdk_brr(baud),
        })

    # ---------- 汇总 ----------
    print()
    bold = 112
    print("=" * bold)
    print(f"  {'请求波特率':>10} {'BRR':>6} {'实际波特率':>10} {'总量':>8} {'块':>6} "
          f"{'耗时':>8} {'整体速率':>13} {'等效速率':>13} {'UART利用率':>9} {'状态':>5}")
    print("-" * bold)
    for row in rows:
        act = row.get("actual_baud")
        if row.get("avg_bps") is None:
            print(f"  {row['baud']:>10} {row.get('brr', 0):>6} "
                  f"{fmt(act, '>10.0f')} {'—':>8} {'—':>6} {'—':>8} "
                  f"{'—':>13} {'—':>13} {'—':>9} {'失败':>5}")
            continue
        print(f"  {row['baud']:>10} {row['brr']:>6} {act:>10.0f} {row['total']:>8} "
              f"{row['chunks']:>6} {row['seconds']:>8.3f} "
              f"{row['avg_bps'] / 1024:>8.1f} KiB/s "
              f"{row['eff_bps'] / 1024:>8.1f} KiB/s "
              f"{row['uart_util']:>8.1f}% {'OK':>5}")
    print("=" * bold)
    print("说明：")
    print("  - BRR = fPCLK/波特率（fPCLK=48 MHz），UART 实际速率只能是 48M/BRR 这些离散值。")
    print("    BRR<16 表示 USARTDIV<1，超出 16 倍过采样规范，硬件能否正常分频需看实测结果。")
    print("  - 整体速率 = 总收字节/总耗时（含块间空隙）；等效速率 = 扣除空隙后的纯传输速率。")
    print("  - UART 利用率 = 等效速率 ÷ (请求波特率/10)。≈100% 表示 UART 已跑满请求速率；")
    print("    明显偏低可能是块太小（USB 往返开销占比高）或设备实际速率与请求不符。")
    print()

    if args.csv:
        import csv
        with open(args.csv, "w", newline="", encoding="utf-8") as fh:
            w = csv.writer(fh)
            w.writerow(["baud", "brr", "actual_baud", "total_bytes", "mode", "chunk",
                        "gap_ms", "chunks", "seconds", "avg_Bps", "eff_Bps",
                        "uart_util_pct", "usb_util_pct", "ok", "note"])
            for row in rows:
                w.writerow([
                    row["baud"], row.get("brr", ""), fmt(row.get("actual_baud"), ".0f", ""),
                    row.get("total", ""), row.get("mode", ""), row.get("chunk", ""),
                    row.get("gap_ms", ""), row.get("chunks", ""),
                    fmt(row.get("seconds"), ".6f", ""), fmt(row.get("avg_bps"), ".1f", ""),
                    fmt(row.get("eff_bps"), ".1f", ""), fmt(row.get("uart_util"), ".2f", ""),
                    fmt(row.get("usb_util"), ".2f", ""),
                    "1" if row.get("ok") else "0", row.get("note", ""),
                ])
        print(f"已写入 {args.csv}")

    return 0 if all(r.get("ok") for r in rows) else 2


if __name__ == "__main__":
    sys.exit(main())
