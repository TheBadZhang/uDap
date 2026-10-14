#!/usr/bin/env python3
"""对照探针的 SWD 吞吐扫描（pyocd / OpenOCD 两个工具都支持）。

用途：给「本板 DAP」以外的第三方 CMSIS-DAP 探针做同样的频率 × 块大小扫描，
结果与 readme 第 18 章（性能实测）里本板的数据可直接对比。

测量方法（与 tools/openocd_speed_test.tcl / tools/pyocd_speed.py 一致）：
  - 测试区：目标 RAM 0x20000000 起；先整块备份，测完逐字恢复，不破坏目标程序
  - 填充模式：0xA5A50000 | i（变化的 pattern，能暴露地址/数据线问题）
  - 每个 (频率, 块大小) 组合跑 N 次，每次分别计时「写整块」与「读整块」
  - 每轮读回逐字校验；失败（ACK 错、超时、数据不符）记 ok=0 并继续
  - 频率用运行时改时钟的方式在同一条会话里扫描，避免反复连接的固定开销

用法:
  # pyocd 扫一遍（默认 1/3/6/9/12/15 MHz × 1/4/16/64 KiB × 5 次）
  python tools/probe_swd_sweep.py --probe 38b30f94aae9ddfa01 \
      --label "FanX Tek DAPLink" --tool both --csv build/sweep_fanx.csv

  # 只跑 OpenOCD、自定义频率与块大小
  python tools/probe_swd_sweep.py --tool openocd --freqs 1000,9000,15000 \
      --sizes 1024,65536 --reps 3

输出：CSV（probe,tool,freq_khz,bytes,rep,write_kib_s,read_kib_s,ok）+ 屏幕汇总。

注意（pyocd 的写计时）：写的时间包含一次「读回最后一个字并校验」的强制冲刷，见
sweep_pyocd 里的说明 —— 否则 pyocd 的写会返回得比数据实际上线还早，把开销记到
下一次读头上，给出物理上不可能的数（如 1 KiB@1 MHz 报 3266 KiB/s）。
"""

from __future__ import annotations

import argparse
import csv
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

# Windows 中文环境下 Python 默认按 GBK 写 stdout，而 PowerShell 的控制台是 UTF-8，
# 中文会乱码甚至抛 UnicodeEncodeError。显式对齐。
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

REPO = Path(__file__).resolve().parent.parent
TEST_ADDR = 0x20000000
PATTERN_BASE = 0xA5A50000
# 测试点：1 / 3 / 6 / 9 / 12 / 15 MHz（6 档）
DEFAULT_FREQS = [1000, 3000, 6000, 9000, 12000, 15000]
DEFAULT_SIZES = [1024, 4096, 16384, 65536]


def find_openocd() -> tuple[str, str] | None:
    """返回 (openocd.exe, scripts 目录)。两种常见布局都认。"""
    cands = []
    root = os.environ.get("OPENOCD_ROOT")
    if root:
        cands.append(Path(root))
    cands += [
        Path(r"C:\Users\lab\scoop\apps\openocd\current"),
        Path(r"C:\MounRiver\MounRiver_Studio2\resources\app\resources\win32"
             r"\components\WCH\OpenOCD\OpenOCD"),
    ]
    for c in cands:
        exe = c / "bin" / "openocd.exe"
        for rel in ("scripts", r"share\openocd\scripts"):
            sc = c / rel
            if exe.is_file() and (sc / "interface").is_dir() and (sc / "target").is_dir():
                return str(exe), str(sc)
    exe = shutil.which("openocd")
    if exe:
        base = Path(exe).resolve().parent.parent
        for rel in ("scripts", r"share\openocd\scripts"):
            sc = base / rel
            if (sc / "interface").is_dir() and (sc / "target").is_dir():
                return str(exe), str(sc)
    return None


# --------------------------------------------------------------------------
# pyocd
# --------------------------------------------------------------------------

