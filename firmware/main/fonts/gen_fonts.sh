#!/usr/bin/env bash
#
# gen_fonts.sh — M3 CJK 位图字体生成
#
# 流程：
#   1. extract_symbols.py 提取符号集 -> symbols.txt (全量 GB2312，16px 用)
#      与 symbols_lg.txt (仅 UI 源码扫描，20px 用)
#   2. lv_font_conv (npx) 从 Arial Unicode.ttf 生成 16px / 20px 两份
#      4bpp 位图字体：font_cjk_16.c / font_cjk_20.c
#
# 20px 字体只渲染静态 UI 文案（标题/大数字），用小符号集；
# 6885 字的 20px 位图约 8MB C 源码，gcc 编译极慢且易内存爆。
# 运行时中文数据（服务器描述）全部走 16px 正文字体。
#
# 对应 UI 字体规格（见 src/main.c）：FONT_SIZE_SM=16, FONT_SIZE_LG=20
#
# 前置条件：
#   - node + npx（lv_font_conv 经 npx 下载运行）
#   - python3
#   - /System/Library/Fonts/Supplemental/Arial Unicode.ttf（macOS 自带）
#
# 用法：
#   cd firmware/main/fonts && bash gen_fonts.sh
#
# 生成后无需改动 CMakeLists：两份 .c 已在 main 组件 SRCS 中登记。
# 若 UI 源码新增文案，重新运行本脚本即可刷新符号集与字体。

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

FONT_TTF="/System/Library/Fonts/Supplemental/Arial Unicode.ttf"

command -v node >/dev/null || { echo "[fonts] ERROR: node 不可用，请先安装 node"; exit 1; }
[ -f "$FONT_TTF" ] || { echo "[fonts] ERROR: 找不到 $FONT_TTF"; exit 1; }

echo "[fonts] node $(node --version)"
echo "[fonts] step 1/2: 提取符号集"
python3 extract_symbols.py

SYMBOLS="$(cat symbols.txt)"
SYMBOLS_LG="$(cat symbols_lg.txt)"
echo "[fonts] step 2/2: 生成位图字体 (4bpp, --no-compress)"

for SIZE in 16 20; do
    if [ "$SIZE" = 16 ]; then SYM="$SYMBOLS"; else SYM="$SYMBOLS_LG"; fi
    echo "[fonts]   -> font_cjk_${SIZE}.c (${SIZE}px, $(echo "$SYM" | wc -c | tr -d ' ') chars)"
    npx --yes lv_font_conv \
        --font "$FONT_TTF" \
        --size "$SIZE" \
        --bpp 4 \
        --format lvgl \
        --no-compress \
        --lv-include "lvgl.h" \
        --symbols "$SYM" \
        -o "font_cjk_${SIZE}.c"
done

echo "[fonts] done:"
ls -l font_cjk_16.c font_cjk_20.c
