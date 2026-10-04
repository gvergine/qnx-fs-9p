#!/bin/bash
# The metadata cache (-o cache=MS): host changes stay hidden for up to MS
# milliseconds and show afterwards, changes made through fs-9p show at
# once, permissions are still checked, and a repeated ls -l is answered
# without walks.
#
# Prerequisites: the image in vm/ contains bin/fs-9p (see docs/dev/test-environment.md).
set -uo pipefail
. "$(dirname "$0")/lib.sh"

SHARE=$FS9P_VM/share
MNT=/mnt/host

echo "== host tree"
rm -rf "$SHARE"
mkdir -p "$SHARE/many" "$SHARE/priv"
echo 12345 >"$SHARE/cached.txt"
for i in $(seq 1 100); do echo "$i" >"$SHARE/many/f$i"; done
echo p >"$SHARE/priv/f"

echo "== boot"
vm_boot -S none || exit 1

echo "== metadata cache (cache=3000)"
expect_rc "mount with the cache" "fs-9p -o cache=3000 hostshare $MNT" 0
# Host changes stay hidden for up to 3 s: proof that the cache answers.
expect "size cached" "ls -l $MNT/cached.txt | awk '{print \$5}'" "6"
expect "missing name cached" "ls $MNT/later.txt 2>&1 | grep -c 'No such'" "1"
echo more >>"$SHARE/cached.txt"
echo later >"$SHARE/later.txt"
expect "host change hidden within the cache time" "ls -l $MNT/cached.txt | awk '{print \$5}'" "6"
expect "host create hidden within the cache time" "cat $MNT/later.txt 2>&1 | grep -c 'No such'" "1"
sleep 4
expect "host change shows after the cache time" "ls -l $MNT/cached.txt | awk '{print \$5}'" "11"
expect "host create shows after the cache time" "cat $MNT/later.txt" "later"
# Changes made through fs-9p show at once.
expect "own write shows at once" "echo x >> $MNT/cached.txt; ls -l $MNT/cached.txt | awk '{print \$5}'" "13"
expect "own create over a cached miss" "ls $MNT/new2 2>/dev/null; echo y > $MNT/new2; cat $MNT/new2" "y"
expect "own rm shows at once" "rm $MNT/new2; ls $MNT/new2 2>&1 | grep -c 'No such'" "1"
expect "own mv shows at once" \
    "mv $MNT/cached.txt $MNT/moved.txt; ls $MNT/cached.txt 2>&1 | grep -c 'No such'; ls -l $MNT/moved.txt | awk '{print \$5}'" \
    "1
13"
expect "own chmod shows at once" "chmod 600 $MNT/moved.txt; ls -l $MNT/moved.txt | cut -c1-10" "-rw-------"
expect "renamed dir drops its children" \
    "mkdir $MNT/d; echo f > $MNT/d/f; ls -l $MNT/d/f >/dev/null; mv $MNT/d $MNT/d2; cat $MNT/d/f 2>&1 | grep -c 'No such'; cat $MNT/d2/f" \
    "1
f"
# With security_model=none the host user (uid 1000, qnxuser) owns
# everything: user1 is "other".
expect_rc "a dir only its owner may search" "chmod 700 $MNT/priv" 0
expect "search permission still checked" \
    "on -u user1 sh -c 'cat $MNT/priv/f; cat $MNT/priv/f' 2>&1 | grep -c 'Permission denied'" "2"
vm_run "ls -l $MNT/many >/dev/null" 120 >/dev/null
w1=$(grep -c '^v9fs_walk ' "$FS9P_VM/qemu-9p.log")
vm_run "ls -l $MNT/many >/dev/null" 120 >/dev/null
w2=$(grep -c '^v9fs_walk ' "$FS9P_VM/qemu-9p.log")
if [ $((w2 - w1)) -lt 5 ]; then
    pass "a repeated ls -l of 100 files is served from the cache ($((w2 - w1)) walks)"
else
    fail "a repeated ls -l of 100 files is served from the cache" "$((w2 - w1)) walks"
fi

echo "== trace"
check_trace ' id 110 err 2$'
vm_stop
summary "CACHE"
