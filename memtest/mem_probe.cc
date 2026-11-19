// Empirical memory probe: compare RE2 peak RSS on a small string vs a
// multi-tens-of-MB text. Runs one mode per process so VmHWM is attributable
// to that mode alone. Not part of the library; a measurement harness.
//
// Usage: mem_probe {small|big} <MiB> <case_index>
// Emits a single TSV line: mode case matched count first_20_bytes peak_kb
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "absl/strings/str_format.h"
#include "re2/re2.h"

static int64_t PeakRssKb() {
  FILE* f = fopen("/proc/self/status", "r");
  if (f == NULL) return -1;
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

struct Case {
  const char* name;
  const char* pattern;
  bool capture;
  // Token that the generator injects periodically into the big text.
  const char* token;
};

static const Case kCases[] = {
    {"literal", "Error: disk full", false, "Error: disk full"},
    {"email", R"(([A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}))", true,
     "alice@example.com"},
    {"digits_run", R"(([0-9]{8,}))", true, "12345678"},
    {"url", R"((https?://[^\s"']+))", true, "https://example.com/path"},
    {"capture_alt", R"((foo+|ba+r|qux+)[0-9])", true, "fooooo1"},
    {"word_anchor", R"(\w+$)", false, "tailword"},
};

static std::string MakeBig(size_t n, const char* token) {
  std::string big;
  big.reserve(n);
  const char filler[] = "2024-01-02 INFO routine log padding line here\n";
  size_t flen = strlen(filler);
  size_t tlen = strlen(token);
  int lines = 0;
  while (big.size() + flen + tlen + 1 <= n) {
    big.append(filler);
    if (++lines % 512 == 0) {
      big.push_back(' ');
      big.append(token);
      big.push_back('\n');
    }
  }
  if (n >= tlen) {
    big.resize(n - tlen);
    big.append(token);
  } else {
    big.resize(n);
  }
  return big;
}

int main(int argc, char** argv) {
  if (argc < 4) {
    absl::FPrintF(stderr, "usage: %s {small|big} <MiB> <case_index>\n",
                  argv[0]);
    return 2;
  }
  bool big_mode = strcmp(argv[1], "big") == 0;
  size_t mb = strtoul(argv[2], NULL, 10);
  int ci = atoi(argv[3]);
  if (ci < 0 || ci >= (int)(sizeof(kCases) / sizeof(kCases[0]))) return 2;
  const Case& c = kCases[ci];

  RE2::Options opts;
  opts.set_log_errors(false);
  RE2 re(c.pattern, opts);
  if (!re.ok()) {
    absl::FPrintF(stderr, "compile failed: %s\n", re.error());
    return 2;
  }

  std::string small =
      "2024-01-02 Error: disk full contact alice@example.com see "
      "https://example.com/x id 12345678 foooo1 bar2 quxxx3 tailword";
  std::string text = big_mode ? MakeBig(mb * 1024ULL * 1024ULL, c.token)
                              : small;

  int count = 0;
  bool matched = false;
  std::string first;
  if (c.capture) {
    absl::string_view cap;
    absl::string_view input = text;
    while (RE2::FindAndConsume(&input, re, &cap)) {
      if (!matched) {
        matched = true;
        first.assign(cap.data(), cap.size() < 20 ? cap.size() : 20);
      }
      count++;
      if (count >= 100000) break;
    }
  } else {
    matched = RE2::PartialMatch(text, re);
    count = matched ? 1 : 0;
    if (matched) first = c.token;
  }

  int64_t peak = PeakRssKb();
  absl::PrintF("%s\t%s\t%d\t%d\t%s\t%ld\t%zu\n", argv[1], c.name, matched,
               count, first.c_str(), peak, text.size());
  return 0;
}
