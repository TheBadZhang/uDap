#!/usr/bin/env python3
"""量化 CDC <-> UART 桥在「无背压」下的丢字节行为，并复现读侧永久阻塞。

前提：DAP 的 UART 引脚 PB0(TX) 与 PB1(RX) 已短接（回环）。

背景：固件的 usbd_cdc_acm_bulk_out() 在把数据交给 UART TX FIFO 之后，
**无条件**立刻重挂 CDC OUT 接收端点，不看 FIFO 剩余空间。于是：

  * USB(bulk) 以 MB/s 级速度把主机写的数据全塞进设备；
  * TX FIFO 只有 UART_TX_FIFO_SIZE 字节，排水速度 = UART 波特率；
  * 装不下的字节被 chry_ringbuffer 丢弃，只累加 uart_tx_overflow 计数。

结果：主机一次写 N 字节，只有约 (FIFO_SIZE + 传输期间排掉的量) 会真正发出。

本脚本做两件事：
  1. 量出「一次写入 N 字节 -> 实际回读多少」，并和 FIFO 模型对照；
  2. 复现「脚本一直不结束」：用无超时的严格读取去等一个永远到不齐的长度。

用法：
    python tools/uart_flow_control_test.py --port COM21
    python tools/uart_flow_control_test.py --port COM21 --baud 115200
    python tools/uart_flow_control_test.py --port COM21 --skip-hang-probe
"""

import argparse
import time

import serial

# 与本工程 drv_uart.c 的 UART_FIFO_SIZE 保持一致。改固件时这里也要改，
# 否则下面「模型预测」一栏会失真。
UART_FIFO_SIZE = 1024

# 一次写入的字节数样本。覆盖 FIFO 之内、边界、以及远超 FIFO 的情形。
BLAST_SIZES = (256, 512, 1024, 1025, 2048, 4096, 8192, 16384)


def payload(size: int, seed: int) -> bytes:
    """xorshift 伪随机载荷，避免 0xAA 之类的规律值掩盖乱序/偏移问题。"""
    value = seed & 0xFFFFFFFF
    data = bytearray(size)
    for i in range(size):
        value ^= (value << 13) & 0xFFFFFFFF
        value ^= value >> 17
        value ^= (value << 5) & 0xFFFFFFFF
        data[i] = value & 0xFF
    return bytes(data)


def drain(ser: serial.Serial, quiet: float = 0.3) -> int:
    """读到连续 quiet 秒没有新数据为止，返回丢弃的字节数。"""
    total = 0
    deadline = time.monotonic() + 2.0
    last = time.monotonic()
    while time.monotonic() < deadline:
        chunk = ser.read(4096)
        if chunk:
            total += len(chunk)
            last = time.monotonic()
        elif time.monotonic() - last > quiet:
            break
    return total


def first_mismatch(got: bytes, want: bytes) -> int | None:
    for i in range(min(len(got), len(want))):
        if got[i] != want[i]:
            return i
    return None


def run_blast(ser: serial.Serial, size: int, baud: int) -> dict:
    """一次写出 size 字节，用有界超时读回，统计实际到达量。"""
    data = payload(size, 0x6D2B79F5)

    ser.reset_input_buffer()
    ser.reset_output_buffer()
    time.sleep(0.05)
    drain(ser, quiet=0.05)

    # 有界超时：绝不无限等待。允许排空 FIFO 所需时间的 3 倍 + 余量。
    # 注意：固件带背压后，write() 本身也会阻塞到串口排完数据，所以
    # 写侧的超时必须留够，否则会抛 SerialTimeoutException。
    line_seconds = size / (baud / 10.0)
    budget = min(3.0 * line_seconds + 0.5, 20.0)
    ser.write_timeout = max(5.0, 2.0 * line_seconds + 1.0)

    t0 = time.monotonic()
    written = ser.write(data)
    ser.flush()
    t_write = time.monotonic() - t0

    got = bytearray()
    deadline = time.monotonic() + budget
    while len(got) < size and time.monotonic() < deadline:
        chunk = ser.read(size - len(got))
        if chunk:
            got += chunk
        else:
            break
    t_read = time.monotonic() - t0

    bad = first_mismatch(bytes(got), data)
    return {
        "size": size,
        "written": written,
        "got": len(got),
        "lost": size - len(got),
        "t_write_ms": t_write * 1000.0,
        "t_total_ms": t_read * 1000.0,
        "prefix_ok": bad is None or bad > 0,
        "first_bad": bad,
    }


