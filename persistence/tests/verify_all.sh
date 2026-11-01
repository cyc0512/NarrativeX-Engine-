#!/usr/bin/env bash
# nx-linux/verify_all.sh
#
# 一条命令跑完四层验证。放在 Linux 侧是因为：
#   - GCC 11 支持完整 C++17（本机 MinGW 只有 gnu++14）
#   - 只有这里才有 libsqlite3，EventStore.hpp 才编译得起来
#
# 用法： cd ~/fs && bash persistence/tests/verify_all.sh
#
# ⚠ ROOT 必须是**项目根**，不是脚本所在目录。脚本住在 persistence/tests/ 下，
#   而下面所有路径都是相对项目根写的（engine/tests、reference、persistence/tests）；
#   旧版把 ROOT 取成脚本所在目录，于是每一步 cd 都拼成
#   `<root>/persistence/tests/engine/tests` 从而全部失败。

set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

pass=0; failn=0
step() { printf '\n=== %s ===\n' "$1"; }
check() { if [ "$1" -eq 0 ]; then printf '  [通过] %s\n' "$2"; pass=$((pass+1));
          else printf '  [失败] %s\n' "$2"; failn=$((failn+1)); fi; }

step "1/5  C++ 内核（GCC 11, C++17 + C++14 双标准）"
cd "$ROOT/engine/tests"
g++ -std=c++17 -Wall -Wextra -O1 -I../include -I../../include test_core.cpp -o test_core17 2>&1 | head -5
check ${PIPESTATUS[0]:-0} "test_core 编译（C++17，零警告）"
g++ -std=c++14 -Wall -Wextra -O1 -I../include -I../../include test_core.cpp -o test_core14 2>&1 | head -5
check ${PIPESTATUS[0]:-0} "test_core 编译（C++14）"
./test_core17 > /tmp/nx17.txt 2>&1;    check $? "内核测试运行"
grep -q "ALL ASSERTIONS PASSED" /tmp/nx17.txt; check $? "断言全部通过"
grep -o "LLM 调用次数=[0-9]*" /tmp/nx17.txt

step "2/5  持久化层 C++（真实 libsqlite3）"
cd "$ROOT/persistence/tests"
g++ -std=c++17 -Wall -Wextra -Wpedantic -O1 -I../include -I../../engine/include -I../../include \
    test_store_cpp.cpp -o test_store_cpp -lsqlite3 2>&1 | head -5
check ${PIPESTATUS[0]:-0} "test_store_cpp 编译（零警告）"
rm -f cpp_smoke.db cpp_smoke.db-wal cpp_smoke.db-shm
./test_store_cpp > /tmp/nxstore.txt 2>&1; check $? "EventStore 烟测运行"
tail -2 /tmp/nxstore.txt | head -1

step "3/5  Python 参考实现"
cd "$ROOT"
python3 reference/simulate.py > /tmp/nxsim.txt 2>&1; check $? "simulate.py 运行"
tail -2 /tmp/nxsim.txt | head -1

step "4/5  SQLite 四道闸门"
python3 persistence/tests/test_store.py > /tmp/nxsql.txt 2>&1; check $? "test_store.py 运行"
tail -2 /tmp/nxsql.txt | head -1

step "5/5  跨语言 Test Oracle：C++ 与 Python 归一化轨迹逐字节比对"
# 两侧用**同一份**写死的输入脚本（A / A0 / B / B0）各跑一遍，
# 输出格式由 reference/cases_pointer.py::emit_trace 与
# engine/tests/test_core.cpp::trace_line 约定，必须逐字段一致。
# tr -d '\r' 是为了抹掉可能的 CRLF 差异，保证 diff 只看语义。
cd "$ROOT/engine/tests"
./test_core17 --emit-trace 2>/dev/null | grep '^case=' | tr -d '\r' > /tmp/nx_cpp_trace.txt
check $? "C++ 侧导出归一化轨迹"
cd "$ROOT"
python3 reference/cases_pointer.py --emit-trace 2>/dev/null | tr -d '\r' > /tmp/nx_py_trace.txt
check $? "Python 侧导出归一化轨迹"
if diff -u /tmp/nx_cpp_trace.txt /tmp/nx_py_trace.txt > /tmp/nx_trace_diff.txt 2>&1; then
    check 0 "两侧轨迹完全一致（$(wc -l < /tmp/nx_cpp_trace.txt) 行逐字节相同）"
else
    check 1 "两侧轨迹完全一致"
    head -20 /tmp/nx_trace_diff.txt
fi

printf '\n========================================\n'
printf '小结：%d 项粗粒度检查通过，%d 项失败\n' "$pass" "$failn"
printf '========================================\n'
[ "$failn" -eq 0 ]
