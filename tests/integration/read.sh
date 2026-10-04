#!/bin/bash
# Reading: a read-only mount of a host share (-o ro).
#
# Builds a test tree in <vm>/share, boots the guest, mounts it with fs-9p and
# checks ls -l, cat, cp, find, du, df, a multi-MB checksum, a directory
# larger than one Rreaddir, symlinks, read-only errors, running binaries
# from the share (setuid ignored), restarts, and the QEMU trace (no
# unexpected v9fs_rerror, no leaked fids).
#
# Prerequisites: the image in vm/ contains bin/fs-9p (see docs/dev/test-environment.md).
# The exec checks need QNX_TARGET (source qnxsdp-env.sh) and are skipped
# without it.
# security_model=none so host symlinks are readable (mapped-xattr can't read
# them: see docs/user/behaviour.md).
set -uo pipefail
. "$(dirname "$0")/lib.sh"

SHARE=$FS9P_VM/share
MNT=/mnt/host
# A QNX binary to run from the share. toybox picks its applet from argv[0],
# so a copy named "id" behaves as id.
TOYBOX=${QNX_TARGET:+$QNX_TARGET/aarch64le/usr/bin/toybox}

make_tree() {
    rm -rf "$SHARE"
    mkdir -p "$SHARE/sub/deeper/deepest" "$SHARE/bigdir"
    echo "Hello from the host" >"$SHARE/hello.txt"
    : >"$SHARE/empty.txt"
    head -c 8M /dev/urandom >"$SHARE/big.bin"
    for i in $(seq -w 1 2000); do echo "$i" >"$SHARE/bigdir/entry_$i"; done
    echo a >"$SHARE/sub/a.txt"
    echo b >"$SHARE/sub/deeper/b.txt"
    head -c 100000 /dev/urandom >"$SHARE/sub/deeper/deepest/c.bin"
    ln -s hello.txt "$SHARE/link_to_hello"
    ln -s sub "$SHARE/link_to_sub"
    touch "$SHARE/$(printf 'n%.0s' $(seq 1 200))"
    if [ -f "${TOYBOX:-}" ]; then
        mkdir -p "$SHARE/suid" "$SHARE/sgid"
        install -m 4755 "$TOYBOX" "$SHARE/suid/id"
        install -m 2755 "$TOYBOX" "$SHARE/sgid/id"
    fi
}

echo "== host tree"
make_tree
HOST_BIG=$(cksum <"$SHARE/big.bin")
HOST_C=$(cksum <"$SHARE/sub/deeper/deepest/c.bin")
HOST_FIND=$(find "$SHARE" | wc -l)
HOST_KB=$(($(stat -f -c '%b * %S' "$SHARE") / 1024))

echo "== boot"
vm_boot -S none || exit 1
expect_rc "mount" "fs-9p -o ro hostshare $MNT" 0

echo "== reads"
expect "cat small file" "cat $MNT/hello.txt" "Hello from the host"
expect "cat empty file" "cat $MNT/empty.txt | wc -c | tr -d ' '" "0"
expect "8 MiB checksum" "cksum <$MNT/big.bin" "$HOST_BIG"
expect "cp host->guest" "cp $MNT/big.bin /tmp/big.bin && cksum </tmp/big.bin" "$HOST_BIG"
expect "lseek via dd skip" "dd if=$MNT/hello.txt bs=1 skip=6 count=4 2>/dev/null" "from"

echo "== directories"
expect "2000-entry dir" "ls $MNT/bigdir | wc -l | tr -d ' '" "2000"
expect "dir order ends" "ls $MNT/bigdir | tail -1" "entry_2000"
expect "find count" "find $MNT | wc -l | tr -d ' '" "$HOST_FIND"
expect "find subtree" "find $MNT/sub | sort | tr '\n' ' '" \
    "$MNT/sub $MNT/sub/a.txt $MNT/sub/deeper $MNT/sub/deeper/b.txt $MNT/sub/deeper/deepest $MNT/sub/deeper/deepest/c.bin "
