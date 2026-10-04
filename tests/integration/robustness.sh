#!/bin/bash
# Robustness: interrupting long transfers, the clean unmount on
# SIGTERM/SIGINT, and request timeouts with recovery (on a second boot with
# the share's reads throttled, so one request outlives the timeout).
# Timings are compared with an uninterrupted run in the same VM, so they
# don't depend on the host's speed.
#
# Prerequisites: the image in vm/ contains bin/fs-9p (see docs/dev/test-environment.md).
# "{ cmd & } 2>/dev/null" starts a background job without the shell's job
# notice in the output, keeping job control (SIGINT not ignored).
set -uo pipefail
. "$(dirname "$0")/lib.sh"

SHARE=$FS9P_VM/share
MNT=/mnt/host

# elapsed GUEST_CMD: run it, print the seconds it took (guest clock).
elapsed() {
    vm_run "s=\$(date +%s.%N); $1; e=\$(date +%s.%N); awk -v a=\$s -v b=\$e 'BEGIN{print b-a}'" 300 |
        tail -1
}
# faster NAME T_INTERRUPTED T_FULL: the interrupted run took under 70%.
faster() {
    if awk -v i="$2" -v f="$3" 'BEGIN { exit !(i > 0 && i < 0.7 * f) }'; then
        pass "$1 (${2}s vs ${3}s)"
    else
        fail "$1" "interrupted ${2}s, uninterrupted ${3}s"
    fi
}

echo "== host tree"
rm -rf "$SHARE"
mkdir -p "$SHARE"
head -c 256M /dev/zero >"$SHARE/big"
head -c 128K /dev/zero >"$SHARE/slow.bin"
echo hello >"$SHARE/hello.txt"

echo "== boot"
vm_boot -S none || exit 1
# The subshell records fs-9p's exit status for the shutdown checks.
expect_rc "mount" "(fs-9p hostshare $MNT; echo status \$? > /tmp/st) & sleep 2" 0

echo "== interrupting long transfers"
# One 256 MiB read() or write() is ~500 Treads/Twrites: a signal must not
# wait for all of them.
full=$(elapsed "dd if=$MNT/big of=/dev/null bs=268435456 count=1 2>/dev/null")
int=$(elapsed "dd if=$MNT/big of=/dev/null bs=268435456 count=1 2>/dev/null & sleep 0.2; kill -INT \$!; wait")
faster "interrupted read returns early" "$int" "$full"
vm_run "dd if=/dev/zero of=/dev/shmem/z bs=1048576 count=256 2>/dev/null" >/dev/null
# The source is RAM, so the signal lands during the write itself.
full=$(elapsed "dd if=/dev/shmem/z of=$MNT/w bs=268435456 count=1 2>/dev/null")
int=$(elapsed "dd if=/dev/shmem/z of=$MNT/w bs=268435456 count=1 2>/dev/null & sleep 0.8; kill -INT \$!; wait")
faster "interrupted write returns early" "$int" "$full"
size=$(stat -c %s "$SHARE/w")
if [ "$size" -gt 0 ] && [ "$size" -lt 268435456 ]; then
    pass "interrupted write kept its prefix ($size bytes)"
else
    fail "interrupted write kept its prefix" "size $size"
fi
vm_run "rm /dev/shmem/z" >/dev/null

echo "== clean unmount"
expect "slay during a read: reader gets an error" \
    "{ dd if=$MNT/big of=/dev/null bs=1048576 2>/tmp/dd.err & } 2>/dev/null; sleep 0.3; slay fs-9p; wait; sleep 1; grep -c 'read error' /tmp/dd.err" \
    "1"
expect "fs-9p exited with status 0" "cat /tmp/st" "status 0"
expect "mount point removed" "ls $MNT 2>&1 | grep -c 'No such'" "1"
# A clean unmount resets the device: the next instance finds it at status 0.
expect "device was reset" \
    "{ fs-9p -o debug hostshare $MNT >/tmp/dbg.log 2>&1 & } 2>/dev/null; sleep 2; grep -c 'status 0x0\$' /tmp/dbg.log" "1"
expect "SIGINT unmounts too" "kill -INT \$!; wait; ls $MNT 2>&1 | grep -c 'No such'" "1"
expect_rc "mount again" "fs-9p hostshare $MNT" 0
expect "serves after the restarts" "wc -c <$MNT/big | tr -d ' '" "268435456"

echo "== trace"
check_trace ' id 110 err 2$'
vm_stop

echo "== timeouts (reads throttled to 16 KiB/s)"
# A 128 KiB Tread takes about 8 s, so with timeout=2000 it times out and
# its slot stays out of use until QEMU finishes it. dd's first read goes
# through on the throttle's burst allowance; the next one times out.
rm -f "$SHARE/w"
vm_boot -S none -F throttling.bps-read=16384 || exit 1
expect_rc "mount, one slot" "fs-9p -o timeout=2000,requests=1 hostshare $MNT" 0
expect "a request outliving the timeout fails" \
    "dd if=$MNT/slow.bin of=/dev/null bs=131072 2>&1 | grep -c 'timed out'" "1"
expect "no free slot: ETIMEDOUT, not a hang" "ls $MNT 2>&1 | grep -c 'timed out'" "1"
sleep 12
expect "the slot comes back when QEMU finishes" "ls $MNT/hello.txt" "$MNT/hello.txt"
expect_rc "remount, four slots" "slay fs-9p; sleep 2; fs-9p -o timeout=2000 hostshare $MNT" 0
expect "a request outliving the timeout fails (4 slots)" \
    "dd if=$MNT/slow.bin of=/dev/null bs=131072 2>&1 | grep -c 'timed out'" "1"
expect "other slots go on meanwhile" "ls $MNT/hello.txt" "$MNT/hello.txt"
sleep 12
if wait_fids_released; then pass "fids released after timeouts"; else fail "fids released after timeouts" "$(trace_fids)"; fi
check_trace ' id 110 err 2$'

vm_stop
summary "ROBUSTNESS"
