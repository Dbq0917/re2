#!/usr/bin/env bash
# Run each pattern against a small string and a large text, in separate
# processes, and compare match results plus peak RSS (VmHWM).
set -u
cd "$(dirname "$0")/.."
MB=${1:-32}
echo "== small (119 bytes) vs big (${MB} MiB), same patterns =="
printf "%-12s | %-28s | %-28s | %s\n" case small big result_consistent
fail=0
for i in 0 1 2 3 4 5; do
  s=$(./memtest/mem_probe small "$MB" "$i")
  b=$(./memtest/mem_probe big "$MB" "$i")
  s_name=$(echo "$s" | cut -f2); s_match=$(echo "$s" | cut -f3); s_peak=$(echo "$s" | cut -f6)
  b_match=$(echo "$b" | cut -f3); b_count=$(echo "$b" | cut -f4); b_peak=$(echo "$b" | cut -f6)
  if [ "$s_match" = "$b_match" ]; then ok=YES; else ok=NO; fail=1; fi
  printf "%-12s | match=%s peak=%7s KB      | match=%s n=%-5s peak=%7s KB | %s\n" \
    "$s_name" "$s_match" "$s_peak" "$b_match" "$b_count" "$b_peak" "$ok"
done
exit $fail
