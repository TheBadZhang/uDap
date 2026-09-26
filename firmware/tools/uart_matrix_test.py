#!/usr/bin/env python3
"""CDC <-> UART 桥的波特率 × 包大小矩阵测试。

前提：DAP 的 UART 引脚 PB0(TX) 与 PB1(RX) 已短接（回环）。

回环是全双工，所以一次「发 N 字节再读回 N 字节」的耗时约等于
单向载荷 / 线速率。据此可以把实测吞吐与线速率上限作比，判断
是否受 CPU/USB 侧限制。

用法：
    python tools/uart_matrix_test.py --port COM21
    python tools/uart_matrix_test.py --port COM21 --baud 115200,921600,2500000
    python tools/uart_matrix_test.py --port COM21 --sizes 1,64,1024,8192
"""

import argparse
import statistics
import time

import serial

# 默认波特率。覆盖低速、常见值、以及之前测出的饱和点附近。
DEFAULT_BAUDS = (115200, 460800, 921600, 1500000, 2500000)

# 默认包大小。1/64 撞 64B 端点包边界与 FIFO 边界，块大会受背压限流。
DEFAULT_SIZES = (1, 64, 512, 4096, 8192)


def payload(size: int, seed: int) -> bytes:
    """xorshift 伪随机载荷：规律值（如全 0xAA）会掩盖乱序/偏移问题。"""
    value = seed & 0xFFFFFFFF
    data = bytearray(size)
    for i in range(size):
        value ^= (value << 13) & 0xFFFFFFFF
        value ^= value >> 17
        value ^= (value << 5) & 0xFFFFFFFF
        data[i] = value & 0xFF
    return bytes(data)


def drain(ser: serial.Serial, quiet: float = 0.25) -> int:
    """读到连续 quiet 秒无新数据，返回丢弃的字节数。"""
    total = 0
    last = time.monotonic()
    deadline = last + 3.0
    while time.monotonic() < deadline:
        chunk = ser.read(8192)
        if chunk:
            total += len(chunk)
            last = time.monotonic()
        elif time.monotonic() - last > quiet:
            break
    return total


def one_case(ser: serial.Serial, size: int, baud: int, rounds: int) -> dict:
    """跑 rounds 次往返，返回通过次数与时间样本。"""
    data = payload(size, 0x6D2B79F5)
    line_s = size / (baud / 10.0)          # 单向线速率理论耗时
    budget = min(4.0 * line_s + 0.5, 30.0)  # 单次往返的读取预算

    # 背压会让主机的 write() 阻塞到串口排完，写超时必须留够。
    ser.write_timeout = max(5.0, 3.0 * line_s + 1.0)
    ser.timeout = 0.05

    times = []
    passed = 0
    first_bad = None

    for _ in range(rounds):
        ser.reset_input_buffer()
        ser.reset_output_buffer()
        drain(ser, quiet=0.03)

        got = bytearray()
        t0 = time.monotonic()
        try:
            ser.write(data)
            ser.flush()
        except serial.SerialException as exc:
            return {"error": f"write 失败: {exc}"}

        deadline = time.monotonic() + budget
        while len(got) < size and time.monotonic() < deadline:
            chunk = ser.read(size - len(got))
            if chunk:
                got += chunk
            else:
                break
        times.append(time.monotonic() - t0)

        if bytes(got) == data:
            passed += 1
        elif first_bad is None:
            n = min(len(got), size)
            bad = next((i for i in range(n) if got[i] != data[i]), n)
            first_bad = (bad, len(got))

    return {
        "passed": passed,
        "rounds": rounds,
        "times": times,
        "first_bad": first_bad,
        "line_s": line_s,
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", default=",".join(str(b) for b in DEFAULT_BAUDS))
    ap.add_argument("--sizes", default=",".join(str(s) for s in DEFAULT_SIZES))
    ap.add_argument("--rounds", type=int, default=5)
    args = ap.parse_args()

    bauds = [int(x) for x in args.baud.split(",")]
    sizes = [int(x) for x in args.sizes.split(",")]

    print(f"端口 {args.port}  每格 {args.rounds} 次往返")
    print(f"波特率 {bauds}")
    print(f"包大小 {sizes} B")
    print()

    rows = {b: {} for b in bauds}

    for baud in bauds:
        try:
            ser = serial.Serial(args.port, baud, timeout=0.05, write_timeout=5.0)
        except Exception as exc:
            for size in sizes:
                rows[baud][size] = {"error": str(exc)}
            continue
        time.sleep(0.4)  # 等 CDC 行编码下发到设备

        for size in sizes:
            rows[baud][size] = one_case(ser, size, baud, args.rounds)
        ser.close()

    # ---- 可用性矩阵：通过次数 ----
    print("=== 可用性（通过 / 总次数）===")
    head = "  {:>9} |".format("波特率")
    for s in sizes:
        head += "{:>12}".format(f"{s}B" if s < 1024 else f"{s // 1024}KB")
    print(head)
    print("  " + "-" * (11 + 12 * len(sizes)))
    for baud in bauds:
        line = "  {:>9} |".format(baud)
        for s in sizes:
            r = rows[baud][s]
            if "error" in r:
                line += "{:>12}".format("打开失败")
            else:
                line += "{:>12}".format(f"{r['passed']}/{r['rounds']}")
        print(line)

    # ---- 吞吐矩阵 ----
    print()
    print("=== 实测吞吐 KiB/s（括号内 = 相对线速率上限的百分比）===")
    print(head)
    print("  " + "-" * (11 + 12 * len(sizes)))
    for baud in bauds:
        line = "  {:>9} |".format(baud)
        for s in sizes:
            r = rows[baud][s]
            if "error" in r or not r["times"]:
                line += "{:>12}".format("—")
                continue
            # 用中位数：避免个别超时样本拉偏
            med = statistics.median(r["times"])
            kibs = (s / 1024.0) / med
            line_s = r["line_s"]
            eff = (line_s / med * 100.0) if med > 0 else 0.0
            line += "{:>12}".format(f"{kibs:.0f} ({eff:.0f}%)")
        print(line)

    # ---- 失败定位 ----
    print()
    problems = []
    for baud in bauds:
        for s in sizes:
            r = rows[baud][s]
            if "error" in r:
                problems.append(f"{baud}Hz/{s}B: {r['error']}")
            elif r["passed"] != r["rounds"]:
                if r["first_bad"]:
                    bad, got = r["first_bad"]
                    problems.append(
                        f"{baud}Hz/{s}B: 通过 {r['passed']}/{r['rounds']}，"
                        f"首个不符 @{bad}（收到 {got}/{s} 字节）"
                    )
                else:
                    problems.append(
                        f"{baud}Hz/{s}B: 通过 {r['passed']}/{r['rounds']}"
                    )
    if problems:
        print("=== 问题 ===")
        for p in problems:
            print("  * " + p)
        return 2

    print("=== 全部通过 ===")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