expect_rc "du runs" "du -sk $MNT >/dev/null" 0
expect_rc "ls -l runs" "ls -l $MNT >/dev/null" 0
expect "long name" "ls $MNT | grep -c '^nnnnnnnnnn'" "1"

echo "== df"
# Guest-side \$ is expanded by the guest shell, not here.
expect "df columns" "set -- \$(df -n $MNT | tail -1); echo \$1 \$2 \$3" "hostshare $MNT 9p"
expect "df flags" "df -g $MNT | grep -c 'Flags.*\[nosuid, rdonly\]'" "1"
# QEMU scales the block size to fit msize (520192 bytes at 512 KiB) and
# rounds the block counts down to it, so allow 1 MiB.
guest_kb=$(vm_run "set -- \$(df -P -k $MNT | tail -1); echo \$2" 60)
if [[ $guest_kb =~ ^[0-9]+$ ]] && ((guest_kb - HOST_KB < 1024 && HOST_KB - guest_kb < 1024)); then
    pass "df total size"
else
    fail "df total size" "guest ${guest_kb} KiB, host ${HOST_KB} KiB"
fi

echo "== symlinks"
expect "lstat shows link" "ls -l $MNT/link_to_hello | cut -c1" "l"
expect "readlink" "readlink $MNT/link_to_hello" "hello.txt"
expect "follow file link" "cat $MNT/link_to_hello" "Hello from the host"
expect "follow dir link" "ls $MNT/link_to_sub | tr '\n' ' '" "a.txt deeper "
expect "through dir link" "cksum <$MNT/link_to_sub/deeper/deepest/c.bin" "$HOST_C"

echo "== errors"
expect_rc "missing file" "cat $MNT/nosuchfile 2>/dev/null" 1
expect "write is EROFS" "(echo x >$MNT/new.txt) 2>&1 | grep -c 'Read-only'" "1"
expect "rm is EROFS" "rm -f $MNT/hello.txt 2>&1 | grep -c 'Read-only'" "1"
expect "mkdir is EROFS" "mkdir $MNT/d 2>&1 | grep -c 'Read-only'" "1"
expect_rc "file as dir" "ls $MNT/hello.txt/x 2>/dev/null" 1

echo "== exec"
if [ -f "${TOYBOX:-}" ]; then
    # Run as root. The files belong to the host user, so honored setuid or
    # setgid bits would change the effective ids.
    expect "setuid bit shown" "ls -l $MNT/suid/id | cut -c1-4" "-rws"
    expect "exec, setuid ignored" "$MNT/suid/id -u" "0"
    expect "exec, setgid ignored" "$MNT/sgid/id -g" "0"
    # A mapped binary has two OCBs on one node; closing both used to abort.
    if wait_fids_released; then pass "mapping fid released"; else fail "mapping fid released" "$(trace_fids)"; fi
    expect "alive after exec" "cat $MNT/hello.txt" "Hello from the host"
else
    skip "exec checks (QNX_TARGET not set)"
fi

echo "== restart"
# One fs-9p per tag while it lives; a killed one's device is taken over.
expect "second instance busy" "fs-9p hostshare /mnt/two 2>&1" "fs-9p: hostshare: Resource busy"
expect "claim name" "ls /dev/name/local/fs-9p" "hostshare"
expect "restart after slay" "slay -f fs-9p; sleep 1; fs-9p -o ro hostshare $MNT && cat $MNT/hello.txt" \
    "Hello from the host"
# SIGKILL while a read is (probably) in flight; the subshell keeps job
# control notices out of the output.
expect "restart after SIGKILL" "(cksum <$MNT/big.bin >/dev/null 2>&1 &); sleep 0.3; \
slay -s KILL -f fs-9p; sleep 1; fs-9p -o ro hostshare $MNT && cksum <$MNT/big.bin" "$HOST_BIG"

echo "== trace"
# Expected errors: Twalk ENOENT (2) for the missing names probed above.
check_trace ' id 110 err 2$'

vm_stop
summary "READ"
