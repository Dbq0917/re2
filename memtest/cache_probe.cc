// Verify the DFA state cache is bounded by max_mem even when the pattern's
// full DFA is vastly larger than the budget. Mirrors dfa_test's construction
// (pattern 0[01]{N}$, De Bruijn input, budget 2^N) and then repeats the input
// up to tens of MiB so the cache-flush path is exercised on long text.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "absl/strings/str_format.h"
#include "re2/prog.h"
#include "re2/re2.h"
#include "re2/regexp.h"

static int g_flushes = 0;
static size_t g_max_cache = 0;

static void OnReset(const re2::hooks::DFAStateCacheReset& info) {
  ++g_flushes;
  if (info.state_cache_size > g_max_cache)
    g_max_cache = info.state_cache_size;
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

static std::string DeBruijn(int n) {
  // Same construction as re2/testing/string_generator.cc:DeBruijnString.
  const size_t size = size_t{1} << static_cast<size_t>(n);
  const size_t mask = size - 1;
  std::vector<bool> did(size, false);
  std::string s;
  s.reserve(static_cast<size_t>(n) + size);
  for (size_t i = 0; i < static_cast<size_t>(n - 1); i++)
    s += '0';
  size_t bits = 0;
  for (size_t i = 0; i < size; i++) {
    bits <<= 1;
    bits &= mask;
    if (!did[bits | 1]) {
      bits |= 1;
      s += '1';
    } else {
      s += '0';
    }
    did[bits] = true;
  }
  return s;
}

int main(int argc, char** argv) {
  size_t mb = argc > 1 ? strtoul(argv[1], NULL, 10) : 32;
  int n = argc > 2 ? atoi(argv[2]) : 18;
  re2::hooks::SetDFAStateCacheResetHook(OnReset);
  re2::Prog::TESTING_ONLY_set_dfa_should_bail_when_slow(false);

  re2::Regexp* regexp = re2::Regexp::Parse(
      absl::StrFormat("0[01]{%d}$", n), re2::Regexp::LikePerl, NULL);
  if (regexp == NULL) return 2;
  int64_t budget = 1LL << n;  // 256 KiB for n=18, same as dfa_test.
  re2::Prog* prog = regexp->CompileToProg(budget);
  if (prog == NULL) {
    regexp->Decref();
    return 2;
  }

  std::string window = DeBruijn(n);
  std::string match = window + "0";
  std::string no_match = window;

  // Repeat the windows to build a long text while keeping each window's
  // state-space coverage. Matching windows are separated by a sentinel so
  // the anchored-end semantics stay local to each window.
  std::string text;
  size_t target = mb * 1024ULL * 1024ULL;
  text.reserve(target);
  int rounds = 0;
  while (text.size() + match.size() + 1 < target) {
    text.append(rounds++ % 2 ? no_match : match);
    text.push_back('\n');
  }

  bool failed = false;
  bool small_m = prog->SearchDFA(match, absl::string_view(),
                                 re2::Prog::kUnanchored,
                                 re2::Prog::kFirstMatch, NULL, &failed, NULL);
  bool small_nm = prog->SearchDFA(no_match, absl::string_view(),
                                  re2::Prog::kUnanchored,
                                  re2::Prog::kFirstMatch, NULL, &failed, NULL);

  // Long-text search: the whole text is one context; expect no match because
  // the final window is a no_match window (ends with 1...0 pattern check).
  g_flushes = 0;
  g_max_cache = 0;
  bool big = prog->SearchDFA(text, absl::string_view(),
                             re2::Prog::kUnanchored,
                             re2::Prog::kFirstMatch, NULL, &failed, NULL);

  absl::PrintF(
      "n=%d budget=%dKB text=%zuMiB(%zuB) small_match=%d small_nomatch=%d "
      "big_match=%d failed=%d cache_flushes=%d max_cached_states=%zu "
      "peak_rss=%ldKB (%.1fMiB)\n",
      n, static_cast<int>(budget / 1024), mb, text.size(), small_m,
      small_nm, big, failed, g_flushes, g_max_cache, PeakRssKb(),
      PeakRssKb() / 1024.0);

  delete prog;
  regexp->Decref();
  return 0;
}
