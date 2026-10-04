#!/bin/bash
# Run every integration suite that doesn't need root, one after the other,
# and print a summary. The disk-full suite runs too when its small
# filesystem is mounted (docs/dev/test-environment.md); the passthrough run
# of write.sh needs QEMU as root and is left to be run by hand.
#
# usage: run-all.sh [LOG_DIR]     (default <vm>/test-logs)
# Each suite's output goes to LOG_DIR/<name>.log. Exit status 0 if all
# passed. Takes about 9 minutes.
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
vm=${FS9P_VM_DIR:-$repo/vm}
logs=${1:-$vm/test-logs}
smallfs=${FS9P_SMALLFS:-$vm/smallfs}
mkdir -p "$logs"

# The image holds a copy of the binary: catch a forgotten mkqnximage --build.
if [ "$repo/build-qnx/fs-9p" -nt "$vm/output/ifs.bin" ]; then
    echo "warning: build-qnx/fs-9p is newer than the image; run (cd vm && mkqnximage --build --noprompt)" >&2
fi
[ -n "${QNX_TARGET:-}" ] || echo "warning: QNX_TARGET not set (source qnxsdp-env.sh): some checks will be skipped" >&2

failed=0
results=()
# run NAME [VAR=VALUE...] SCRIPT [ARGS...]
run() {
    local name=$1
    shift
    local log=$logs/$name.log start rc last
    echo "== $name"
    start=$(date +%s)
    env "$@" >"$log" 2>&1
    rc=$?
    last=$(tail -1 "$log")
    results+=("$(printf '%-14s %4ss  %s' "$name" "$(($(date +%s) - start))" "$last")")
    [ "$rc" -eq 0 ] || failed=$((failed + 1))
}

run read             "$here/read.sh"
run write            "$here/write.sh"
run write-cache      FS9P_MOUNT_OPTS=cache=2000 "$here/write.sh"
run write-mapfile    FS9P_SECURITY_MODEL=mapped-file "$here/write.sh"
run robustness       "$here/robustness.sh"
run cache            "$here/cache.sh"
run stress           "$here/stress.sh"
run stress-cache     FS9P_MOUNT_OPTS=cache=2000 "$here/stress.sh"
if mountpoint -q "$smallfs" 2>/dev/null; then
    run diskfull "$here/diskfull.sh"
else
    results+=("$(printf '%-14s %5s  %s' diskfull - "not run: $smallfs is not mounted")")
fi

echo
echo "Summary (logs in $logs):"
printf '  %s\n' "${results[@]}"
echo "  passthrough: not run (needs QEMU as root, see docs/dev/test-environment.md)"
if [ "$failed" -eq 0 ]; then echo "ALL SUITES PASSED"; else echo "$failed SUITE(S) FAILED"; fi
exit $((failed > 0))