def sweep_pyocd(a) -> list[dict]:
    from pyocd.core.helpers import ConnectHelper
    import numpy as np

    rows: list[dict] = []
    kwargs = dict(target_override=a.target, frequency=a.freqs[0] * 1000,
                  options={"resume_on_disconnect": False}, blocking=False)
    if a.probe:
        kwargs["unique_id"] = a.probe
    session = ConnectHelper.session_with_chosen_probe(**kwargs)
    if session is None:
        print("错误：没找到探针", file=sys.stderr)
        return rows

    max_words = max(a.sizes) // 4
    with session:
        t = session.target
        t.halt()
        saved = t.read_memory_block32(TEST_ADDR, max_words)

        for khz in a.freqs:
            session.probe.set_clock(khz * 1000)
            for nbytes in a.sizes:
                nwords = nbytes // 4
                pattern = np.array([PATTERN_BASE | i for i in range(nwords)],
                                   dtype=np.uint32)
                words = pattern.tolist()
                last_addr = TEST_ADDR + nbytes - 4

                # 写：**必须自带一次冲刷**。pyocd 的 write_memory_block32 会把末尾的
                # 传输留在流水线里（尤其是块小的时候），如果写完紧接着读，那笔开销会
                # 记到**下一次读**头上 —— 实测 1 KiB@1 MHz 会给出 3266 KiB/s 这种
                # 物理上不可能的数（1 MHz 的线上限才约 87 KiB/s），而随后那次读只剩
                # 33.5 KiB/s。因此这里在计时区内读回最后一个字（并校验）强制冲刷，
                # 这样每个 rep 的写耗时都是独立的、可比的。
                def measure_write():
                    t0 = time.perf_counter()
                    t.write_memory_block32(TEST_ADDR, words)
                    got = t.read32(last_addr)
                    us = (time.perf_counter() - t0) * 1e6
                    return us, int(got) == int(pattern[-1])

                # 读：同样单独计时，不与写交替
                def measure_read():
                    t0 = time.perf_counter()
                    got = t.read_memory_block32(TEST_ADDR, nwords)
                    us = (time.perf_counter() - t0) * 1e6
                    return us, np.array_equal(np.asarray(got, dtype=np.uint32), pattern)

                for fn in (measure_write, measure_read):
                    try:
                        fn()
                    except Exception:
                        pass
                for rep in range(1, a.reps + 1):
                    try:
                        w, w_ok = measure_write()
                        r, r_ok = measure_read()
                    except Exception as exc:
                        print("  [pyocd %d kHz %d B rep%d] 失败: %s"
                              % (khz, nbytes, rep, exc), file=sys.stderr)
                        rows.append(dict(tool="pyocd", freq_khz=khz, bytes=nbytes,
                                         rep=rep, write_kib_s=0.0, read_kib_s=0.0, ok=0))
                        continue
                    rows.append(dict(
                        tool="pyocd", freq_khz=khz, bytes=nbytes, rep=rep,
                        write_kib_s=nbytes * 1e6 / (w * 1024) if w else 0.0,
                        read_kib_s=nbytes * 1e6 / (r * 1024) if r else 0.0,
                        ok=int(w_ok and r_ok)))
                print("  pyocd %5d kHz %6d B  done" % (khz, nbytes), file=sys.stderr)

        t.write_memory_block32(TEST_ADDR, saved)
        t.resume()
    return rows


# --------------------------------------------------------------------------
# OpenOCD
# --------------------------------------------------------------------------

