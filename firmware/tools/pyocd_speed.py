#!/usr/bin/env python3
# 用 pyocd 测 SWD 内存读写速度（与 tools/speed.ps1 的 OpenOCD 测法对齐，可直接比数）
#
# 为什么要有这个工具：实测同一块板、同一份固件，pyocd 比 OpenOCD / probe-rs 都快
# 得多，「命令间空转」几乎为零，所以它更接近固件的真实天花板 —— 拿它当基准才能
# 看出固件还剩多少余量：
#
#     pyocd   0.45.1   写 252.2 / 读 202.9 KiB/s   （15.49 µs/字，贴着线上极限）
#     probe-rs 0.32.0  写 155.1 / 读 140.0 KiB/s   （25.19 µs/字）
#     OpenOCD 0.12.0   写 139.0 / 读 129.0 KiB/s   （28.11 µs/字）
#
# 用法:
#   python tools/pyocd_speed.py                       # 默认 6000 kHz / 64 KiB / 5 轮
#   python tools/pyocd_speed.py --frequency 12000     # 与 6000 同速（见下）
#   python tools/pyocd_speed.py --bytes 32768 --rounds 10
#   python tools/pyocd_speed.py --target stm32f411ce --probe 923CABCDFB83BD7AFFFFFFFF
#
# 注意:
#   - 默认 6000 kHz 就是推荐档位：快路径的实际 SWCLK 由固定指令数决定（约 6 MHz），
#     填 6 MHz 和填 12 MHz 拿到同一个速度，实测两组数字完全一致。
#   - pyocd 的内置目标表**没有 stm32f411**，首次使用要先装 Keil pack：
#         pyocd pack -i stm32f411
#     否则报 Target type stm32f411ce not recognized。
#   - 被测区间会先备份、测完恢复，所以不会破坏目标正在运行的程序。
#   - 被测地址必须是可写 RAM；STM32F411 的 SRAM 只有 128 KiB (0x20000000..0x20020000)。
#   - TEST_ADDR 与速度测试 tcl（tools/openocd_speed_test.tcl）保持一致，便于横向比数。
#   - pyocd 单字读要轮询 AP 状态，固定成本高达约 228 µs；本工具只测大批量传输。

from __future__ import annotations

import argparse
import logging
import sys
import time

import numpy as np

# Windows 中文环境下 Python 默认按 GBK 写 stdout，而 PowerShell 的
# [Console]::OutputEncoding 是 UTF-8，中文会变成乱码。显式改成 UTF-8 对齐。
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

TEST_ADDR = 0x20000000
PATTERN_BASE = 0xA5A50000
FAST_MIN_KHZ = 6000


def parse_address(text: str) -> int:
    try:
        return int(text, 0)
    except ValueError:
        raise argparse.ArgumentTypeError(f"地址需为 0x 开头的十六进制（当前 {text}）")


