#!/bin/bash
# The macOS client runs as a payload the host executable loads, when
# LaunchServices starts the bundle. Needs a PARTIES_UPDATE_TESTING build and a
# logged-in GUI session.
#
#   host_payload_probe.sh <built Parties.app> <entitlements> <output dir>
set -euo pipefail

built_app=$1
entitlements=$2
out=$3
bundle_id=org.parties.client

rm -rf "$out"
mkdir -p "$out"
app="$out/Parties.app"
ditto "$built_app" "$app"
host="$app/Contents/MacOS/Parties"
payload="$app/Contents/Frameworks/PartiesClient.dylib"
log="$out/front.stderr.log"

fail() {
    echo "::error::$*"
    exit 1
}

# $1: extra codesign options, empty for no hardened runtime.
sign() {
    codesign --force --sign - $1 --identifier "$bundle_id.app" "$payload"
    codesign --force --sign - $1 --entitlements "$entitlements" "$app"
    codesign --verify --strict "$app"
    codesign --display --verbose=2 "$app" 2>&1 | grep -E '^(Identifier|CodeDirectory|Signature|TeamIdentifier)'
    codesign --display --verbose=2 "$payload" 2>&1 | grep -E '^(Identifier|CodeDirectory)'
}

self_test() {
    local name=$1 file=$2 rc=0
    "$host" --self-test="$file" > "$out/self-test-$name.log" 2>&1 || rc=$?
    echo "self-test $name: exit $rc"
    sed 's/^/    /' "$out/self-test-$name.log"
    return "$rc"
}

# Capture device init blocks on the microphone prompt, which nobody answers on
# a CI runner. GitHub's runner VMs let root write the TCC databases.
grant_tcc() {
    local now db service
    now=$(date +%s)
    for db in "$HOME/Library/Application Support/com.apple.TCC/TCC.db" \
              "/Library/Application Support/com.apple.TCC/TCC.db"; do
        for service in kTCCServiceMicrophone kTCCServiceCamera; do
            if sudo sqlite3 "$db" "INSERT OR REPLACE INTO access
                    (service, client, client_type, auth_value, auth_reason, auth_version, flags, last_modified)
                    VALUES ('$service', '$bundle_id', 0, 2, 3, 1, 0, $now);"; then
                echo "TCC grant $service in $db: inserted"
            else
                echo "TCC grant $service in $db: insert failed"
                sudo sqlite3 "$db" ".schema access" || true
            fi
            echo "    row: $(sudo sqlite3 "$db" "SELECT service, client, client_type, auth_value, auth_reason
                FROM access WHERE service = '$service' AND client = '$bundle_id';" 2>&1)"
        done
    done
    sudo pkill -x tccd || true
}

logged_front_pid() {
    sed -n 's/^\[host\] front pid=\([0-9]*\).*/\1/p' "$log" | head -n 1
}

# $1: file name for the full sample. Prints the main thread's call graph.
sample_main_thread() {
    local pid
    pid=$(logged_front_pid)
    [ -n "$pid" ] || return 0
    echo "-- main thread of $pid"
    sample "$pid" 3 -file "$out/$1" > /dev/null 2>&1 || true
    awk '/^Call graph:/ {on=1} on && /^ +[0-9]+ Thread_/ && ++threads > 1 {exit} on' "$out/$1" 2>/dev/null | head -n 120 || true
}

# What the front's main thread and the screen show when it never got healthy.
diagnose_front() {
    echo "-- displays"
    system_profiler SPDisplaysDataType 2>&1 | tee "$out/displays.txt" || true
    echo "-- screenshot"
    screencapture -x "$out/screen.png" 2>&1 && ls -l "$out/screen.png" || true
    sample_main_thread sample.txt
}

stop_app() {
    pkill -f "$host" 2>/dev/null || true
}
trap stop_app EXIT

echo "== Library validation: hardened runtime, ad-hoc signatures"
sign "--options runtime"
lv_dir=$(mktemp -d "${TMPDIR:-/tmp}/parties-lv.XXXXXX")
cp "$payload" "$lv_dir/PartiesClient.dylib"
cp "$built_app/Contents/Frameworks/PartiesClient.dylib" "$lv_dir/linker-signed.dylib"
codesign --display --verbose=2 "$lv_dir/linker-signed.dylib" 2>&1 | grep -E '^(Identifier|CodeDirectory|Signature)' || true
hardened=1
self_test bundle-hardened "$payload" || hardened=0
self_test tmpdir-adhoc-hardened "$lv_dir/PartiesClient.dylib" || true
self_test tmpdir-linker-signed-hardened "$lv_dir/linker-signed.dylib" || true
if [ "$hardened" = 0 ]; then
    echo "::warning::hardened runtime with ad-hoc signatures refused the payload in the bundle; the app runs without the hardened runtime"
    sign ""
    self_test bundle-plain "$payload" || fail "the payload does not load from the bundle even without the hardened runtime"
