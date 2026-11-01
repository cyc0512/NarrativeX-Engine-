#!/usr/bin/env bash
#
# scripts/build_asan.sh —— AddressSanitizer 构建入口（T03 取证用，不参与生产构建）
#
# 目的：在**不改动** CMakeLists.txt（那是 T02 并行改动区）的前提下，
#       用 -fsanitize=address -fno-omit-frame-pointer -g 编译出 ASan 版本的可执行文件，
#       拿到 heap-use-after-free 的完整调用栈。
#
# 用法：
#   bash scripts/build_asan.sh                      # 默认：构建 fs_terminal_asan（绕开 Qt 线程干扰）
#   bash scripts/build_asan.sh gui                  # 构建 fs_asan（Qt GUI + ASan，需 QT_QPA_PLATFORM=offscreen）
#   bash scripts/build_asan.sh src  <x.cpp> <out>   # ASan 编译任意单文件源文件
#   bash scripts/build_asan.sh plain <x.cpp> <out>  # 不加 ASan，仅 -g -O1（跑正常回归用）
#
# 输出统一落在 $ROOT/build/ 下。
set -euo pipefail

CXX="${CXX:-g++}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTDIR="${OUTDIR:-$ROOT/build}"
mkdir -p "$OUTDIR"

# ASan 三件套：地址消毒 + 保留帧指针（栈要能回溯到真实调用者）+ 调试符号。
# -O1 而不是 -O0：O0 下 Qt 模板内联展开会让栈变得极长且难读，O1 可读性最好。
ASAN_CXXFLAGS="-fsanitize=address -fno-omit-frame-pointer -fno-optimize-sibling-calls -g -O1"
ASAN_LDFLAGS="-fsanitize=address"

QT_PREFIX="${QT_PREFIX:-/usr/include/x86_64-linux-gnu/qt5}"
QT_INC="-I$QT_PREFIX -I$QT_PREFIX/QtCore -I$QT_PREFIX/QtGui -I$QT_PREFIX/QtWidgets -I$QT_PREFIX/QtConcurrent"
QT_DEFS="-DQT_CORE_LIB -DQT_GUI_LIB -DQT_WIDGETS_LIB -DQT_CONCURRENT_LIB"
QT_LIBS="-lQt5Widgets -lQt5Concurrent -lQt5Gui -lQt5Core"

COMMON_INC="-I$ROOT/include"
COMMON_LIBS="-lsqlite3 -lcurl -lpthread"

compile_one() {
    # $1 = 源文件, $2 = 输出可执行文件, $3 = 额外编译参数
    local src="$1" out="$2" extra="$3"
    echo "[build_asan] $src -> $out"
    echo "[build_asan]   extra flags: $extra"
    "$CXX" -std=gnu++17 -fPIC $COMMON_INC $extra "$src" -o "$out" $COMMON_LIBS
}

case "${1:-terminal}" in
    terminal)
        # 默认目标：终端版 + ASan。先用它绕开 Qt 的线程 / 事件循环干扰，
        # 单独验证引擎侧（D-5 SQL 注入、D-6 数据竞争）有没有爆。
        compile_one "$ROOT/src/main.cpp" "$OUTDIR/fs_terminal_asan" \
            "$ASAN_CXXFLAGS $ASAN_LDFLAGS"
        ;;
    gui)
        # Qt GUI 版 + ASan：用来抓 D-4（5 秒超时回调里的悬空 QFutureWatcher）。
        compile_one "$ROOT/src/qt_main.cpp" "$OUTDIR/fs_asan" \
            "$ASAN_CXXFLAGS $ASAN_LDFLAGS $QT_INC $QT_DEFS $QT_LIBS"
        ;;
    src)
        # ASan 编译任意单文件（取证用的最小复现程序走这条路）。
        if [[ $# -lt 3 ]]; then echo "usage: $0 src <x.cpp> <out> [extra...]" >&2; exit 2; fi
        extra="$ASAN_CXXFLAGS $ASAN_LDFLAGS"
        if [[ $# -ge 4 ]]; then
            if [[ "$4" == "qt" ]]; then extra="$extra $QT_INC $QT_DEFS $QT_LIBS"; fi
        fi
        compile_one "$2" "$OUTDIR/$3" "$extra"
        ;;
    pilot)
        # GUI 自动驾驶插件：LD_PRELOAD 覆盖 QCoreApplication::exec()，
        # 用来在无 X 服务的机器上驱动**真实的 fs 二进制**。必须编译成 .so 且不带 ASan。
        echo "[build_asan] scripts/ui_autopilot.cpp -> $OUTDIR/libfs_autopilot.so"
        "$CXX" -std=gnu++17 -fPIC -shared $QT_INC $QT_DEFS scripts/ui_autopilot.cpp \
            -o "$OUTDIR/libfs_autopilot.so" $QT_LIBS -ldl
        ;;
    plain)
        # 不加 ASan，只带 -g：跑"20 次自由输入 0 崩溃"这类回归时用，
        # 避免 ASan 的 LeakSanitizer 把 Qt 自身的内存泄漏报成失败。
        if [[ $# -lt 3 ]]; then echo "usage: $0 plain <x.cpp> <out> [qt]" >&2; exit 2; fi
        extra="-fno-omit-frame-pointer -g -O1"
        if [[ $# -ge 4 && "$4" == "qt" ]]; then extra="$extra $QT_INC $QT_DEFS $QT_LIBS"; fi
        compile_one "$2" "$OUTDIR/$3" "$extra"
        ;;
    *)
        echo "unknown target: $1" >&2
        echo "usage: $0 [terminal|gui|src <x.cpp> <out>|plain <x.cpp> <out>]" >&2
        exit 2
        ;;
esac

echo "[build_asan] OK"
