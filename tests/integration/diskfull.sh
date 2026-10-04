#!/bin/bash
# Disk full: the host disk under the share fills up.
#
# The share is a small filesystem mounted at <vm>/smallfs (or
# $FS9P_SMALLFS); mounting one needs root, so it is set up by hand once:
#
#   truncate -s 32M vm/smallfs.img
#   mkfs.ext4 -q -m 0 vm/smallfs.img
#   mkdir -p vm/smallfs
#   sudo mount -o loop vm/smallfs.img vm/smallfs
#   sudo chown "$USER:" vm/smallfs
#
# Checks: a write past the end fails with ENOSPC and keeps what fit, small
# writes and mkdir on the full disk fail cleanly, df shows it full, fs-9p
# keeps serving, and writing works again once space is freed. Then the
# trace checks (no unexpected v9fs_rerror, no leaked fids).
set -uo pipefail
. "$(dirname "$0")/lib.sh"

SMALL=${FS9P_SMALLFS:-$FS9P_VM/smallfs}
MNT=/mnt/host

if ! mountpoint -q "$SMALL"; then
    echo "$SMALL is not a mounted filesystem; see the comment at the top of $0" >&2
    exit 2
fi
size_kb=$(df --output=size -k "$SMALL" | tail -1 | tr -d ' ')
if [ "$size_kb" -gt 204800 ]; then
    echo "$SMALL is ${size_kb} KiB; refusing to fill anything over 200 MiB" >&2
    exit 2
fi
if [ ! -w "$SMALL" ]; then
    echo "$SMALL is not writable by $(id -un) (sudo chown \"\$USER:\" $SMALL)" >&2
    exit 2
fi

# host_expect NAME HOST_CMD EXPECTED_OUTPUT
host_expect() {
    local out
    out=$(eval "$2" 2>&1)
    if [ "$out" = "$3" ]; then pass "$1"; else fail "$1" "expected: $3
got:      $out"; fi
}
# Empty the filesystem (lost+found belongs to root and stays).
clean() {
    find "$SMALL" -mindepth 1 -maxdepth 1 ! -name lost+found -exec rm -rf {} +
}

echo "== host"
clean
echo hello >"$SMALL/hello.txt"
free_kb=$(df --output=avail -k "$SMALL" | tail -1 | tr -d ' ')
echo "   $SMALL: ${size_kb} KiB, ${free_kb} KiB free"

echo "== boot"
vm_boot -S mapped-xattr -s "$SMALL" || exit 1
expect_rc "mount" "fs-9p hostshare $MNT" 0

echo "== filling the disk"
# 8 MiB more than is free, in 1 MiB writes.
expect "write past the end fails with ENOSPC" \
    "dd if=/dev/zero of=$MNT/fill bs=1048576 count=$((free_kb / 1024 + 8)) 2>&1 | grep -c 'No space left'" "1"
fill=$(stat -c %s "$SMALL/fill" 2>/dev/null || echo 0)
if [ "$fill" -gt $(((free_kb - 2048) * 1024)) ]; then
    pass "what fit was kept (${fill} bytes)"
else
    fail "what fit was kept" "${fill} bytes written, ${free_kb} KiB were free"
fi
# ext4 refuses a large write while a few blocks are still free (it can't
# reserve enough for it); fill the rest with 4 KiB writes (observed: ~188
# KiB were left).
expect "topping up fails with ENOSPC too" \
    "dd if=/dev/zero of=$MNT/top bs=4096 count=100000 2>&1 | grep -c 'No space left'" "1"
expect "df shows the disk full" "set -- \$(df -P -k $MNT | tail -1); [ \$4 -lt 64 ] && echo full" "full"
expect "small write on the full disk fails" \
    "dd if=/dev/zero of=$MNT/more bs=4096 count=4 2>&1 | grep -c 'No space left'" "1"
expect "mkdir on the full disk fails" "mkdir $MNT/newdir 2>&1 | grep -c 'No space left'" "1"
expect "reads still work" "cat $MNT/hello.txt" "hello"
expect "listing still works" "ls $MNT | grep -c hello.txt" "1"

echo "== after freeing space"
expect_rc "rm the big files" "rm -f $MNT/fill $MNT/top $MNT/more" 0
expect "writes work again" "dd if=/dev/zero of=$MNT/again bs=1048576 count=4 2>/dev/null && wc -c <$MNT/again | tr -d ' '" "4194304"
expect_rc "mkdir works again" "mkdir $MNT/newdir2" 0
host_expect "host sees the new file" "stat -c %s $SMALL/again" "4194304"

echo "== trace"
# Expected errors: Twalk ENOENT (2) before creates; ENOSPC (28) from Twrite
# (118), Tlcreate (14), Tmkdir (72) and Tsetattr (26, the xattrs that
# mapped-xattr stores for a new file's owner).
check_trace ' id \(110 err 2\|118 err 28\|14 err 28\|72 err 28\|26 err 28\)$'

vm_stop
clean
summary "DISK FULL"
