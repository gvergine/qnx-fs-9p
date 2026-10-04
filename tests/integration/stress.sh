#!/bin/bash
# Stress: no hang or crash under parallel load (cp -r with
# diff, find, dd with checksums, the same binary run from the share by
# several processes at once, small-file churn), with
# security_model=mapped-xattr. Afterwards fs-9p must still serve, an
# untouched file must be intact, and the trace must show no leaked fids.
#
# usage: stress.sh [SECONDS]   (default 60)
# FS9P_MOUNT_OPTS adds fs-9p mount options (e.g. cache=2000).
# The exec loops need QNX_TARGET (toybox from the SDP) and are left out
# without it.
set -uo pipefail
. "$(dirname "$0")/lib.sh"

SHARE=$FS9P_VM/share
MNT=/mnt/host
secs=${1:-60}
TOYBOX=${QNX_TARGET:+$QNX_TARGET/aarch64le/usr/bin/toybox}

echo "== host tree"
rm -rf "$SHARE"
mkdir -p "$SHARE/tree" "$SHARE/bin"
cp -r "$FS9P_REPO/src" "$FS9P_REPO/lib9p" "$FS9P_REPO/docs" "$SHARE/tree/"
cp "$(dirname "$0")/stress-guest.sh" "$SHARE/"
head -c 4M /dev/urandom >"$SHARE/ref.bin"
HOST_REF=$(cksum <"$SHARE/ref.bin")
if [ -f "${TOYBOX:-}" ]; then
    install -m 755 "$TOYBOX" "$SHARE/bin/id"
else
    skip "exec loops (QNX_TARGET not set)"
fi

echo "== boot"
vm_boot -S mapped-xattr || exit 1
expect_rc "mount" "fs-9p ${FS9P_MOUNT_OPTS:+-o $FS9P_MOUNT_OPTS }hostshare $MNT" 0

echo "== load (${secs}s)"
out=$(vm_run "sh $MNT/stress-guest.sh $MNT $secs" $((secs + 900)))
rc=$?
sed 's/^/      /' <<<"$out"
if [ "$rc" = 124 ]; then
    fail "load finished" "timed out: hang?"
else
    pass "load finished"
fi
fails_seen=$(grep -c ' FAIL' <<<"$out")
if [ "$fails_seen" = 0 ]; then pass "no loop failed"; else fail "loops failed" "$(grep ' FAIL' <<<"$out")"; fi
for loop in cp1 cp2 dd1 dd2 small1 small2 find; do
    if grep -q "^$loop ok [1-9]" <<<"$out"; then pass "$loop ran"; else fail "$loop ran"; fi
done
if [ -f "${TOYBOX:-}" ]; then
    for loop in exec1 exec2 exec3; do
        if grep -q "^$loop ok [1-9]" <<<"$out"; then pass "$loop ran"; else fail "$loop ran"; fi
    done
fi

echo "== after"
expect "fs-9p still serves" "ls $MNT/tree | tr '\n' ' '" "docs lib9p src "
expect "untouched file intact" "cksum <$MNT/ref.bin" "$HOST_REF"
if wait_fids_released; then pass "fids released"; else fail "fids released" "$(trace_fids)"; fi
# find and ls race with deletions: Twalk ENOENT (2) is expected, and so is
# a Tgetattr (24) or Treadlink (22) ENOENT for a file or symlink removed
# between its walk and the next request.
check_trace ' id \(110 err 2\|24 err 2\|22 err 2\)$'

vm_stop
summary "STRESS"