OCD_TCL = r"""
# Generated by tools/probe_swd_sweep.py -- keep this file pure ASCII:
# OpenOCD's TCL parser is not reliable with non-ASCII source.
# Sweeps frequency x block-size x repeats inside a single session.
set freqs {__FREQS__}
set sizes {__SIZES__}
set reps __REPS__
set addr __ADDR__

# init at the lowest speed (most tolerant to target timing)
adapter speed [lindex $freqs 0]
init
halt

set maxsz [lindex $sizes end]
set maxwords [expr {$maxsz / 4}]

# back up the test region (all block sizes start at the same address)
if {[catch {set saved [read_memory $addr 32 $maxwords]} err]} {
    echo "SWEEP_ERROR backup failed: $err"
    shutdown error
}

foreach khz $freqs {
    adapter speed $khz
    foreach nbytes $sizes {
        set nwords [expr {$nbytes / 4}]
        set pat {}
        for {set i 0} {$i < $nwords} {incr i} {
            lappend pat [format "0x%08x" [expr {0xA5A50000 | ($i & 0xffff)}]]
        }
        # warm up once
        catch {write_memory $addr 32 $pat}
        catch {read_memory $addr 32 $nwords}
        for {set rep 1} {$rep <= $reps} {incr rep} {
            set ok 1
            set t0 [clock microseconds]
            if {[catch {write_memory $addr 32 $pat} err]} {
                set w 0; set ok 0
            } else {
                set w [expr {[clock microseconds] - $t0}]
            }
            set t0 [clock microseconds]
            if {[catch {set got [read_memory $addr 32 $nwords]} err]} {
                set r 0; set ok 0
            } else {
                set r [expr {[clock microseconds] - $t0}]
            }
            # verify word by word
            if {$ok} {
                for {set i 0} {$i < $nwords} {incr i} {
                    if {[expr {[lindex $got $i] != [lindex $pat $i]}]} {
                        set ok 0
                        break
                    }
                }
            }
            echo "SWEEP_ROW $khz $nbytes $rep $w $r $ok"
        }
        echo "SWEEP_PROGRESS $khz $nbytes"
    }
}

catch {write_memory $addr 32 $saved}
echo "SWEEP_DONE"
resume
shutdown
"""


def sweep_openocd(a) -> list[dict]:
    rows: list[dict] = []
    found = find_openocd()
    if not found:
        print("错误：找不到 openocd", file=sys.stderr)
        return rows
    exe, scripts = found

    tcl = (OCD_TCL
           .replace("__FREQS__", " ".join(str(f) for f in a.freqs))
           .replace("__SIZES__", " ".join(str(s) for s in a.sizes))
           .replace("__REPS__", str(a.reps))
           .replace("__ADDR__", hex(TEST_ADDR)))
    tmpdir = Path(tempfile.gettempdir())
    tcl_path = tmpdir / "probe_swd_sweep.tcl"
    tcl_path.write_text(tcl, encoding="ascii")

    cmd = [exe, "-s", scripts,
           "-f", "interface/cmsis-dap.cfg",
           "-f", f"target/{a.target_ocd}.cfg"]
    if a.probe:
        cmd += ["-c", f"adapter serial {a.probe}"]
    # tcl 路径用正斜杠：OpenOCD 的 TCL 对 Windows 反斜杠转义处理不统一
    cmd += ["-c", "source {%s}" % str(tcl_path).replace("\\", "/")]

    print("  openocd:", " ".join(cmd), file=sys.stderr)
    proc = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    out = (proc.stdout or "") + (proc.stderr or "")

    seen = set()
    for line in out.splitlines():
        m = re.search(r"SWEEP_ROW\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)", line)
        if not m:
            continue
        khz, nbytes, rep, wus, rus, ok = (int(x) for x in m.groups())
        rows.append(dict(
            tool="openocd", freq_khz=khz, bytes=nbytes, rep=rep,
            write_kib_s=nbytes * 1e6 / (wus * 1024) if wus else 0.0,
            read_kib_s=nbytes * 1e6 / (rus * 1024) if rus else 0.0,
            ok=ok))
        seen.add((khz, nbytes))

    if not rows:
        print("错误：OpenOCD 没产出任何结果，原始输出尾部：", file=sys.stderr)
        for l in out.splitlines()[-25:]:
            print("   " + l, file=sys.stderr)
    else:
        print("  openocd: 收到 %d 行（%d 个 freq×size 组合）"
              % (len(rows), len(seen)), file=sys.stderr)
    return rows


