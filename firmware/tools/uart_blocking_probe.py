#!/usr/bin/env python3
"""最小复现：CDC 读侧在「数据被固件丢弃」时永久阻塞。

证明链条：
  1. 固件丢掉超出 FIFO 的字节（tools/uart_flow_control_test.py 已量化）；
  2. 于是读侧等一个到不齐的长度时，read() 永远不返回；
  3. 任何「写多字节再等额读回」的脚本都会卡在这一步。

用法：
    python tools/uart_blocking_probe.py --port COM21
    python tools/uart_blocking_probe.py --port COM21 --timeout 3     # 带超时，用于对照
"""

import argparse
import time

import serial


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--size", type=int, default=8192, help="一次写入并期望读回的字节数")
    ap.add_argument("--timeout", type=float, default=None,
                    help="read 超时秒数；不传则用 None（无限等待），复现卡死")
    args = ap.parse_args()

    # timeout=None 是 pyserial 的默认值：read(n) 会一直阻塞到凑满 n 字节。
    ser = serial.Serial(args.port, args.baud, timeout=args.timeout, write_timeout=5.0)
    time.sleep(0.3)
    try:
        print(f"[1] 打开 {args.port} @ {args.baud}，read timeout = {ser.timeout}", flush=True)

        ser.reset_input_buffer()
        ser.reset_output_buffer()
        time.sleep(0.05)

        # 先确认回环通，避免把「没插回环」误判为卡死
        ser.write(b"\xa5" * 8)
        ser.flush()
        echo = ser.read(8) if args.timeout is not None else b""
        if args.timeout is not None and echo != b"\xa5" * 8:
            print(f"[!] 回环不通（回读 {len(echo)} 字节），无法继续", flush=True)
            return 1
        print("[2] 回环正常", flush=True)

        ser.reset_input_buffer()
        data = bytes((i * 137 + 41) & 0xFF for i in range(args.size))

        t0 = time.monotonic()
        n = ser.write(data)
        ser.flush()
        print(f"[3] write({args.size}) 返回 {n}，耗时 {(time.monotonic() - t0) * 1000:.2f} ms", flush=True)

        print(f"[4] 开始 read({args.size}) —— timeout={ser.timeout}", flush=True)
        t0 = time.monotonic()
        got = ser.read(args.size)
        elapsed = time.monotonic() - t0
        print(f"[5] read 返回 {len(got)} 字节，耗时 {elapsed * 1000:.1f} ms", flush=True)

        if len(got) < args.size:
            print(f"[!] 少 {args.size - len(got)} 字节；"
                  f"若 timeout=None，这里会永远不返回 -> 脚本看起来「一直跑」", flush=True)
        else:
            print("[OK] 读满，本配置下未丢字节", flush=True)
        return 0
    finally:
        ser.close()


if __name__ == "__main__":
    raise SystemExit(main())
