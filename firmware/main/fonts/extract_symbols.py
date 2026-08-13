#!/usr/bin/env python3
"""
extract_symbols.py — M3 字体工具链第一步

符号集 = 三部分并集：
  1. UI 源码 (src/ui/*, firmware/main/business/ui/*) 中出现的非 ASCII 字符
  2. ASCII 可打印字符 + 中文标点 + 单位/排版符号兜底集
  3. GB2312 全量汉字（6763 字）——服务器监控页的 desc/name 是运行时
     数据，任意汉字都可能出现，只有整集内置才能避免“豆腐块”。

输出两份：
  symbols.txt    = 1+2+3，供 16px 正文字体（渲染运行时数据）
  symbols_lg.txt = 1+2，  供 20px 大字体（只渲染静态 UI 文案，
                   6885 字的 20px 位图会让 gcc 编译巨慢/内存爆）

去重排序后写入 symbols.txt（单行 UTF-8，供 lv_font_conv 的 --symbols
参数使用）。

用法（由 gen_fonts.sh 调用，也可单独运行）：
    python3 extract_symbols.py

输出：与脚本同目录下的 symbols.txt
"""

import glob
import os

HERE = os.path.dirname(os.path.abspath(__file__))
# firmware/main/fonts/ -> 仓库根
REPO_ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
# 旧模拟器 UI 目录（如仍存在则一并扫描）+ 现固件 UI 目录
UI_DIRS = [
    os.path.join(REPO_ROOT, "src", "ui"),
    os.path.join(REPO_ROOT, "firmware", "main", "business", "ui"),
]
OUT_PATH = os.path.join(HERE, "symbols.txt")
OUT_PATH_LG = os.path.join(HERE, "symbols_lg.txt")


def collect_source_chars():
    """所有出现在 UI 源码中的非 ASCII 字符（含注释里的中文）。"""
    chars = set()
    for ui_dir in UI_DIRS:
        for pattern in ("*.c", "*.h"):
            for path in sorted(glob.glob(os.path.join(ui_dir, pattern))):
                with open(path, "r", encoding="utf-8") as f:
                    text = f.read()
                chars.update(ch for ch in text if ord(ch) > 127)
    return chars


def collect_fixed_chars():
    """ASCII 可打印字符 + 中文标点 + 单位/排版符号兜底集。"""
    chars = set(chr(c) for c in range(0x20, 0x7F))  # 空格 ~ ~
    chars.update("，。！？：；、·…—“”‘’（）【】《》〈〉℃°％")
    # WiFi 信号强度条（源码中以 \xe2\x96\x87 转义书写，扫描不到）
    chars.update("▇")
    return chars


def collect_gb2312_hanzi():
    """GB2312 全量汉字（一级 3755 + 二级 3008 = 6763）。

    用 Python 的 gb2312 编解码器精确枚举：CJK 统一表意文字区
    (U+4E00-U+9FFF) 中能成功 encode('gb2312') 的即为该集成员。
    运行时数据（服务器描述文案）的汉字覆盖靠它兜底。"""
    chars = set()
    for cp in range(0x4E00, 0x9FFF + 1):
        ch = chr(cp)
        try:
            ch.encode("gb2312")
            chars.add(ch)
        except UnicodeEncodeError:
            pass
    return chars


def main():
    base = collect_source_chars() | collect_fixed_chars()
    full = base | collect_gb2312_hanzi()
    # 按码点排序，输出稳定，便于 diff 审查
    for chars, path in ((full, OUT_PATH), (base, OUT_PATH_LG)):
        symbols = "".join(sorted(chars))
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(symbols)
        print(f"[fonts] {len(symbols)} unique symbols -> {path}")
    for ui_dir in UI_DIRS:
        print(f"[fonts]   source dir : {ui_dir}")


if __name__ == "__main__":
    main()
