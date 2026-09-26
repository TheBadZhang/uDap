#!/usr/bin/env python3
"""CDC 环回矩阵扫描：波特率 × 发送块大小（可选块间空隙）。

回答的问题
----------
撤掉波特率上限后，哪些波特率真的可用？在哪些「发送粒度」下可用？
持续满载（sustained）会丢字节，那改成小载荷 / 分块发送能救回多少？

扫描结果是一张矩阵：行 = 波特率，列 = 块大小，格子里是
「OK/FAIL + 等效速率」。这样一眼能看出每种波特率的**最小可用发送粒度**。

前提：DAP 的 PB0(TX) 与 PB1(RX) 已短接（环回跳线）。

用法
----
    # 默认矩阵：2M~6M × 64B~8KB
    python tools/com_loopback_matrix.py

    # 指定波特率与块大小
    python tools/com_loopback_matrix.py --baud 2000000,3000000 --chunk 256,1024,4096

    # 加块间空隙（默认 0 = 收到就立刻发下一块）
    python tools/com_loopback_matrix.py --gap-ms 5

    # 每格重复两次，两行分别显示（看稳定性）
    python tools/com_loopback_matrix.py --repeats 2

关于判定
--------
每格发送 `--total` 字节，收满且逐字节相符才算 OK。
注意：一旦中途丢了字节，「已收字节数」就永远追不上目标值，会在超时后判 FAIL ——
所以 FAIL 的实际含义是「这一段里至少丢过 1 字节」。
"""

import argparse
import sys
import time

import serial

from com_loopback_paced import (BITS_PER_BYTE, USB_FS_BULK_BYTES_PER_S,
                                actual_baud, find_dap_port, payload_bytes,
                                sdk_brr)

DEFAULT_BAUDS = [2_000_000, 2_500_000, 3_000_000, 3_500_000, 4_000_000, 6_000_000]
DEFAULT_CHUNKS = [64, 256, 512, 1024, 2048, 4096, 8192]


def run_case(port: str, baud: int, total: int, chunk: int,
             gap_s: float, fail_timeout: float) -> dict:
    """跑一格：分块发送，每块收齐再发下一块。"""
    res = {"baud": baud, "chunk": chunk, "total": total, "ok": False,
           "recv": 0, "seconds": None, "eff_bps": None, "util": None,
           "note": ""}

    try:
        ser = serial.Serial(port, baudrate=baud, timeout=0.2,
                            write_timeout=fail_timeout + 2.0,
                            rtscts=False, dsrdtr=False, xonxoff=False)
    except Exception as exc:  # noqa: BLE001
        res["note"] = f"打开失败：{exc}"
        return res

    try:
        time.sleep(0.15)
        ser.reset_input_buffer()
        ser.reset_output_buffer()
        time.sleep(0.02)

        data = payload_bytes(total)
        received = bytearray()
        sent = 0
        t_start = time.perf_counter()
        gap_total = 0.0

        while sent < total:
            blk = data[sent : sent + min(chunk, total - sent)]
            try:
                ser.write(blk)
            except Exception as exc:  # noqa: BLE001
                res["note"] = f"写失败：{exc}"
                break
            sent += len(blk)

            want = sent
            # 这一块的等待预算：按请求波特率算它需要多久，再给足余量。
            # 用固定上限而不是按 total 放大 —— 失败时要**快速**失败，
            # 否则一张矩阵要跑几十分钟。
            budget = min(chunk, total) * BITS_PER_BYTE / baud * 4.0 + fail_timeout
            deadline = time.perf_counter() + budget
            while len(received) < want:
                if time.perf_counter() > deadline:
                    res["note"] = f"超时：只收到 {len(received)}/{want} 字节"
                    break
                got = ser.read(min(65536, want - len(received)))
                if got:
                    received += got
                    deadline = time.perf_counter() + budget
            if res["note"]:
                break

            if sent < total and gap_s > 0:
                time.sleep(gap_s)
                gap_total += gap_s

        elapsed = time.perf_counter() - t_start
        res["recv"] = len(received)
        res["seconds"] = elapsed
        transfer = max(elapsed - gap_total, 0.0)
        if len(received) and transfer > 0:
            res["eff_bps"] = len(received) / transfer
            res["util"] = res["eff_bps"] / (baud / BITS_PER_BYTE) * 100.0

        if not res["note"] and bytes(received) != data:
            n = min(len(received), total)
            bad = next((i for i in range(n) if received[i] != data[i]), n)
            res["note"] = f"数据不符（首个不同位置 {bad}）"
        res["ok"] = not res["note"] and res["recv"] == total
    finally:
        ser.close()

    return res


def cell(res: dict, show_rate: bool) -> str:
    if res["ok"]:
        if show_rate and res["eff_bps"]:
            return f"OK {res['eff_bps'] / 1024:.0f}"
        return "OK"
    return "FAIL"


