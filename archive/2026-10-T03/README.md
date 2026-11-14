# 归档：2026-10 T03 闪退取证素材

> 归档日期：2026-11-10
> 来源：原 `forensics/`（26 文件）、`scripts/` 中 4 个失效脚本、`engine/tests/` 的临时输出
> 关联文档：`docs/CRASH_ROOT_CAUSE.md`（D-4 / D-7 根因结论，正文仍按原路径引用本目录文件）

## 为什么归档

这批文件是 2026-10-30 那轮 R-03（自由输入闪退）取证的一次性产物，时间戳全部 ≤ 2026-11-03，
而生产链路最后一次改动是 2026-11-09。它们**不被 `CMakeLists.txt` 引用**，且其中 4 个 `.cpp`
仍 `#include "GameEngine.h"`（该文件已在后续重构中删除）→ 已无法编译。

归档是**移动而非删除**，全部可从本目录取回；被移动的文件原本都被 git 跟踪，亦可 `git checkout <旧提交> -- <路径>` 恢复。

## 完整备份

清理前的全量快照在 VM 的 `~/fs_before_cleanup_20261110.tgz`（13 MB，含未提交的 `src/EngineAdapter.cpp`）。

## 已删除（未归档）

6 个 ELF 可执行文件：`asan_sanity` / `d4_final` / `d4_probe2` / `d4_probe3` / `d4_probe4` / `d4_watcher_uaf`
（合计约 4.2 MB）。它们是编译产物，可由本目录同名 `.cpp` 配合 `scripts/build_asan.sh` 重新生成，
也可从 git 历史恢复，故不保留二进制。

## 未归档、仍在原位的工具

| 文件 | 用途 |
|---|---|
| `scripts/build_asan.sh` | ASan / 非 ASan 构建入口，`terminal` / `gui` / `src` / `plain` 模式仍引用生产源码，有效 |
| `scripts/ui_autopilot.cpp` | `build_asan.sh pilot` 模式的 LD_PRELOAD 自动驾驶依赖 |

> ⚠️ 复用注意事项：判定 D-4 这类 Qt 生命周期 UAF **必须以非 ASan 构建为准**。
> 系统 Qt 未插桩且 ASan 替换分配器会把 freed 区域隔离，带 ASan 反而跑绿，会掩盖崩溃。

## 目录内容索引

**取证证据**：`evidence_A.txt`（pre-fix 非 ASan，退出码 139）、`evidence_B.txt`（ASan 掩盖，0 崩溃）、
`evidence_C.txt` / `evidence_C2_r04.txt` / `evidence_C3_r04.txt`（post-fix 与 R-04 入口数校验）

**驱动器**：`drive_free_input.cpp`（真实 sendBtn lambda 链路）、`mock_ai.py`（快慢两种 AI 响应）
**冻结副本**：`qt_main_prefix.cpp`（修复前 `qt_main.cpp`）、`db_prefix/DatabaseManager.h`（参数化前版本）
**对照测试**：`sql_injection.cpp`
**迭代探针**：`asan_sanity.cpp` / `d4_*.cpp` / `d5_probe.cpp` / `e2e_chinese.cpp` / `d4_uaf.cpp` / `sql_bind_test.cpp`
**临时输出**：`a.txt` / `b.txt` / `actual_trace.txt`
**已失效脚本**：`run_sql_bind_test.sh`（引用已删除的 `data/story.json`）
