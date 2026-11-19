#!/usr/bin/env bash
# Compare whole-input vs streaming chunked matching:
#   - match results must be identical for bounded patterns;
#   - peak RSS (VmHWM) is recorded per policy in a fresh process.
# Chunk size is derived from a memory budget, not hard-coded.
set -u
cd "$(dirname "$0")/.."

MB=${1:-32}
PATTERN=${2:-'[A-Za-z0-9._%+-]{1,64}@[A-Za-z0-9.-]{1,128}\.[A-Za-z]{2,16}'}
TOKEN=${3:-alice@example.com}

echo "text=${MB}MiB pattern=${PATTERN}"
printf "%-8s %-10s %-12s %-12s %s\n" policy budgetKB matches peak_rssKB verdict

whole_line=$(./memtest/stream_probe whole "$MB" $((MB * 1024)) "$PATTERN" "$TOKEN")
whole_matches=$(echo "$whole_line" | sed 's/.*matches=\([0-9]*\).*/\1/')
whole_rss=$(echo "$whole_line" | sed 's/.*peak_rss=\([0-9]*\)KB/\1/')
printf "%-8s %-10s %-12s %-12s %s\n" whole "$((MB * 1024))" "$whole_matches" "$whole_rss" reference

fail=0
for budget in 4096 1024 256 64; do
  stream_line=$(./memtest/stream_probe stream "$MB" "$budget" "$PATTERN" "$TOKEN" 2>/tmp/stream_warn)
  stream_matches=$(echo "$stream_line" | sed 's/.*matches=\([0-9]*\).*/\1/')
  stream_rss=$(echo "$stream_line" | sed 's/.*peak_rss=\([0-9]*\)KB/\1/')
  if [ "$stream_matches" = "$whole_matches" ]; then
    verdict="OK ($(( whole_rss / stream_rss ))x lower peak)"
  else
    verdict="DIFF whole=$whole_matches stream=$stream_matches"
    fail=1
  fi
  printf "%-8s %-10s %-12s %-12s %s\n" stream "$budget" "$stream_matches" "$stream_rss" "$verdict"
  [ -s /tmp/stream_warn ] && sed 's/^/  ! /' /tmp/stream_warn
done
exit $fail
