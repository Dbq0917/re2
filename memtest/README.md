# 大文本正则匹配的内存策略实测

本目录是独立的测量脚手架，**不改动 `re2/` 库代码**。它回答任务里的三个问题：

1. 同一模式在短串和几十 MB 长文本上，引擎自身的内存开销是否随输入增长？
2. 整块匹配和按需分块匹配的峰值内存差多少？分块会不会漏匹配？
3. 块大小如何不写死、又如何处理跨块边界？

## 背景结论（先看代码再实测）

标准 RE2 的匹配路径里没有“先把整段输入缓存下来做预处理、再匹配”的第二阶段。
输入始终以 `absl::string_view` 零拷贝传递，引擎侧与文本长度相关的分配只有：

- DFA 状态缓存：受 `RE2::Options::max_mem` 约束（默认 8 MiB）。预算耗尽时
  `DFA::ResetCache()` 丢弃全部状态重建，见 `re2/dfa.cc` 的 `CachedState`
  与 `ResetCache`，不会随输入无限增长。
- BitState 位图：`list_count * (text.size()+1)` 位，但有
  `kBitStateBitmapMaxSize = 256 Kb` 上限（`re2/prog.cc`），超限直接不启用。
- NFA 线程队列：按程序指令数和捕获组数分配，与文本长度无关（`re2/nfa.cc`）。

因此“内存翻几倍”里的大头是**调用方持有的整段输入本身**，不是引擎缓存。
分块策略要压的是这部分常驻内存。

## 探针说明

| 程序 | 作用 |
| --- | --- |
| `mem_probe.cc` | 同一模式分别跑短串和 32/64 MiB 长文本，分进程读 `VmHWM`，比对匹配结果 |
| `heap_probe.cc` | 用 `mallinfo2().uordblks` 区分输入占用与引擎匹配后保留的堆内存 |
| `cache_probe.cc` | 复用 `dfa_test` 的 `0[01]{18}$` + De Bruijn 构造，统计 `DFAStateCacheReset` 次数，验证状态缓存封顶 |
| `fallback_probe.cc` | 带捕获组与极小 `max_mem`，强制走 DFA→NFA/OnePass/BitState 回退路径，测其堆增长 |
| `chunk_match.cc` | 同一文本上整块匹配与分块匹配的完整匹配偏移量逐一对拍（含语法树推导的最大匹配长度） |
| `stream_probe.cc` | 真流式：数据按需生成进固定缓冲，整段文本从不同时存在；对比 whole/stream 两策略的峰值 RSS 与匹配数 |
| `run_compare.sh` | `mem_probe` 的短串/长文本批量对拍 |
| `run_stream_compare.sh` | `stream_probe` 的多预算（4096/1024/256/64 KiB）对拍，块大小由预算自适应 |

## 构建

依赖 Abseil。仓库根目录下用 CMake：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DRE2_TEST=ON \
  -DCMAKE_PREFIX_PATH=$PWD/.deps/abseil-cpp-20250814.1/install
cmake --build build -j
```

探针单独编译（示例）：

```sh
g++ -std=c++17 -O2 -g -I. -I.deps/abseil-cpp-20250814.1/install/include \
  memtest/stream_probe.cc -o memtest/stream_probe build/libre2.a \
  -Wl,--start-group $(find .deps/abseil-cpp-20250814.1/install/lib \
  -name 'libabsl_*.a' | tr '\n' ' ') -Wl,--end-group -lpthread
```

## 实测结果（本机，32/64 MiB 文本）

引擎自身内存与文本长度解耦：

- 32 MiB 文本匹配后引擎堆增长约 5 KiB，64 MiB 同样约 5 KiB（`heap_probe`）。
- 32 MiB 文本下 DFA 缓存被重置 8414 次、64 MiB 下 16828 次，缓存状态数封顶
  在约 1.2k，峰值 RSS 始终约等于“输入大小 + 4 MiB 固定基线”（`cache_probe`）。

分块 vs 整块（有界模式，匹配结果逐一对拍一致）：

| 文本 | 策略 | 块预算 | 峰值 RSS | 匹配数 |
| --- | --- | --- | --- | --- |
| 32 MiB | whole | 32 MiB | ~37.5 MiB | 69952 |
| 32 MiB | stream | 4 MiB | ~8.9 MiB | 69952 |
| 32 MiB | stream | 1 MiB | ~5.8 MiB | 69952 |
| 32 MiB | stream | 256 KiB | ~5.0 MiB | 69952 |
| 32 MiB | stream | 64 KiB | ~4.8 MiB | 69952 |
| 64 MiB | whole | 64 MiB | ~70 MiB | 139817 |
| 64 MiB | stream | 256 KiB | ~5.2 MiB | 139817 |

## 跨块边界怎么不漏

- 重叠窗口宽度 = 模式最长可能匹配长度 + 1。该长度由解析后的 `Regexp`
  语法树推导（`chunk_match.cc` 的 `MaxMatchLength`），不是写死的常量。
- 块步进 = 块大小 − 重叠宽度；每个匹配起点只在唯一一个块的非重叠区被接受，
  配合全局已消费游标，避免跨块重复或漏报。
- 块大小来自调用方给的内存预算（`budget − overlap`），预算变化时块大小
  自适应，不存在固定魔数。
- 无界模式（`*`、`+`、无上界的 `{n,}`）不存在能保证正确性的有限重叠窗口。
  `stream_probe` 通过语法树检测到这种模式会显式告警；这类模式要么退回整块
  匹配，要么由上层保证单条记录不会跨块（例如按行切分）。
