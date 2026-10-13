#!/usr/bin/env python3
# 把 doc/ 下的分节文档合并成仓库根目录的 readme.md。
#
# 为什么这样组织：readme.md 有 2000+ 行，单文件不利于分工编写（人和 AI 都一样）。
# 所以正文按小节拆到 doc/ 下，**doc/ 是唯一事实来源，readme.md 是生成产物**。
# 改文档 = 改 doc/ 下对应的小文件，然后跑一次本脚本。
#
# 合并规则（刻意保持极简，避免「哪个文件要补分隔线」这类隐性约定）：
#   1. 只收**文件名以两位数字开头**的 .md —— 形如 `NN-slug.md` 或 `NN-MM-slug.md`
#      （如 02-hardware.md、18-05-ram-flash-speed.md）。按文件名排序即正文顺序，
#      所以**文件名的数字前缀决定章节次序**。本目录的 README.md、以及本脚本，
#      都不匹配该模式，不会参与合并。
#   2. 每两个文件之间插入一条水平分隔线（`\n\n---\n\n`）。
#   3. 文件内部的 `---` 原样保留（某一节自己再分子块时用得到）。
#   4. 输出末尾补一个换行；换行符沿用 doc/ 里文件的风格（当前 CRLF）。
#
# 用法:
#   python doc/build_readme.py            # 生成 / 覆盖 readme.md
#   python doc/build_readme.py --check    # 只校验 readme.md 是否与 doc/ 同步（CI 用）
#   python doc/build_readme.py --list     # 列出参与合并的文件与其行数
#
# 约定:
#   - 每个文件以其标题行（如 `## 5. 快速开始`）开头；属于「第 N 部分」的**第一个**
#     文件，把该部分的一级标题（`# 第N部分：…`）也放在文件最前面，这样合并后
#     部分标题与它的第一节之间不会多出一条 `---`。
#   - 章节编号由文件内容自己声明（脚本不维护编号），故插入/删除小节后，需要手工把
#     后续标题的编号顺移 —— 脚本只负责拼接，**不修改任何内容**。
#   - 文本里的交叉引用一律用**标题**（如「见『撤除波特率上限之后』」）而不是章节号，
#     这样重排/插入小节后引用不会失效。
#   - 脚本只读 doc/、只写 readme.md。

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# Windows 中文环境下 Python 默认按 GBK 写 stdout，而 PowerShell 的
# [Console]::OutputEncoding 是 UTF-8，中文（以及 ↔ 这类符号）会乱码甚至抛
# UnicodeEncodeError。显式改成 UTF-8 对齐。
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

DOC_DIR = Path(__file__).resolve().parent
REPO_ROOT = DOC_DIR.parent
OUT_FILE = REPO_ROOT / "readme.md"

# 参与合并的文件名：两位数字 + 可选的两级数字 + 短名
PART_RE = re.compile(r"^\d{2}(-\d{2})?-.+\.md$")
SEPARATOR = "\n\n---\n\n"


def collect() -> list[Path]:
    if not DOC_DIR.is_dir():
        sys.exit(f"错误：找不到 {DOC_DIR}")
    files = sorted(p for p in DOC_DIR.iterdir() if PART_RE.match(p.name))
    if not files:
        sys.exit(f"错误：{DOC_DIR} 下没有符合「NN-*.md」的文件")
    return files


def read_normalized(p: Path) -> str:
    """读成 LF 文本（去掉 BOM、统一行尾），便于拼接与比较。"""
    text = p.read_bytes().decode("utf-8-sig")
    return text.replace("\r\n", "\n").replace("\r", "\n")


def detect_eol(files: list[Path]) -> str:
    """沿用第一个文件的换行风格。"""
    return "\r\n" if b"\r\n" in files[0].read_bytes() else "\n"


def build(files: list[Path]) -> tuple[str, str]:
    chunks = []
    for p in files:
        body = read_normalized(p).strip("\n")
        if not body:
            sys.exit(f"错误：{p.name} 是空文件")
        chunks.append(body)
    return SEPARATOR.join(chunks) + "\n", detect_eol(files)


def main() -> int:
    ap = argparse.ArgumentParser(description="把 doc/ 下的分节文档合并成 readme.md")
    ap.add_argument("--check", action="store_true",
                    help="只校验 readme.md 是否与 doc/ 同步，不写入")
    ap.add_argument("--list", action="store_true", help="列出参与合并的文件")
    a = ap.parse_args()

    files = collect()
    text, eol = build(files)

    if a.list:
        total = 0
        for p in files:
            lines = read_normalized(p).split("\n")
            total += len(lines)
            print("%-34s %5d 行   %s" % (p.name, len(lines), lines[0][:56]))
        print("-" * 78)
        print("%-34s %5d 行   (%d 个文件)" % ("合计", total, len(files)))
        return 0

    if a.check:
        current = read_normalized(OUT_FILE) if OUT_FILE.exists() else ""
        if current == text:
            print(f"readme.md 与 doc/ 同步（{len(files)} 个文件，"
                  f"{len(text.splitlines())} 行）")
            return 0
        print("readme.md 与 doc/ **不一致**，请跑一次 doc/build_readme.py",
              file=sys.stderr)
        return 1

    out_text = text.replace("\n", eol) if eol != "\n" else text
    OUT_FILE.write_bytes(out_text.encode("utf-8"))
    print(f"已生成 {OUT_FILE.name}：{len(files)} 个文件 -> "
          f"{len(text.splitlines())} 行")
    return 0


if __name__ == "__main__":
    sys.exit(main())