def main() -> int:
    ap = argparse.ArgumentParser(
        description="用 pyocd 测 SWD 内存读写速度",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    ap.add_argument("--probe", default=None, help="探针 UID，留空则自动选第一个")
    ap.add_argument("--target", default="stm32f411ce", help="pyocd 目标名")
    ap.add_argument("--frequency", type=int, default=6000 * 1000, help="SWCLK，单位 Hz")
    ap.add_argument("--address", type=parse_address, default=TEST_ADDR, help="测试起始地址")
    ap.add_argument("--bytes", type=int, default=65536, help="测试块大小")
    ap.add_argument("--rounds", type=int, default=5, help="计时轮数")
    ap.add_argument("--warmup", type=int, default=1, help="预热轮数（不计入）")
    a = ap.parse_args()

    if a.bytes <= 0 or a.bytes % 4:
        print("错误：--bytes 必须是 4 的正倍数", file=sys.stderr)
        return 2
    if a.rounds < 1:
        print("错误：--rounds 必须大于 0", file=sys.stderr)
        return 2

    try:
        from pyocd.core.helpers import ConnectHelper
        from pyocd.core.exceptions import ProbeError, TargetSupportError
    except ImportError:
        print("错误：未安装 pyocd。请先 pip install pyocd", file=sys.stderr)
        return 2

    # pyocd 会去猜「板子 ID」并抱怨 "Board ID 923C is not recognized"（我们按目标
    # 连接、根本不认板子），这条警告与测速无关，压掉避免和结果混在一起。
    logging.getLogger("pyocd").setLevel(logging.ERROR)

    nwords = a.bytes // 4
    # 变化的 pattern，能暴露地址线/数据线错误与突发顺序错误
    pattern = np.array([PATTERN_BASE | i for i in range(nwords)], dtype=np.uint32)
    words = pattern.tolist()

    khz = a.frequency / 1000.0
    print()
    print(f"探针     : {a.probe or '(自动)'}")
    print(f"目标     : {a.target}")
    print(f"SWCLK    : {khz:.0f} kHz"
          f"{'  (快路径)' if khz >= FAST_MIN_KHZ else '  (慢路径)'}")
    print(f"测试块   : {a.bytes} 字节 ({a.bytes / 1024:.1f} KiB) @ 0x{a.address:08X}")
    print(f"轮数     : {a.rounds}（预热 {a.warmup} 轮，不计入）")
    print()

    try:
        session = ConnectHelper.session_with_chosen_probe(
            unique_id=a.probe,
            target_override=a.target,
            frequency=a.frequency,
            options={"resume_on_disconnect": False},
            blocking=False,
        )
    except TargetSupportError:
        print(f"错误：pyocd 不认识目标 {a.target}。", file=sys.stderr)
        print("      stm32f411 需要先装 Keil pack：pyocd pack -i stm32f411", file=sys.stderr)
        return 2
    except (ProbeError, SystemExit) as exc:
        # 探针 UID 不匹配时 pyocd 自己会 sys.exit(2)，所以这里也要接住 SystemExit，
        # 才能统一给出同一条可操作的提示。
        detail = getattr(exc, "code", exc)
        print(f"错误：无法打开探针（{detail}）。", file=sys.stderr)
        print("      用 pyocd list 确认已插入且 UID 正确。", file=sys.stderr)
        return 2

    if session is None:
        print("错误：没找到可用的探针。", file=sys.stderr)
        print("      用 pyocd list 确认已插入且 UID 正确。", file=sys.stderr)
        return 2

    with session:
        target = session.target
        target.halt()

        # 备份被测区间，测完恢复，别破坏目标正在跑的程序
        saved = target.read_memory_block32(a.address, nwords)

        for _ in range(a.warmup):
            target.write_memory_block32(a.address, words)
            target.read_memory_block32(a.address, nwords)

        write_us: list[float] = []
        read_us: list[float] = []
        passed = 0

        for r in range(a.rounds):
            t0 = time.perf_counter()
            target.write_memory_block32(a.address, words)
            write_us.append((time.perf_counter() - t0) * 1e6)

            t0 = time.perf_counter()
            got = target.read_memory_block32(a.address, nwords)
            read_us.append((time.perf_counter() - t0) * 1e6)

            if np.array_equal(np.asarray(got, dtype=np.uint32), pattern):
                passed += 1
            else:
                print(f"第 {r + 1} 轮数据不一致", file=sys.stderr)

        target.write_memory_block32(a.address, saved)
        target.resume()

    def kibs(samples: list[float]) -> float:
        return a.bytes * len(samples) * 1e6 / (sum(samples) * 1024.0)

    def us_per_word(samples: list[float]) -> float:
        return sum(samples) / len(samples) / nwords

    wr, rd = kibs(write_us), kibs(read_us)
    print('=' * 58)
    print("  {0:<10} {1:>16} {2:>16}".format("SWCLK", "写 KiB/s", "读 KiB/s"))
    print('-' * 58)
    print("  {0:<10} {1:>16.3f} {2:>16.3f}".format(f"{khz:.0f}kHz", wr, rd))
    print('-' * 58)
    print(f"  通过轮数: {passed} / {a.rounds}")
    print(f"  每字耗时: 写 {us_per_word(write_us):.2f} µs / 读 {us_per_word(read_us):.2f} µs")
    print('=' * 58)
    print()

    if a.rounds == 1:
        print("提示：单次测量有约 ±5% 波动，需要更稳用 --rounds 10")
        print()

    return 0 if passed == a.rounds else 1


if __name__ == "__main__":
    sys.exit(main())
