// Attribute heap usage precisely: mallinfo2.uordblks counts bytes
// allocated (not RSS), so we can separate the input text's own footprint
// from any extra heap that the matching engine retains.
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "absl/strings/str_format.h"
#include "re2/re2.h"

static int64_t HeapBytes() {
  struct mallinfo2 mi = mallinfo2();
  return static_cast<int64_t>(mi.uordblks);
}

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

int main(int argc, char** argv) {
  size_t mb = argc > 1 ? strtoul(argv[1], NULL, 10) : 32;
  const char* pattern =
      argc > 2 ? argv[2]
               : R"(([A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}))";

  RE2::Options opts;
  opts.set_log_errors(false);
  RE2 re(pattern, opts);
  if (!re.ok()) return 2;

  int64_t heap0 = HeapBytes();

  size_t n = mb * 1024ULL * 1024ULL;
  std::string text;
  int64_t heap_before_reserve = HeapBytes();
  text.reserve(n);
  int64_t heap_after_reserve = HeapBytes();
  const char filler[] = "line with alice@example.com and padding text\n";
  size_t flen = strlen(filler);
  while (text.size() + flen <= n) text.append(filler);
  text.resize(n);

  int64_t heap_with_text = HeapBytes();

  int count = 0;
  absl::string_view input = text;
  absl::string_view cap;
  while (RE2::FindAndConsume(&input, re, &cap)) {
    if (++count >= 5000000) break;
  }

  int64_t heap_after_match = HeapBytes();
  absl::PrintF(
      "text=%zuMiB matches=%d\n"
      "  heap baseline=%ldKB, reserve_delta=%ldKB, fill_delta=%ldKB, "
      "engine_delta_after_match=%ldKB\n"
      "  total heap=%ldKB of which text capacity=%zuKB; "
      "engine_retained_over_text=%ldKB\n"
      "  peak RSS=%ldKB (%.1fMiB)\n",
      mb, count, heap0 / 1024,
      (heap_after_reserve - heap_before_reserve) / 1024,
      (heap_with_text - heap_after_reserve) / 1024,
      (heap_after_match - heap_with_text) / 1024,
      heap_after_match / 1024, text.capacity() / 1024,
      (heap_after_match - static_cast<int64_t>(text.capacity())) / 1024,
      PeakRssKb(), PeakRssKb() / 1024.0);
  return 0;
}