fi

echo "== Microphone and camera access for $bundle_id"
if [ "${GITHUB_ACTIONS:-}" = true ]; then
    grant_tcc
else
    echo "not on GitHub Actions; leaving TCC alone, answer the prompts by hand"
fi

echo "== Launch through LaunchServices (hardened runtime: $hardened)"
: > "$log"
touch "$out/started"
started=$(date "+%Y-%m-%d %H:%M:%S")
open -W --stdout "$out/front.stdout.log" --stderr "$log" --env PARTIES_TEST_EXIT_AFTER_HEALTHY_MS=10000 "$app" &
open_pid=$!

healthy=0
loaded_at=
slow_start=
sampled=0
for _ in $(seq 1 1800); do
    if grep -q '^\[update-testing\] healthy' "$log"; then healthy=1; break; fi
    kill -0 "$open_pid" 2>/dev/null || break
    if [ -z "$loaded_at" ] && grep -q '^\[host\] payload loaded' "$log"; then loaded_at=$SECONDS; fi
    if [ -n "$loaded_at" ] && [ "$sampled" = 0 ] && [ $((SECONDS - loaded_at)) -ge 10 ]; then
        sampled=1
        slow_start=$(sample_main_thread slow-start-sample.txt)
    fi
    sleep 0.1
done
echo "-- front stderr so far"
cat "$log"
if [ "$sampled" = 1 ]; then
    echo "-- not healthy 10 s after the payload loaded (recorded, not asserted)"
    echo "$slow_start"
fi
if [ "$healthy" = 0 ]; then
    echo "-- front stdout (tail)"
    tail -n 80 "$out/front.stdout.log" || true
    diagnose_front
    fail "the app did not report healthy within 180 s"
fi

front_pid=$(logged_front_pid)
healthy_pid=$(sed -n 's/^\[update-testing\] healthy pid=\([0-9]*\).*/\1/p' "$log" | head -n 1)
[ -n "$front_pid" ] || fail "the host did not log its pid"
[ "$front_pid" = "$healthy_pid" ] || fail "host pid $front_pid and payload pid $healthy_pid differ"
echo "front pid $front_pid runs $(ps -o comm= -p "$front_pid")"
lsof -p "$front_pid" 2>/dev/null | grep -F PartiesClient.dylib || fail "PartiesClient.dylib is not mapped into the front"

echo "== LaunchServices registrations for $bundle_id"
lsappinfo find "bundleid=$bundle_id" | tee "$out/lsappinfo-find.txt"
asns=$(grep -oE 'ASN:0x[0-9a-fA-F]+-0x[0-9a-fA-F]+' "$out/lsappinfo-find.txt" || true)
count=$(printf '%s' "$asns" | grep -c . || true)
echo "registrations: $count"
[ "$count" = 1 ] || fail "expected one LaunchServices registration for $bundle_id, found $count"
lsappinfo info "$asns:" | tee "$out/lsappinfo-info.txt"
ls_pid=$(lsappinfo info -only pid "$asns:" | sed -n 's/.*"pid"=\([0-9]*\).*/\1/p')
echo "LaunchServices pid: $ls_pid"
[ "$ls_pid" = "$front_pid" ] || fail "LaunchServices registered pid $ls_pid, the front is $front_pid"

echo "== Screen while healthy"
screencapture -x "$out/screen-healthy.png" && ls -l "$out/screen-healthy.png" || echo "screencapture failed"

echo "== Quit after healthy"
for _ in $(seq 1 600); do
    kill -0 "$open_pid" 2>/dev/null || break
    sleep 0.1
done
kill -0 "$open_pid" 2>/dev/null && fail "the app did not quit within 60 s of reporting healthy"
open_rc=0
wait "$open_pid" || open_rc=$?
echo "open -W exit: $open_rc"
cat "$log"
[ "$open_rc" = 0 ] || fail "open -W failed with $open_rc"
kill -0 "$front_pid" 2>/dev/null && fail "the front is still running"

grep -q '^\[update-testing\] lobby loaded' "$log" || fail "the lobby document did not load"
grep -Eq '^\[update-testing\] (frame presented|frames completed in a visible window, presentedTime unavailable|rendering suspended)$' "$log" \
    || fail "no frame was presented and the view was not paused"
grep -q '^\[host\] terminate: exiting 0$' "$log" || fail "the app did not quit through terminate:"
crashes=$(find "$HOME/Library/Logs/DiagnosticReports" -name 'Parties*' -newer "$out/started" 2>/dev/null || true)
[ -z "$crashes" ] || fail "crash reports: $crashes"

echo "== launchd's view of the exit (recorded, not asserted)"
log show --start "$started" --style compact \
    --predicate "process == \"launchd\" AND eventMessage CONTAINS \"$bundle_id\"" 2>&1 | tail -n 20 | tee "$out/launchd.log" || true

echo "host payload probe passed"
