#!/bin/bash
# The macOS front door end to end (process-contract.md). A fake launcher that
# the host runs, and LaunchServices never started, launches fronts through
# NSWorkspace and checks each fake client scenario's ExitKind, single instance
# and data.lock. Then the launcher is killed under a running front, which must
# quit by itself. Needs a PARTIES_UPDATE_TESTING build and a logged-in GUI
# session.
#
#   front_door_e2e.sh <built Parties.app> <entitlements> <fake client dylib> <fake launcher dylib> <output dir>
set -euo pipefail

built_app=$1
entitlements=$2
fake_client=$3
fake_launcher=$4
out=$5

rm -rf "$out"
mkdir -p "$out/logs" "$out/data" "$out/runtime"
app="$out/Parties.app"
ditto "$built_app" "$app"
host="$app/Contents/MacOS/Parties"
codesign --force --sign - --identifier org.parties.client.app "$app/Contents/Frameworks/PartiesClient.dylib"
codesign --force --sign - --options runtime --entitlements "$entitlements" "$app"

fail() {
    echo "::error::$*"
    exit 1
}

stop_all() {
    pkill -9 -f "$host" 2>/dev/null || true
}
trap stop_all EXIT

if [ "${GITHUB_ACTIONS:-}" = true ]; then
    # No "quit unexpectedly" dialog for the crash scenarios on the runner.
    defaults write com.apple.CrashReporter DialogType none
fi

common=(--fake-client="$fake_client" --install-root="$out/runtime" --data-dir="$out/data" --log-dir="$out/logs")

echo "== Scenarios through the front door"
rc=0
"$host" --role=fake-launcher="$fake_launcher" "${common[@]}" --test-namespace="e2e-$$" 2>&1 \
    | tee "$out/fake-launcher.log" || rc=$?
if [ "$rc" != 0 ]; then
    for f in "$out"/logs/*.log; do
        echo "-- $f"
        cat "$f"
    done
    fail "front door scenarios failed (exit $rc)"
fi
launcher=$(sed -n 's/^\[e2e\] fake launcher pid=\([0-9]*\),.*/\1/p' "$out/fake-launcher.log")
echo "-- the launcher's front-door trace"
cat "$out/logs/front-$launcher.log"

# ReportCrash writes the reports of the crash scenarios up to a minute later.
# Wait for them, so a later check for new Parties crash reports does not see them.
for scenario in crash abort; do
    pid=$(sed -n "s/^\[e2e\] $scenario: front pid=\([0-9]*\)$/\1/p" "$out/fake-launcher.log")
    report=
    for _ in $(seq 1 240); do
        report=$(grep -lE "\"pid\" ?: ?$pid[,}]" "$HOME"/Library/Logs/DiagnosticReports/Parties*.ips 2>/dev/null | head -n 1 || true)
        [ -n "$report" ] && break
        sleep 0.5
    done
    if [ -n "$report" ]; then
        echo "$scenario front $pid: crash report $report"
    else
        echo "::warning::no crash report for the $scenario front $pid within 120 s"
    fi
done

echo "== Launcher loss: kill -9 the launcher under a running front"
loss_log="$out/launcher-loss.log"
"$host" --role=fake-launcher="$fake_launcher" "${common[@]}" --test-namespace="loss-$$" --mode=launcher-loss \
    > "$loss_log" 2>&1 &
launcher_pid=$!
owner=
for _ in $(seq 1 900); do
    owner=$(sed -n 's/^\[e2e\] launcher-loss owner pid=\([0-9]*\) loaded$/\1/p' "$loss_log")
    [ -n "$owner" ] && break
    kill -0 "$launcher_pid" 2>/dev/null || break
    sleep 0.1
done
cat "$loss_log"
[ -n "$owner" ] || fail "the launcher-loss front never loaded"
kill -9 "$launcher_pid"
gone_ms=
for i in $(seq 1 400); do
    if ! kill -0 "$owner" 2>/dev/null; then
        gone_ms=$((i * 50))
        break
    fi
    sleep 0.05
done
owner_log="$out/logs/front-$owner.log"
echo "-- $owner_log"
cat "$owner_log"
[ -n "$gone_ms" ] || fail "front $owner outlived its launcher by more than 20 s"
echo "front $owner was gone about $gone_ms ms after its launcher was killed"
grep -q '^\[host\] launcher lost' "$owner_log" || fail "the front did not notice that its launcher was gone"
grep -q '^\[host\] terminate: exiting 0$' "$owner_log" || fail "the front did not quit through terminate:"

echo "front door e2e passed"
