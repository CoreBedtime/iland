#!/bin/sh
LOG=/tmp/exit-weston.log
echo "[exit] $(date) pid=$$ ppid=$PPID" >> "$LOG"
# Try PID file first (most reliable, no pkill pattern matching)
for f in /tmp/weston.pid /tmp/weston-runtime-$(id -u)/weston.pid "$XDG_RUNTIME_DIR/weston.pid" 2>/dev/null; do
  if [ -f "$f" ]; then echo "[exit] trying pidfile $f: $(cat "$f" 2>/dev/null)" >> "$LOG"; kill -TERM "$(cat "$f" 2>/dev/null)" 2>>"$LOG" || true; kill -9 "$(cat "$f" 2>/dev/null)" 2>>"$LOG" || true; sudo -n kill -TERM "$(cat "$f" 2>/dev/null)" 2>>"$LOG" || true; sudo -n kill -9 "$(cat "$f" 2>/dev/null)" 2>>"$LOG" || true; fi
done
for sig in TERM KILL; do
  /usr/bin/pkill -$sig -f "frontend/weston" 2>>"$LOG" || true
  /usr/bin/pkill -$sig weston 2>>"$LOG" || true
  if command -v sudo >/dev/null 2>&1; then sudo -n /usr/bin/pkill -$sig -f "frontend/weston" 2>>"$LOG" || true; sudo -n /usr/bin/pkill -$sig weston 2>>"$LOG" || true; fi
  [ "$sig" = "TERM" ] && sleep 0.3
done
ps aux | grep -i weston | grep -v grep >> "$LOG" 2>&1 || true
# Also terminate the parent run-weston.sh via its trap by killing weston PID
exit 0
