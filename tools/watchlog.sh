#!/bin/sh
# Watch cotwvr.log for a pattern, exit 0 when it appears, 1 on timeout.
#
#   tools/watchlog.sh 'OpenXR ready|init failed' 120
#
# Use the POSIX path, NOT "$LOCALAPPDATA/...".  In Git Bash that variable holds
# a Windows path with backslashes, and the obvious fix - ${VAR//\\//} - DELETES
# the backslashes rather than converting them, silently producing
# "...AppData\LocaltheHunterCotWVRcotwvr.log".  That mistake cost three watchers
# that reported "timeout" while the log sat there with the answer in it.

LOG="/c/Users/$USERNAME/AppData/Local/theHunterCotWVR/cotwvr.log"
PATTERN="${1:-OpenXR ready|init failed|failed:}"
TIMEOUT="${2:-120}"

if [ ! -f "$LOG" ]; then
    echo "waiting for $LOG to appear..."
fi

i=0
while [ "$i" -lt "$TIMEOUT" ]; do
    if [ -f "$LOG" ] && grep -qE "$PATTERN" "$LOG" 2>/dev/null; then
        echo "--- matched after ${i}s ---"
        grep -E "$PATTERN" "$LOG"
        exit 0
    fi
    i=$((i + 1))
    sleep 1
done

echo "TIMEOUT after ${TIMEOUT}s - pattern '$PATTERN' never appeared"
echo "(log exists: $([ -f "$LOG" ] && echo yes || echo NO))"
exit 1
