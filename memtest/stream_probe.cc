// True streaming memory probe: text is synthesized on the fly into a
// fixed-size working buffer, so no multi-tens-of-MB string ever exists.
// Compares two policies in separate processes:
//   whole  - buffer the entire generated text then match it once;
//   stream - feed overlapping chunks sized by a memory budget, never
//            holding more than (chunk + overlap) bytes of text at once.
// Both emit match counts; a separate checker compares them.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <string>

#include "absl/strings/str_format.h"
#include "re2/regexp.h"
#include "re2/re2.h"

// Returns true if every match of the parsed regexp has a finite length.
// Walks the Regexp tree; Star/Plus and upper-unbounded Repeat make it false.
static bool IsBounded(re2::Regexp* r) {
  using re2::Regexp;
  if (r == NULL) return true;
  switch (r->op()) {
    case re2::kRegexpStar:
    case re2::kRegexpPlus:
      return false;
    case re2::kRegexpRepeat:
      if (r->max() < 0) return false;
      return IsBounded(r->sub()[0]);
    case re2::kRegexpConcat:
    case re2::kRegexpAlternate: {
      re2::Regexp** subs = r->sub();
      for (int i = 0; i < r->nsub(); ++i)
        if (!IsBounded(subs[i])) return false;
      return true;
    }
    case re2::kRegexpQuest:
    case re2::kRegexpCapture:
      return IsBounded(r->sub()[0]);
    default:
      return true;  // literals, classes, zero-width assertions, etc.
  }
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

// Deterministic pseudo-random filler so both policies see identical text.
struct Generator {
  uint64_t seed;
  const std::string token;
  size_t period;
  char At(size_t pos) {
    if (period && pos % period < token.size())
      return token[pos % token.size()];
    static const char alpha[] =
        "abcdefghijklmnopqrstuvwxyz0123456789 .,\n";
    uint64_t h = seed ^ (pos * 0x9e3779b97f4a7c15ULL);
    h ^= h << 13;
    h ^= h >> 7;
    h ^= h << 17;
    return alpha[h % (sizeof(alpha) - 1)];
  }
};

int main(int argc, char** argv) {
  if (argc < 5) {
    absl::FPrintF(stderr,
                  "usage: %s {whole|stream} <MiB> <budgetKB> <pattern> "
                  "[token]\n",
                  argv[0]);
    return 2;
  }
  bool stream = strcmp(argv[1], "stream") == 0;
  size_t mb = strtoul(argv[2], NULL, 10);
  size_t budget = strtoul(argv[3], NULL, 10) * 1024;
  std::string wrapped = std::string("(") + argv[4] + ")";
  std::string token = argc > 5 ? argv[5] : "alice@example.com";

  RE2::Options opts;
  opts.set_log_errors(false);
  RE2 re(wrapped, opts);
  if (!re.ok()) {
    absl::FPrintF(stderr, "compile failed: %s\n", re.error());
    return 2;
  }

  size_t total = mb * 1024ULL * 1024ULL;
  // A bounded token length is the overlap guarantee for this harness: the
  // pattern's longest possible single match cannot exceed the token size.
  size_t overlap = std::min(token.size() + 1, budget / 2);
  size_t chunk = stream ? std::min(budget, total) : total;
  size_t stride = chunk > overlap ? chunk - overlap : chunk;

  // The overlap window must be at least as wide as the longest possible
  // single match. With an unbounded pattern (e.g. .*, a+ or a{3,}) there is
  // no finite window that guarantees this, so flag the limitation explicitly
  // rather than silently dropping boundary-straddling matches.
  bool pattern_is_bounded = IsBounded(re.Regexp());
  if (stream && !pattern_is_bounded) {
    absl::FPrintF(stderr,
                  "WARNING: pattern may be unbounded; streaming overlap "
                  "(%zuB) cannot guarantee correctness for matches longer "
                  "than the window that straddle a chunk boundary\n",
                  overlap);
  }

  Generator gen{0x123456789abcdef0ULL, token, 256};
  std::string buf;
  buf.reserve(chunk);

  int64_t matches = 0;
  size_t cursor = 0;  // global consumed offset
  size_t base = 0;

  if (!stream) {
    // Whole-input policy: materialize everything first.
    for (size_t pos = 0; pos < total; ++pos) buf.push_back(gen.At(pos));
    absl::string_view input = buf, m;
    while (RE2::PartialMatch(input, re, &m)) {
      ++matches;
      size_t step = (m.data() - input.data()) + (m.empty() ? 1 : m.size());
      input.remove_prefix(step);
    }
  } else {
    // Streaming policy: refill only the consumed part of the buffer.
    size_t valid = 0;
    while (base < total) {
      size_t want = std::min(chunk, total - base);
      // Keep the trailing overlap in place at the front of the buffer and
      // generate only the genuinely new bytes after it.
      if (base > 0) {
        size_t keep = std::min(overlap, valid);
        memmove(buf.data(), buf.data() + valid - keep, keep);
        valid = keep;
      }
      for (size_t pos = base + valid; pos < base + want; ++pos)
        buf[valid++] = gen.At(pos);
      absl::string_view window(buf.data(), valid);
      size_t skip = cursor > base ? cursor - base : 0;
      absl::string_view input = window.substr(skip);
      absl::string_view m;
      size_t safe = (base + want == total) ? want
                                           : std::min(base + stride, total) -
                                                 base;
      while (RE2::PartialMatch(input, re, &m)) {
        size_t local_begin = m.data() - window.data();
        if (local_begin >= safe) break;
        ++matches;
        cursor = base + local_begin + m.size();
        size_t rel = (m.data() - input.data()) +
                     (m.empty() ? 1 : m.size());
        input.remove_prefix(rel);
        if (input.empty()) break;
      }
      if (base + want == total) break;
      base += stride;
    }
  }

  absl::PrintF("%s\t%zuMiB\tbudget=%zuKB\tchunk=%zuKB\toverlap=%zuB\t"
               "matches=%ld\tpeak_rss=%ldKB\n",
               stream ? "stream" : "whole", mb, budget / 1024, chunk / 1024,
               overlap, matches, PeakRssKb());
  return 0;
}