# --------------------------------------------------------------------------

def summarize(rows: list[dict], label: str, freqs, sizes) -> None:
    for tool in ("pyocd", "openocd"):
        sub = [r for r in rows if r["tool"] == tool]
        if not sub:
            continue
        print("\n=== %s / %s（中位数，KiB/s） ===" % (label, tool))
        print("  %-9s" % "SWCLK" + "".join(
            "%22s" % ("%d KiB 写/读" % (b // 1024)) for b in sizes))
        for khz in freqs:
            cells = []
            for b in sizes:
                g = [r for r in sub if r["freq_khz"] == khz and r["bytes"] == b]
                good = [r for r in g if r["ok"]]
                if not good:
                    cells.append("%22s" % "失败")
                else:
                    w = statistics.median(r["write_kib_s"] for r in good)
                    rd = statistics.median(r["read_kib_s"] for r in good)
                    cells.append("%22s" % ("%.1f / %.1f" % (w, rd)))
            label = "%.1f" % (khz / 1000.0) if khz < 1000 else "%.0f" % (khz / 1000.0)
            print("  %-9s" % (label + " MHz") + "".join(cells))


def main() -> int:
    ap = argparse.ArgumentParser(description="对照探针 SWD 吞吐扫描")
    ap.add_argument("--label", default="unknown", help="写进 CSV 的探针标签")
    ap.add_argument("--probe", default=None, help="探针序列号（OpenOCD adapter serial / pyocd unique_id）")
    ap.add_argument("--tool", choices=["pyocd", "openocd", "both"], default="both")
    ap.add_argument("--freqs", default=",".join(str(f) for f in DEFAULT_FREQS),
                    help="频率列表，kHz，逗号分隔")
    ap.add_argument("--sizes", default=",".join(str(s) for s in DEFAULT_SIZES),
                    help="块大小列表，字节，逗号分隔")
    ap.add_argument("--reps", type=int, default=5, help="每个组合重复次数")
    ap.add_argument("--target", default="stm32f411ce", help="pyocd 目标名")
    ap.add_argument("--target-ocd", default="stm32f4x", help="OpenOCD 目标配置名")
    ap.add_argument("--csv", default=None, help="把逐次结果写到这个 CSV")
    a = ap.parse_args()

    a.freqs = [int(x) for x in a.freqs.split(",") if x.strip()]
    a.sizes = [int(x) for x in a.sizes.split(",") if x.strip()]

    print("探针标签 : %s" % a.label)
    print("序列号   : %s" % (a.probe or "(自动)"))
    print("频率     : %s kHz" % a.freqs)
    print("块大小   : %s B" % a.sizes)
    print("重复     : %d 次/组合" % a.reps)
    print("测试区   : 0x%08X" % TEST_ADDR)

    rows: list[dict] = []
    if a.tool in ("pyocd", "both"):
        print("\n[pyocd] ...", file=sys.stderr)
        rows += sweep_pyocd(a)
    if a.tool in ("openocd", "both"):
        print("\n[openocd] ...", file=sys.stderr)
        rows += sweep_openocd(a)

    if not rows:
        print("没有拿到任何数据", file=sys.stderr)
        return 1

    if a.csv:
        p = Path(a.csv)
        p.parent.mkdir(parents=True, exist_ok=True)
        with p.open("w", newline="", encoding="utf-8") as f:
            w = csv.DictWriter(f, fieldnames=["probe", "tool", "freq_khz", "bytes",
                                              "rep", "write_kib_s", "read_kib_s", "ok"])
            w.writeheader()
            for r in rows:
                w.writerow(dict(probe=a.label, **r))
        print("\nCSV: %s（%d 行）" % (p, len(rows)))

    summarize(rows, a.label, a.freqs, a.sizes)
    return 0


if __name__ == "__main__":
    sys.exit(main())