def probe_hang(ser: serial.Serial, size: int) -> str:
    """复现读侧永久阻塞：用无超时的 read() 去等一个到不齐的长度。

    这里用「先设 timeout=None，再读 size 字节」的方式。因为固件已把多出来的
    数据丢掉，size 字节永远不会齐，read() 会一直不返回 —— 这正是脚本
    「一直跑不能结束」的成因。
    """
    ser.timeout = None
    ser.reset_input_buffer()
    ser.reset_output_buffer()
    time.sleep(0.05)

    ser.write(b"\xa5" * 8)
    ser.flush()
    # 先确认回环是通的，避免把「没插回环线」误判成「卡死」。
    echo = ser.read(8)
    if echo != b"\xa5" * 8:
        ser.timeout = 0.5
        return f"回环不通（回读 {len(echo)} 字节），跳过卡死复现"

    ser.reset_input_buffer()
    ser.write(payload(size, 0x1234))
    ser.flush()

    # 关键：读一个永远到不齐的长度，且没有超时。
    try:
        ser.read(size)
        ser.timeout = 0.5
        return f"未阻塞（读满了 {size} 字节）—— 说明此时没有丢数据"
    except Exception as exc:  # 正常不应该走到这里
        ser.timeout = 0.5
        return f"异常: {exc!r}"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True, help="CDC 串口号，例如 COM21")
    ap.add_argument("--baud", type=int, default=115200, help="CDC 行编码里的 UART 波特率")
    ap.add_argument("--skip-hang-probe", action="store_true", help="跳过卡死复现")
    args = ap.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=0.5, write_timeout=5.0)
    time.sleep(0.3)
    try:
        print(f"端口 {args.port}  波特率 {args.baud}  固件 FIFO {UART_FIFO_SIZE} B")
        print()
        print("=== 一次写入 N 字节，实际回读多少 ===")
        print(f"  {'写入':>7} {'回读':>7} {'丢失':>7} {'丢%':>6} "
              f"{'写耗时':>9} {'总耗时':>9} {'前缀OK':>7} {'首个错误':>9}")
        print("  " + "-" * 74)

        rows = []
        for size in BLAST_SIZES:
            r = run_blast(ser, size, args.baud)
            rows.append(r)
            loss_pct = r["lost"] * 100.0 / size
            bad = "无" if r["first_bad"] is None else str(r["first_bad"])
            print(f"  {r['written']:>7} {r['got']:>7} {r['lost']:>7} {loss_pct:>5.1f}% "
                  f"{r['t_write_ms']:>8.2f}ms {r['t_total_ms']:>8.1f}ms "
                  f"{'是' if r['prefix_ok'] else '否':>7} {bad:>9}")
            # 每次爆完都把在途残渣清掉，避免下一轮读到上一轮的尾巴
            drain(ser, quiet=0.2)

        print()
        print("=== 与 FIFO 模型对照 ===")
        bloated = [r for r in rows if r["t_write_ms"] > 100.0]
        if bloated:
            # 写入被阻塞 -> 说明固件已启用背压，主机写被 NAK 限流到串口速率。
            # 此时「能到达量」就等于写入量，无需再看 USB 突发时序。
            print("  检测到 write() 被阻塞 -> 固件已带背压（主机写被限流到串口速率）")
            print(f"  {'写入':>7} {'实测回读':>9} {'理论上限':>9} {'偏差':>7}")
            print("  " + "-" * 40)
            for r in rows:
                print(f"  {r['size']:>7} {r['got']:>9} {r['size']:>9} "
                      f"{r['got'] - r['size']:>+7}")
        else:
            print("  write() 几乎立即返回 -> 固件无背压（旧行为）")
            print("  模型：能达到量 ≈ FIFO + USB传输期间排掉的字节")
            print(f"  {'写入':>7} {'实测回读':>9} {'模型预测':>9} {'偏差':>7}")
            print("  " + "-" * 40)
            for r in rows:
                drained = (r["t_write_ms"] / 1000.0) * (args.baud / 10.0)
                predicted = min(r["size"], UART_FIFO_SIZE + drained)
                print(f"  {r['size']:>7} {r['got']:>9} {predicted:>9.0f} "
                      f"{r['got'] - predicted:>+7.0f}")

        print()
        print("=== 结论 ===")
        over = [r for r in rows if r["size"] > UART_FIFO_SIZE and r["got"] < r["size"]]
        if over:
            worst = max(over, key=lambda r: r["lost"])
            print(f"  * 写入超过 FIFO({UART_FIFO_SIZE} B) 后必然丢字节，"
                  f"最差 {worst['size']} B 丢 {worst['lost']} B "
                  f"({worst['lost'] * 100.0 / worst['size']:.1f}%)")
            print("  * 根因：CDC OUT 回调无条件重挂端点，没有背压。")
            print("  * 主机侧表现：期望读满 N 字节的脚本会永久阻塞 -> "
                  "「一直跑不能结束」。")
        else:
            print("  * 未丢字节 —— 背压生效。")
            print("  * 机制：CDC OUT 回调先查 UART TX FIFO 余量，不足则不重挂端点；"
                  "主机持续收到 NAK，其 write() 自然阻塞在串口波特率上。")
            print(f"  * 代价：主机一次大 write() 的耗时 = 载荷 / 线速率"
                  f"（{args.baud} baud 下 8 KiB ≈ "
                  f"{8192 / (args.baud / 10.0) * 1000:.0f} ms）。")
            print("  * 因此主机侧必须设足 write_timeout，否则会抛 "
                  "SerialTimeoutException。")

        if not args.skip_hang_probe:
            print()
            print("=== 卡死复现（无超时读一个到不齐的长度）===")
            print("  注意：若固件确实丢数据，这一项会一直不返回，")
            print("        需要在外部超时后手动中断（这是预期行为，即为故障现象）。")
            verdict = probe_hang(ser, 8192)
            print(f"  结果: {verdict}")

        return 0
    finally:
        ser.timeout = 0.5
        ser.close()


if __name__ == "__main__":
    raise SystemExit(main())
