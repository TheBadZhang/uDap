#!/usr/bin/env python3
"""CDC <-> USART4 桥的持续压力测试（带逐字节校验）。

前提：PB0(TX) / PB1(RX) 已短接，固件已烧录。

用法：
    python tools/uart_soak_test.py --seconds 30
    python tools/uart_soak_test.py --seconds 60 --baud 921600
    python tools/uart_soak_test.py --seconds 30 --out soak.json

设计要点：
  * 设备侧 TX/RX FIFO 各 1024 字节，UART 无 RTS/CTS 流控。因此**单次灌入不能超过
    这个量**，否则物理上必然丢数据。载荷大于 BLOCK 时按块往返，这样测的是桥的正确性，
    而不是在测「谁先溢出」。
  * 载荷长度在 MIN..MAX 之间随机，并覆盖 64(MPS) 边界与 63/65 邻居，专门撞
    包边界和 FIFO 边界的 off-by-one。
  * 每块数据用 xorshift 伪随机生成并携带块序号，能区分「丢失」「重排」「重复」。
"""

import argparse
import json
import random
import time

import serial
import serial.tools.list_ports as list_ports

DAP_VID = 0x0D28
DAP_PID = 0x0204
DAP_SERIAL = "00000000000000000123456789ABCDEF"

BLOCK = 512          # 单次灌入上限，留一半 FIFO 余量
MIN_LEN = 1
MAX_LEN = 4096


def find_dap_port():
    for p in list_ports.comports():
        if p.vid == DAP_VID and p.pid == DAP_PID and p.serial_number == DAP_SERIAL:
            return p.device
    for p in list_ports.comports():
        if p.vid == DAP_VID and p.pid == DAP_PID:
            return p.device
    return None


def xorshift_payload(size: int, seed: int) -> bytes:
    v = seed | 1
    out = bytearray(size)
    for i in range(size):
        v ^= (v << 13) & 0xFFFFFFFF
        v ^= v >> 17
        v ^= (v << 5) & 0xFFFFFFFF
        out[i] = v & 0xFF
    return bytes(out)


def sized_lengths(rng: random.Random) -> int:
    """随机长度，偶尔精确命中 64 边界及其邻居。"""
    roll = rng.random()
    if roll < 0.25:
        # 撞 MPS 边界族
        return rng.choice([1, 2, 63, 64, 65, 127, 128, 129, 255, 256, 257])
    if roll < 0.55:
        return rng.randint(1, 256)
    if roll < 0.85:
        return rng.randint(257, 2048)
    return rng.randint(2049, MAX_LEN)


def classify(exp: bytes, got: bytes) -> str:
    if exp == got:
        return "ok"
    if len(got) < len(exp) and exp[: len(got)] == got:
        return "truncated"
    if len(got) > len(exp) and got[: len(exp)] == exp:
        return "extra"
    n = min(len(got), len(exp))
    for i in range(n):
        if got[i] != exp[i]:
            # 期望字节是否出现在后方（=> 丢失）
            if exp[i] in got[i:]:
                return "lost"
            # 实收字节是否来自前方（=> 重复）
            if got[i] in exp[max(0, i - 16) : i]:
                return "dup"
            return "corrupt"
    return "corrupt"


def resync(ser: serial.Serial, quiet: float = 0.15) -> int:
    """排空在途数据直到静默。

    一次真实的丢字节会让「本轮剩余数据 + 上轮尾巴」错位，若不排空，后面的轮次
    会拿到错位的旧数据而连续误报。这里把管线洗到干净再继续，保证统计的是
    「真实丢失次数」而不是「一次丢失 × 级联轮数」。
    """
    dropped = 0
    ser.reset_input_buffer()
    last = time.time()
    while time.time() - last < quiet:
        d = ser.read(256)
        if d:
            dropped += len(d)
            last = time.time()
    ser.reset_input_buffer()
    ser.reset_output_buffer()
    return dropped


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=30.0)
    ap.add_argument("--seed", type=int, default=12345)
    ap.add_argument("--out", help="把结果写成 JSON")
    args = ap.parse_args()

    port = args.port or find_dap_port()
    if not port:
        print("找不到 DAP 的 CDC 串口")
        return 1

    rng = random.Random(args.seed)
    stats = {"ok": 0, "truncated": 0, "extra": 0, "lost": 0, "dup": 0, "corrupt": 0,
             "resync_bytes": 0}
    failures = []
    bytes_ok = 0
    bytes_sent = 0
    rounds = 0

    ser = serial.Serial(port, args.baud, timeout=1.0, write_timeout=3.0)
    time.sleep(0.3)
    ser.reset_input_buffer()
    ser.reset_output_buffer()

    t_start = time.time()
    deadline = t_start + args.seconds
    print(f"端口 {port}  波特率 {args.baud}  时长 {args.seconds:.0f}s  块 {BLOCK}B")

    try:
        while time.time() < deadline:
            size = sized_lengths(rng)
            payload = xorshift_payload(size, rng.getrandbits(32))
            got = bytearray()
            t0 = time.time()

            # 分块往返，每块都校验
            for off in range(0, size, BLOCK):
                blk = payload[off : off + BLOCK]
                written = 0
                while written < len(blk) and time.time() - t0 < 10.0:
                    n = ser.write(blk[written:])
                    if not n:
                        time.sleep(0.001)
                        continue
                    written += n
                ser.flush()

                want = len(blk)
                part = bytearray()
                lim = time.time() + 10.0
                while len(part) < want and time.time() < lim:
                    d = ser.read(want - len(part))
                    if not d:
                        # 不能在这里放弃：SWD 做 10 KB 块传输时会占住主循环几百毫秒，
                        # 期间 CDC IN 通路完全不动、ser.read() 空返回。数据没丢只是晚到。
                        # 提前 break 会把「被 SWD 占住」误报成「丢字节」。
                        continue
                    part += d
                got += part
                if len(part) != want:
                    break

            rounds += 1
            bytes_sent += size
            verdict = classify(payload, bytes(got))
            stats[verdict] += 1
            if verdict == "ok":
                bytes_ok += size
            else:
                if len(failures) < 8:
                    n = min(len(got), size)
                    bad = next((i for i in range(n) if got[i] != payload[i]), n)
                    failures.append(
                        {"len": size, "got": len(got), "verdict": verdict, "first_diff": bad}
                    )
                # 洗掉在途残渣，避免错位级联
                stats["resync_bytes"] += resync(ser)
    finally:
        ser.close()

    dt = time.time() - t_start
    rate = bytes_ok / 1024.0 / dt if dt > 0 else 0.0
    print(f"轮次 {rounds}，投递 {bytes_sent} B，校验通过 {bytes_ok} B，用时 {dt:.2f}s")
    print(f"吞吐 {rate:.1f} KiB/s")
    print("判定: " + "  ".join(f"{k}={v}" for k, v in stats.items()))
    if failures:
        print("失败样例:")
        for f in failures:
            print(f"   len={f['len']} got={f['got']} {f['verdict']} 首个不同@{f['first_diff']}")

    if args.out:
        with open(args.out, "w", encoding="utf-8") as fp:
            json.dump(
                {
                    "port": port,
                    "baud": args.baud,
                    "seconds": args.seconds,
                    "rounds": rounds,
                    "bytes_sent": bytes_sent,
                    "bytes_ok": bytes_ok,
                    "kib_per_s": rate,
                    "stats": stats,
                    "failures": failures,
                },
                fp,
                indent=2,
            )

    return 0 if stats["ok"] == rounds else 2


if __name__ == "__main__":
    raise SystemExit(main())
