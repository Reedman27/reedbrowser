#!/usr/bin/env bash
set -euo pipefail
pids="$({ pgrep -x reedbrowser || true; pgrep -x WebKitWebProcess || true; pgrep -x WebKitNetworkProcess || true; pgrep -x WebKitGPUProcess || true; } | sort -un)"
printf '%-8s %-10s %-10s %-8s %s\n' PID RSS_KiB PSS_KiB CPU COMMAND
while IFS= read -r p; do
  [[ -n "$p" && -r "/proc/$p/status" ]] || continue
  rss=$(awk '/^VmRSS:/ {print $2}' "/proc/$p/status")
  pss=$(awk '/^Pss:/ {print $2}' "/proc/$p/smaps_rollup" 2>/dev/null || echo 0)
  cpu=$(ps -p "$p" -o %cpu= 2>/dev/null | tr -d ' ')
  cmd=$(tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null | cut -c1-110)
  printf '%-8s %-10s %-10s %-8s %s\n' "$p" "${rss:-0}" "${pss:-0}" "${cpu:-0}" "${cmd:-[exited]}"
done <<< "$pids"
printf '\nWebKit process tree:\n'
ps -eo pid,ppid,rss,%cpu,comm,args --sort=-rss | grep -E 'reedbrowser|WebKit(WebProcess|NetworkProcess|GPUProcess)' | grep -v grep || true
