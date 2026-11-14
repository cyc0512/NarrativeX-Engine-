#!/usr/bin/env bash
#
# scripts/run_sql_bind_test.sh —— R-11 的一键验证（A 直测 + B GameEngine 全链路）
#
# B 段为什么要沙箱：GameEngine 内部把存档路径写死成 "../data/save.db"，
# 直接在项目里跑会把恶意玩家名写进真实的 data/save.db。
# 这里另起一个目录 /tmp/fs_sqltest，里面放一份 story.json 和一张空 save.db，
# 再把工作目录切进去跑，GameEngine 的路径兜底逻辑会正确解析到沙箱里的文件。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/build/sql_bind_test"
SANDBOX="${SANDBOX:-/tmp/fs_sqltest}"

if [[ ! -x "$BIN" ]]; then
    echo "找不到 $BIN，请先： bash scripts/build_asan.sh plain scripts/sql_bind_test.cpp sql_bind_test"
    exit 2
fi

echo "############ A 段：DatabaseManager 直测 ############"
"$BIN"

echo
echo "############ B 段：GameEngine 全链路（沙箱 $SANDBOX）############"
rm -rf "$SANDBOX"
mkdir -p "$SANDBOX/data"
cp "$ROOT/data/story.json" "$SANDBOX/data/story.json"
: > "$SANDBOX/data/save.db"
( cd "$SANDBOX" && "$BIN" engine "data/save.db" )

echo
echo "############ 静态核查 ############"
echo -n "sqlite3_bind_ 出现次数:      "
grep -c 'sqlite3_bind_' "$ROOT/include/DatabaseManager.h"
echo -n "sqlite3_prepare_v2 出现次数: "
grep -c 'sqlite3_prepare_v2' "$ROOT/include/DatabaseManager.h"
echo -n "仍用字符串拼接 SQL 的处数:   "
grep -cE '(".*VALUES *\('"'"'|\+ *playerName *\+|\+ *itemId *\+|\+ *tagId *\+)' "$ROOT/include/DatabaseManager.h" || true
