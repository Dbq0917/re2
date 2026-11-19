// Streaming chunked matcher vs whole-input matching.
//
// Demonstrates the memory-policy tradeoff described in the task:
//   * whole-input matching never misses a match, but the caller must hold
//     the entire text in memory;
//   * chunked matching bounds live memory, but a match can straddle a chunk
//     boundary, so chunks overlap by a window wide enough to cover it.
//
// The overlap window is derived from the pattern, not hard-coded:
//   1. If the program has a finite maximum match length, use it.
//   2. Otherwise (unbounded quantifiers), fall back to an adaptive window
//      scaled from the caller's memory budget, which is also how the chunk
//      size is chosen.
//
// Correctness is checked by replaying the same text through whole-input
// matching and comparing the full match-offset list.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "absl/strings/str_format.h"
#include "re2/prog.h"
#include "re2/re2.h"
#include "re2/regexp.h"

namespace {

struct Match {
  size_t begin;
  size_t end;
  bool operator==(const Match& o) const {
    return begin == o.begin && end == o.end;
  }
};

// Finite maximum match length (in bytes), or -1 when the pattern admits
// arbitrarily long matches. Walks the parsed Regexp tree directly.
int64_t MaxMatchLength(re2::Regexp* r) {
  using re2::Regexp;
  if (r == NULL) return -1;
  switch (r->op()) {
    case re2::kRegexpNoMatch:
    case re2::kRegexpEmptyMatch:
    case re2::kRegexpBeginLine:
    case re2::kRegexpEndLine:
    case re2::kRegexpWordBoundary:
    case re2::kRegexpNoWordBoundary:
    case re2::kRegexpBeginText:
    case re2::kRegexpEndText:
    case re2::kRegexpHaveMatch:
      return 0;
    case re2::kRegexpLiteral:
      // Bytes needed to encode the rune in UTF-8 (Latin1 -> 1).
      return r->rune() < 0x80 ? 1 : r->rune() < 0x800 ? 2
                              : r->rune() < 0x10000 ? 3 : 4;
    case re2::kRegexpLiteralString:
      return r->nrunes();  // Latin1 runes in this simplified form.
    case re2::kRegexpAnyChar:
    case re2::kRegexpAnyByte:
    case re2::kRegexpCharClass:
      return 4;  // worst-case UTF-8 encoding of one rune
    case re2::kRegexpConcat: {
      int64_t total = 0;
      re2::Regexp** subs = r->sub();
      for (int i = 0; i < r->nsub(); ++i) {
        int64_t s = MaxMatchLength(subs[i]);
        if (s < 0) return -1;
        total += s;
      }
      return total;
    }
    case re2::kRegexpAlternate: {
      int64_t best = 0;
      re2::Regexp** subs = r->sub();
      for (int i = 0; i < r->nsub(); ++i) {
        int64_t s = MaxMatchLength(subs[i]);
        if (s < 0) return -1;
        best = std::max(best, s);
      }
      return best;
    }
    case re2::kRegexpStar:
    case re2::kRegexpPlus:
      return -1;  // unbounded
    case re2::kRegexpQuest:
      return MaxMatchLength(r->sub()[0]);
    case re2::kRegexpRepeat: {
      if (r->max() < 0) return -1;  // e.g. a{3,}
      int64_t one = MaxMatchLength(r->sub()[0]);
      if (one < 0) return -1;
      return one * r->max();
    }
    case re2::kRegexpCapture:
      return MaxMatchLength(r->sub()[0]);
  }
  return -1;
}

int64_t PatternMaxMatchLength(const RE2& re) {
  return MaxMatchLength(re.Regexp());
}

std::vector<Match> WholeInputMatches(absl::string_view text, const RE2& re) {
  std::vector<Match> out;
  absl::string_view input = text;
  absl::string_view m;
  while (RE2::PartialMatch(input, re, &m)) {
    size_t begin = m.data() - input.data();
    size_t abs_begin = (input.data() - text.data()) + begin;
    out.push_back({abs_begin, abs_begin + m.size()});
    input.remove_prefix(begin + (m.empty() ? 1 : m.size()));
    if (input.data() >= text.data() + text.size()) break;
  }
  return out;
}

// Chunked scan. `budget_bytes` is the desired live-memory ceiling for the
// working chunk; the actual chunk size adapts to it and to the overlap
// window instead of being a fixed constant.
std::vector<Match> ChunkedMatches(absl::string_view text, const RE2& re,
                                  size_t budget_bytes,
                                  size_t* chunk_size_used,
                                  size_t* overlap_used) {
  std::vector<Match> out;
  if (text.empty()) return out;

  int64_t finite_max = PatternMaxMatchLength(re);
  // Keep the overlap comfortably above any single finite match. A match of
  // length L starting in the previous chunk needs L-1 bytes of overlap.
  size_t overlap;
  if (finite_max >= 0) {
    overlap = static_cast<size_t>(finite_max) + 1;
  } else {
    // Unbounded pattern: no finite window can guarantee correctness in the
    // abstract; use a budget-derived window and report the limitation.
    overlap = std::min(budget_bytes / 4, size_t(1 << 20));
  }
  if (overlap >= budget_bytes / 2)
    overlap = budget_bytes / 2;  // pathological tiny budget guard
  overlap = std::max<size_t>(overlap, 1);

  size_t chunk = budget_bytes > 2 * overlap ? budget_bytes : 1024;
  chunk = std::min(chunk, text.size());
  *chunk_size_used = chunk;
  *overlap_used = overlap;

  size_t base = 0;
  size_t stride = chunk > overlap ? chunk - overlap : chunk;
  size_t cursor = 0;  // global position already consumed by the scan
  while (base < text.size()) {
    size_t end = std::min(base + chunk, text.size());
    absl::string_view window(text.data() + base, end - base);

    // Never rescan bytes consumed by a match that extended into the overlap.
    size_t skip = cursor > base ? cursor - base : 0;
    absl::string_view input(window.data() + skip, window.size() - skip);
    absl::string_view m;
    // The non-overlap region is [0, stride) in window coordinates. The last
    // chunk's safe region extends to the window end.
    size_t safe = (end == text.size()) ? window.size()
                                       : std::min(stride, window.size());
    while (RE2::PartialMatch(input, re, &m)) {
      size_t local_begin = m.data() - window.data();
      size_t local_end = local_begin + m.size();
      // Only accept matches that start inside this chunk's non-overlap
      // region; starts inside the overlap belong to the next chunk.
      if (local_begin >= safe) break;
      out.push_back({base + local_begin, base + local_end});
      cursor = base + local_end;
      size_t rel = m.data() - input.data() + (m.empty() ? 1 : m.size());
      input.remove_prefix(rel);
      if (input.empty()) break;
    }

    if (end == text.size()) break;
    base += stride;  // next chunk begins inside the previous overlap
  }
  return out;
}

int64_t PeakRssKb() {
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

}  // namespace

int main(int argc, char** argv) {
  size_t mb = argc > 1 ? strtoul(argv[1], NULL, 10) : 32;
  size_t budget_kb = argc > 2 ? strtoul(argv[2], NULL, 10) : 1024;
  const char* pattern =
      argc > 3 ? argv[3] : R"(([A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}))";
  const char* token = argc > 4
                          ? argv[4]
                          : "alice@example.com";

  RE2::Options opts;
  opts.set_log_errors(false);
  // Wrap in an outer capture group so the whole match can always be
  // extracted via one submatch argument regardless of the input pattern.
  std::string wrapped = std::string("(?:") + pattern + ")";
  wrapped = "(" + wrapped + ")";
  RE2 re(wrapped, opts);
  if (!re.ok()) {
    absl::FPrintF(stderr, "compile failed: %s\n", re.error());
    return 2;
  }
  int64_t finite = PatternMaxMatchLength(re);

  size_t n = mb * 1024ULL * 1024ULL;
  std::string text;
  text.reserve(n);
  const char prefix[] = "record ";
  const char suffix[] = " padding padding padding padding\n";
  size_t plen = strlen(prefix), slen = strlen(suffix), tlen = strlen(token);
  while (text.size() + plen + tlen + slen <= n) {
    text.append(prefix);
    text.append(token);
    text.append(suffix);
  }
  text.resize(n);

  // Whole-input reference.
  int64_t rss0 = PeakRssKb();
  std::vector<Match> whole = WholeInputMatches(text, re);
  int64_t rss_whole = PeakRssKb();

  // Chunked run with the adaptive budget. Note: text still exists in this
  // process, so RSS is not the chunked policy's true ceiling; a separate
  // driver (memtest/chunk_driver.sh) spawns fresh processes for that.
  size_t chunk_used = 0, overlap_used = 0;
  std::vector<Match> chunked =
      ChunkedMatches(text, re, budget_kb * 1024, &chunk_used, &overlap_used);
  int64_t rss_chunk = PeakRssKb();

  bool same = whole == chunked;
  absl::PrintF(
      "pattern=%s\n  finite_max_match=%s text=%zuMiB budget=%zuKB\n"
      "  whole:   matches=%zu peak_rss=%ldKB\n"
      "  chunked: matches=%zu chunk=%zuB overlap=%zuB peak_rss=%ldKB "
      "(in-process, text still resident)\n"
      "  result_consistent=%d\n",
      pattern, finite < 0 ? "UNBOUNDED" : std::to_string(finite).c_str(), mb,
      budget_kb, whole.size(), rss_whole, chunked.size(), chunk_used,
      overlap_used, rss_chunk, same);
  if (!same) {
    size_t shown = 0;
    for (size_t i = 0; i < std::max(whole.size(), chunked.size()) &&
                       shown < 5;
         ++i) {
      const Match* w = i < whole.size() ? &whole[i] : NULL;
      const Match* c = i < chunked.size() ? &chunked[i] : NULL;
      if (w == NULL || c == NULL || !(*w == *c)) {
        absl::FPrintF(stderr, "  first diff at %zu: whole=[%s] chunk=[%s]\n",
                      i, w ? absl::StrFormat("%zu,%zu", w->begin, w->end)
                                 .c_str()
                           : "-",
                      c ? absl::StrFormat("%zu,%zu", c->begin, c->end)
                                 .c_str()
                           : "-");
        ++shown;
      }
    }
    return 1;
  }
  return 0;
}
