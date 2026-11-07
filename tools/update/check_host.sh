#!/bin/bash
# Automatic updates never replace the macOS host, so its sources stay small
# enough to audit, and it stays the only code that loads a payload.
set -euo pipefail

repo=$(cd "$(dirname "$0")/../.." && pwd)
max_lines=900
failures=0

lines=$(find "$repo/updater/host" -type f \( -name '*.h' -o -name '*.cpp' -o -name '*.mm' \) -exec cat {} + | wc -l | tr -d ' ')
echo "Host sources: $lines lines, limit $max_lines"
if [ "$lines" -gt "$max_lines" ]; then
    echo "::error::host sources have $lines lines, limit $max_lines"
    failures=1
fi

outside=$(git -C "$repo" grep --untracked -n -w dlopen -- '*.c' '*.cc' '*.cpp' '*.h' '*.hpp' '*.m' '*.mm' '*.swift' ':!updater/host/' || true)
if [ -n "$outside" ]; then
    echo "::error::dlopen appears outside updater/host:"
    echo "$outside"
    failures=1
else
    echo "dlopen appears only in updater/host"
fi

exit "$failures"
