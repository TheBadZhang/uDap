#!/usr/bin/env python3
"""CDC <-> USART4 桥的回环连通性测试。

前提：DAP 的 UART 引脚 PB0(TX) 与 PB1(RX) 已短接，且固件已烧录。

用法：
    python tools/uart_loopback_test.py                 # 自动找 COM 口
    python tools/uart_loopback_test.py --port COM20
    python tools/uart_loopback_test.py --baud 921600   # 同时设置 UART 侧波特率
"""

import argparse
import time

import serial
import serial.tools.list_ports as list_ports

# DAP 的 USB 标识
DAP_VID = 0x0D28
DAP_PID = 0x0204
DAP_SERIAL = "00000000000000000123456789ABCDEF"


def find_dap_port() -> str | None:
    """按 VID/PID + 序列号定位 CDC 口（VID/PID 与某些 DAPLink 冲突，故用序列号区分）。"""
    for p in list_ports.comports():
        if p.vid == DAP_VID and p.pid == DAP_PID:
            if p.serial_number == DAP_SERIAL:
                return p.device
    # 退而求其次：只看 VID/PID
    for p in list_ports.comports():
        if p.vid == DAP_VID and p.pid == DAP_PID:
            return p.device
    return None


def case_payloads():
    """构造测试用例：(名字, 载荷, 分块大小)"""
    return [
        ("1B", b"\xa5", 64),
        ("5B 短包", b"HELLO", 64),
        ("16B 递增", bytes(range(16)), 64),
        ("63B 短包", bytes(range(63)), 64),
        ("64B = MPS", bytes(range(64)), 64),
        ("65B 跨包", bytes(range(65)), 64),
        ("128B = 2*MPS", bytes(range(128)), 64),
        ("256B 0xAA", b"\xaa" * 256, 64),
        ("256B 递增", bytes(range(256)), 64),
        ("512B 0x55", b"\x55" * 512, 64),
        ("1KB 递增", bytes(range(256)) * 4, 64),
        ("1KB 大块", bytes(range(256)) * 4, 512),
        ("2KB 伪随机", bytes((i * 137 + 41) & 0xFF for i in range(2048)), 256),
        ("4KB 伪随机", bytes((i * 137 + 41) & 0xFF for i in range(4096)), 512),
        ("8KB 伪随机", bytes((i * 137 + 41) & 0xFF for i in range(8192)), 512),
    ]


# 设备侧 TX/RX FIFO 各 1024 字节，且 UART 无 RTS/CTS 流控 —— 一次性灌超过这个量
# 必然溢出丢数据（那是硬件的物理限制，不是桥的缺陷）。所以超过此值就分块往返。
FIFO_SAFE = 512


def _write_all(ser: serial.Serial, data: bytes, deadline: float) -> int:
    """写完整块，返回实际写出的字节数。"""
    sent = 0
    while sent < len(data) and time.time() < deadline:
        n = ser.write(data[sent:])
        if not n:
            time.sleep(0.001)
            continue
        sent += n
    ser.flush()
    return sent


def _read_exact(ser: serial.Serial, want: int, deadline: float) -> bytes:
    got = b""
    while len(got) < want and time.time() < deadline:
        d = ser.read(want - len(got))
        if not d:
            break
        got += d
    return got


def run_case(ser: serial.Serial, name: str, payload: bytes, chunk: int) -> bool:
    ser.reset_input_buffer()
    ser.reset_output_buffer()
    time.sleep(0.05)

    per_round = 5.0 + len(payload) / 5000.0  # 按 UART 线速率给足超时
    t0 = time.time()
    deadline = t0 + per_round

    if len(payload) <= FIFO_SAFE:
        # 小载荷：一把写完再读
        _write_all(ser, payload, deadline)
        got = _read_exact(ser, len(payload), deadline)
    else:
        # 大载荷：分块往返，避免撑爆 FIFO
        block = min(chunk, FIFO_SAFE)
        got = b""
        for i in range(0, len(payload), block):
            blk = payload[i : i + block]
            if _write_all(ser, blk, deadline) != len(blk):
                break
            part = _read_exact(ser, len(blk), deadline)
            if len(part) != len(blk):
                got += part
                break
            got += part
            deadline = time.time() + per_round  # 每块成功后续期
    dt = time.time() - t0

    ok = got == payload
    mark = "OK  " if ok else "FAIL"
    print(f"  {name:12s} 发{len(payload):6d} 收{len(got):6d}  {mark} {dt * 1000:8.1f} ms")
    if not ok:
        n = min(len(got), len(payload))
        bad = next((i for i in range(n) if got[i] != payload[i]), n)
        exp = payload[bad] if bad < len(payload) else None
        act = got[bad] if bad < len(got) else None
        print(f"       首个不同 byte[{bad}]: 期望 {exp} 实收 {act}")
        print(f"       期望前32: {payload[:32].hex(' ')}")
        print(f"       实收前32: {got[:32].hex(' ')}")
    return ok


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", help="COM 口，缺省自动探测")
    ap.add_argument("--baud", type=int, default=115200, help="CDC 行编码里的 UART 波特率")
    ap.add_argument("--repeat", type=int, default=1, help="每个用例重复次数")
    args = ap.parse_args()

    port = args.port or find_dap_port()
    if not port:
        print("找不到 DAP 的 CDC 串口。插好了吗？")
        return 1
    print(f"端口 {port}，UART 波特率 {args.baud}")

    try:
        ser = serial.Serial(port, args.baud, timeout=1.0, write_timeout=2.0)
    except Exception as e:
        print(f"打开失败: {e}")
        return 1

    time.sleep(0.3)  # 等 CDC 行编码下发
    try:
        print("=== CDC <-> USART4 回环 ===")
        results = []
        for _ in range(args.repeat):
            for name, payload, chunk in case_payloads():
                results.append(run_case(ser, name, payload, chunk))
        npass = sum(results)
        print(f"=== {npass}/{len(results)} 通过 ===")
        return 0 if npass == len(results) else 2
    finally:
        ser.close()


if __name__ == "__main__":
    raise SystemExit(main())
