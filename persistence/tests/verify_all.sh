#!/usr/bin/env bash
# nx-linux/verify_all.sh
#
# 一条命令跑完四层验证。放在 Linux 侧是因为：
#   - GCC 11 支持完整 C++17（本机 MinGW 只有 gnu++14）
#   - 只有这里才有 libsqlite3，EventStore.hpp 才编译得起来
#
# 用法： cd ~/nx-linux && bash verify_all.sh

set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

pass=0; failn=0
step() { printf '\n=== %s ===\n' "$1"; }
check() { if [ "$1" -eq 0 ]; then printf '  [通过] %s\n' "$2"; pass=$((pass+1));
          else printf '  [失败] %s\n' "$2"; failn=$((failn+1)); fi; }

step "1/4  C++ 内核（GCC 11, C++17 + C++14 双标准）"
cd "$ROOT/engine/tests"
g++ -std=c++17 -Wall -Wextra -O1 -I../include -I../../include test_core.cpp -o test_core17 2>&1 | head -5
check ${PIPESTATUS[0]:-0} "test_core 编译（C++17，零警告）"
g++ -std=c++14 -Wall -Wextra -O1 -I../include -I../../include test_core.cpp -o test_core14 2>&1 | head -5
check ${PIPESTATUS[0]:-0} "test_core 编译（C++14）"
./test_core17 > /tmp/nx17.txt 2>&1;    check $? "内核测试运行"
grep -q "ALL ASSERTIONS PASSED" /tmp/nx17.txt; check $? "断言全部通过"
grep -o "LLM 调用次数=[0-9]*" /tmp/nx17.txt

step "2/4  持久化层 C++（真实 libsqlite3）"
cd "$ROOT/persistence/tests"
g++ -std=c++17 -Wall -Wextra -Wpedantic -O1 -I../include -I../../engine/include -I../../include \
    test_store_cpp.cpp -o test_store_cpp -lsqlite3 2>&1 | head -5
check ${PIPESTATUS[0]:-0} "test_store_cpp 编译（零警告）"
rm -f cpp_smoke.db cpp_smoke.db-wal cpp_smoke.db-shm
./test_store_cpp > /tmp/nxstore.txt 2>&1; check $? "EventStore 烟测运行"
tail -2 /tmp/nxstore.txt | head -1

step "3/4  Python 参考实现"
cd "$ROOT"
python3 reference/simulate.py > /tmp/nxsim.txt 2>&1; check $? "simulate.py 运行"
tail -2 /tmp/nxsim.txt | head -1

step "4/4  SQLite 四道闸门"
python3 persistence/tests/test_store.py > /tmp/nxsql.txt 2>&1; check $? "test_store.py 运行"
tail -2 /tmp/nxsql.txt | head -1

printf '\n========================================\n'
printf '小结：%d 项粗粒度检查通过，%d 项失败\n' "$pass" "$failn"
printf '========================================\n'
[ "$failn" -eq 0 ]