def main() -> int:
    ap = argparse.ArgumentParser(
        description="CDC 环回矩阵：波特率 × 块大小（需 PB0-PB1 环回跳线）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--port", help="COM 口，默认按 VID:PID 自动查找")
    ap.add_argument("--baud", default=",".join(str(b) for b in DEFAULT_BAUDS))
    ap.add_argument("--chunk", default=",".join(str(c) for c in DEFAULT_CHUNKS))
    ap.add_argument("--gap-ms", type=float, default=0.0, help="块间空隙（默认 0）")
    ap.add_argument("--total", type=int, default=32768, help="每格总字节数（默认 %(default)s）")
    ap.add_argument("--fail-timeout", type=float, default=2.0,
                    help="每块额外的等待余量，决定失败场景多久放弃（默认 %(default)s s）")
    ap.add_argument("--repeats", type=int, default=1, help="每格重复次数")
    ap.add_argument("--csv", help="结果写入 CSV")
    args = ap.parse_args()

    port = args.port or find_dap_port()
    if not port:
        print("找不到 DAP 的 CDC 口，请用 --port 指定", file=sys.stderr)
        return 1

    bauds = [int(x) for x in args.baud.split(",") if x.strip()]
    chunks = [int(x) for x in args.chunk.split(",") if x.strip()]

    print()
    print(f"端口     : {port}")
    print(f"每格总量 : {args.total} B，块间空隙 {args.gap_ms:g} ms，"
          f"每格重复 {args.repeats} 次")
    print(f"失败预算 : 每块多等 {args.fail_timeout:g} s 后放弃")
    print()

    results: dict[tuple[int, int, int], dict] = {}

    for baud in bauds:
        act = actual_baud(baud)
        print(f"  {baud:>9} baud  BRR={sdk_brr(baud)}  实际≈{act:.0f} baud")
        for chunk in chunks:
            for r in range(args.repeats):
                res = run_case(port, baud, args.total, chunk,
                               args.gap_ms / 1000.0, args.fail_timeout)
                results[(baud, chunk, r)] = res
                mark = "OK  " if res["ok"] else "FAIL"
                rate = (f"{res['eff_bps'] / 1024:7.1f} KiB/s "
                        f"({res['util']:5.1f}%)" if res["eff_bps"] else
                        " " * 24)
                note = f"  {res['note']}" if res["note"] else ""
                tag = f" #{r + 1}" if args.repeats > 1 else ""
                print(f"    块 {chunk:>5} B  {mark}  收 {res['recv']:>6}/{args.total} B  "
                      f"{rate}{note}{tag}")

    # ---------- 矩阵 ----------
    print()
    show_rate = args.repeats == 1
    header = "  ".join(f"{c:>8}" for c in chunks)
    width = 12 + len(header)
    print("=" * width)
    print(f"  {'波特率':>9}  {header}   （单位 KiB/s，OK/FAIL）")
    print("-" * width)
    for baud in bauds:
        cells = []
        for chunk in chunks:
            rs = [results[(baud, chunk, r)] for r in range(args.repeats)]
            if len(rs) == 1:
                cells.append(f"{cell(rs[0], show_rate):>8}")
            else:
                # 多次重复：显示通过次数
                passed = sum(1 for r in rs if r["ok"])
                cells.append(f"{f'{passed}/{len(rs)}':>8}")
        print(f"  {baud:>9}  " + "  ".join(cells))
    print("=" * width)
    print("说明：")
    print("  - 每格：发送 --total 字节，逐字节校验；丢过任何字节就判 FAIL。")
    print("  - 格内数字为**等效速率**（KiB/s）：扣除块间空隙后的纯传输速率。")
    print("    它含每块的 USB 往返开销，所以块越小越低 —— 这是分块发送的代价。")
    print("  - 从左到右块变大：越小越稳但越慢；能通过的最小块 = 该波特率的可用粒度。")
    if args.gap_ms:
        print(f"  - 本表已加 {args.gap_ms:g} ms 块间空隙（默认 0 时靠 USB 往返天然拉开）。")
    print()

    if args.csv:
        import csv
        with open(args.csv, "w", newline="", encoding="utf-8") as fh:
            w = csv.writer(fh)
            w.writerow(["baud", "brr", "actual_baud", "chunk", "run", "total_bytes",
                        "received", "seconds", "eff_Bps", "uart_util_pct",
                        "ok", "note"])
            for baud in bauds:
                for chunk in chunks:
                    for r in range(args.repeats):
                        res = results[(baud, chunk, r)]
                        w.writerow([
                            baud, sdk_brr(baud), f"{actual_baud(baud):.0f}",
                            chunk, r + 1, res["total"], res["recv"],
                            f"{res['seconds']:.6f}" if res["seconds"] else "",
                            f"{res['eff_bps']:.1f}" if res["eff_bps"] else "",
                            f"{res['util']:.2f}" if res["util"] else "",
                            "1" if res["ok"] else "0", res["note"],
                        ])
        print(f"已写入 {args.csv}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
