// Exercise the DFA->NFA fallback path (capturing match requires submatch
// tracking after the DFA locates the match) and the BitState path on large
// text, measuring whether either engine retains O(text) memory.
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "absl/strings/str_format.h"
#include "re2/re2.h"

static int64_t HeapBytes() { return mallinfo2().uordblks; }

static int64_t PeakRssKb() {
  FILE* f = fopen("/proc/self/status", "r");
  char line[256];
  int64_t peak = -1;
  while (fgets(line, sizeof line, f) != NULL) {
    if (strncmp(line, "VmHWM:", 6) == 0) {
      sscanf(line + 6, "%ld", &peak);
      break;
    }
  }
  fclose(f);
  return peak;
}

static void Run(const char* name, const char* pattern, size_t mb,
                int64_t max_mem) {
  RE2::Options opts;
  opts.set_log_errors(false);
  if (max_mem > 0) opts.set_max_mem(max_mem);
  RE2 re(pattern, opts);
  if (!re.ok()) {
    absl::FPrintF(stderr, "[%s] compile failed: %s\n", name, re.error());
    return;
  }

  size_t n = mb * 1024ULL * 1024ULL;
  std::string text;
  text.reserve(n);
  const char filler[] = "user=alice id=42 region=us-west message=hello\n";
  size_t flen = strlen(filler);
  while (text.size() + flen <= n) text.append(filler);
  text.resize(n);

  int64_t heap_before = HeapBytes();
  int count = 0;
  absl::string_view a, b;
  absl::string_view input = text;
  // Two capture groups forces submatch tracking -> NFA/OnePass/BitState work.
  while (RE2::FindAndConsume(&input, re, &a, &b)) {
    if (++count >= 5000000) break;
  }
  int64_t heap_growth = (HeapBytes() - heap_before) / 1024;

  absl::PrintF(
      "[%-18s] %zuMiB matches=%d engine_heap_growth=%ldKB peak_rss=%ldKB\n",
      name, mb, count, heap_growth, PeakRssKb());
}

int main(int argc, char** argv) {
  size_t mb = argc > 1 ? strtoul(argv[1], NULL, 10) : 32;
  // Capturing patterns: after DFA finds the match end, RE2 runs the NFA
  // family (OnePass/BitState/NFA) to recover submatch boundaries.
  Run("capture_onetpass", R"(user=(\w+) id=(\d+))", mb, -1);
  Run("capture_generic", R"((\w+)=(\w+))", mb, -1);
  // Tiny DFA budget to force frequent DFA failure -> NFA fallback for the
  // whole text instead of a short anchored sub-search.
  Run("tiny_dfabudget", R"(user=(\w+) id=(\d+))", mb, 4096);
  return 0;
}
