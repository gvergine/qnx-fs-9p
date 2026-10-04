#!/bin/bash
# Writing: a read-write mount.
#
# Builds a small tree in <vm>/share, boots the guest with
# security_model=mapped-xattr (QEMU's usual unprivileged model, which stores
# guest ownership and modes in xattrs) and checks the write path (create,
# append, overwrite, multi-MB copies, truncate, writes past 4 GiB, fsync,
# chmod/chown/utime with their permission rules, ownership of new files),
# namespace operations (mkdir, rmdir, rm, mv, symlinks, hard links, sticky
# directories, files removed while open), cross-development (cp -r both
# ways, a build-like workload compared with the same run on the host, a
# host-built binary run from the share) and the -o ro mount. Ends with the
# trace checks (no unexpected v9fs_rerror, no leaked fids).
#
# Prerequisites: the image in vm/ contains bin/fs-9p (see docs/dev/test-environment.md).
# The host-built binary check needs qcc (source qnxsdp-env.sh) and is
# skipped without it.
#
# FS9P_MOUNT_OPTS adds fs-9p mount options, e.g. FS9P_MOUNT_OPTS=cache=2000
# to run everything with the metadata cache on (the host-side changes are
# skipped then: they may show up only after the cache time).
#
# FS9P_SECURITY_MODEL picks QEMU's security_model (default mapped-xattr).
# mapped-file keeps metadata in .virtfs_metadata directories on the host,
# which host-side diffs ignore. passthrough needs QEMU running as root:
# run the whole script with sudo (see docs/dev/test-environment.md); the
# checks that the host refuses QEMU are skipped then, root bypasses them.
# Not none: there every file belongs to the host user (qnxuser in the guest,
# the same user the permission checks use as "someone else"), so the
# ownership and permission checks fail by design. read.sh covers none.
set -uo pipefail
. "$(dirname "$0")/lib.sh"

SHARE=$FS9P_VM/share
MNT=/mnt/host
OPTS=${FS9P_MOUNT_OPTS:+,$FS9P_MOUNT_OPTS}
MODEL=${FS9P_SECURITY_MODEL:-mapped-xattr}
if [ "$MODEL" = passthrough ] && [ "$(id -u)" != 0 ]; then
    echo "security_model=passthrough needs QEMU running as root: run this script with sudo" >&2
    exit 2
fi
# Host-side diffs: mapped-file's metadata directories are QEMU's, not ours.
HDIFF="diff -r -x .virtfs_metadata"

# host_expect NAME HOST_CMD EXPECTED_OUTPUT
host_expect() {
    local out
    out=$(eval "$2" 2>&1)
    if [ "$out" = "$3" ]; then pass "$1"; else fail "$1" "expected: $3
got:      $out"; fi
}

# Inputs for workload.sh: a small source tarball and a -p1 patch against it.
make_workload_inputs() {
    local t=$FS9P_VM/workload-src
    rm -rf "$t"
    mkdir -p "$t/src"
    for n in alpha beta gamma; do
        printf '/* %s */\nint %s(void) { return %d; }\n' "$n" "$n" "${#n}" >"$t/src/$n.c"
    done
    printf '#define VERSION "@VERSION@"\n' >"$t/src/version.h"
    tar --format=ustar -C "$t" -cf "$SHARE/in/src.tar" src
    cp -r "$t/src" "$t/new"
    sed -i 's/return 4;/return 40;/' "$t/new/beta.c"
    (cd "$t" && diff -u src/beta.c new/beta.c |
        sed -e 's|^--- src/|--- a/|' -e 's|^+++ new/|+++ b/|' >"$SHARE/in/change.patch") || true
    cp "$(dirname "$0")/workload.sh" "$SHARE/in/"
}

make_tree() {
    rm -rf "$SHARE" "$FS9P_VM/work-host"
    mkdir -p "$SHARE/qdir" "$SHARE/in" "$SHARE/tree" "$SHARE/bin"
    echo "Hello from the host" >"$SHARE/hello.txt"
    chmod 0755 "$SHARE" "$SHARE/qdir"
    # A real source tree for cp -r, plus a binary blob.
    cp -r "$FS9P_REPO/src" "$FS9P_REPO/lib9p" "$FS9P_REPO/docs" "$SHARE/tree/"
    head -c 300000 /dev/urandom >"$SHARE/tree/blob.bin"
    make_workload_inputs
    echo "hello from the host" >"$SHARE/in/msg.txt"
    if command -v qcc >/dev/null; then
        qcc -Vgcc_ntoaarch64le -o "$SHARE/bin/xdev" "$(dirname "$0")/progs/xdev.c"
    fi
}

echo "== host tree"
make_tree

echo "== boot"
echo "   (security_model=$MODEL${OPTS:+, mount options ${OPTS#,}})"
vm_boot -S "$MODEL" || exit 1
expect_rc "mount" "fs-9p -o rw$OPTS hostshare $MNT" 0

echo "== write"
expect "create and read" "echo hello > $MNT/w.txt && cat $MNT/w.txt" "hello"
host_expect "host sees it" "cat $SHARE/w.txt" "hello"
expect "umask applied" "umask 022; echo x > $MNT/m.txt; ls -l $MNT/m.txt | cut -c1-10" "-rw-r--r--"
expect "append" "echo more >> $MNT/w.txt; cat $MNT/w.txt | tr '\n' ' '" "hello more "
# Two descriptors appending in turn: the host fd is opened O_APPEND.
expect "append from two fds" \
    "exec 3>>$MNT/a.txt 4>>$MNT/a.txt; print -n 1 >&3; print -n 2 >&4; print -n 3 >&3; exec 3>&- 4>&-; cat $MNT/a.txt" \
    "123"
expect "overwrite truncates" "echo new > $MNT/w.txt; cat $MNT/w.txt" "new"
host_expect "host sees overwrite" "cat $SHARE/w.txt" "new"
expect "noclobber" "set -o noclobber; (echo z > $MNT/w.txt) 2>&1 | grep -c 'File exists'; set +o noclobber" "1"

GUEST_SUM=$(vm_run "dd if=/dev/urandom of=/tmp/r.bin bs=65536 count=128 2>/dev/null; cksum </tmp/r.bin" 300)
expect "8 MiB cp to share" "cp /tmp/r.bin $MNT/r.bin && cksum <$MNT/r.bin" "$GUEST_SUM"
host_expect "host checksum of cp" "cksum <$SHARE/r.bin" "$GUEST_SUM"
expect "8 MiB dd bs=1M" "dd if=/tmp/r.bin of=$MNT/r2.bin bs=1048576 2>/dev/null; cksum <$MNT/r2.bin" "$GUEST_SUM"
host_expect "host checksum of dd" "cksum <$SHARE/r2.bin" "$GUEST_SUM"

expect_rc "truncate shrink" "truncate -s 100 $MNT/r.bin" 0
host_expect "host size after shrink" "stat -c %s $SHARE/r.bin" "100"
expect_rc "truncate grow" "truncate -s 300000 $MNT/r.bin" 0
host_expect "host size after grow" "stat -c %s $SHARE/r.bin" "300000"

expect_rc "write past 4 GiB" "dd if=/dev/zero of=$MNT/s.bin bs=1 count=1 seek=5368709120 2>/dev/null" 0
host_expect "host size past 4 GiB" "stat -c %s $SHARE/s.bin" "5368709121"
expect "read back past 4 GiB" "dd if=$MNT/s.bin bs=1 skip=5368709120 2>/dev/null | wc -c | tr -d ' '" "1"

expect "fsync" "dd if=/dev/zero of=$MNT/f.bin bs=4096 count=1 conv=fsync 2>/dev/null && echo ok" "ok"
host_expect "fsync reached the server" "grep -c '^v9fs_fsync' $FS9P_VM/qemu-9p.log | awk '{print (\$1 > 0)}'" "1"

echo "== attributes"
expect "chmod" "chmod 640 $MNT/m.txt; ls -l $MNT/m.txt | cut -c1-10" "-rw-r-----"
# mapped-xattr stores the whole st_mode sent: the type bits must survive.
expect "chmod keeps a directory a directory" "chmod 1777 $MNT/qdir; ls -ld $MNT/qdir | cut -c1-10" "drwxrwxrwt"
expect_rc "touch -d" "touch -d '2020-01-02 03:04:05' $MNT/m.txt" 0
host_expect "host mtime" "stat -c %Y $SHARE/m.txt" "1577934245"

echo "== ownership"
expect_rc "hand qdir to qnxuser" "chown qnxuser:qnxuser $MNT/qdir && chmod 755 $MNT/qdir" 0
expect "root's new file is root's" "echo r > $MNT/byroot.txt; ls -l $MNT/byroot.txt | awk '{print \$3, \$4}'" "root root"
expect "creator owns a new file" \
    "on -u qnxuser sh -c 'echo q > $MNT/qdir/byq.txt'; ls -l $MNT/qdir/byq.txt | awk '{print \$3, \$4}'" \
    "qnxuser qnxuser"
expect "no create in another user's dir" \
    "on -u user1 sh -c 'echo u > $MNT/qdir/u.txt' 2>&1 | grep -c 'Permission denied'" "1"
expect "root chown" "chown user1:user2 $MNT/byroot.txt; ls -l $MNT/byroot.txt | awk '{print \$3, \$4}'" "user1 user2"
expect "non-root can't give a file away" \
    "on -u qnxuser sh -c 'chown user1 $MNT/qdir/byq.txt' 2>/dev/null; ls -l $MNT/qdir/byq.txt | awk '{print \$3}'" \
    "qnxuser"
expect "non-owner can't chmod" \
    "on -u qnxuser sh -c 'chmod 600 $MNT/byroot.txt' 2>/dev/null; ls -l $MNT/byroot.txt | cut -c1-10" "-rw-r--r--"

echo "== namespace"
N=$MNT/ns
expect_rc "mkdir" "mkdir $N && mkdir $N/d1 $N/d2 && mkdir -m 700 $N/priv" 0
expect "mkdir mode" "ls -ld $N/priv | cut -c1-10" "drwx------"
expect "rmdir" "rmdir $N/d2 && ls $N | tr '\n' ' '" "d1 priv "
expect "rmdir non-empty" "echo x > $N/d1/x; rmdir $N/d1 2>&1 | grep -c 'not empty'" "1"
expect "rmdir a file" "rmdir $N/d1/x 2>&1 | grep -c 'Not a directory'" "1"
expect "rm a directory" "rm $N/d1 2>&1 | grep -c 'Is a directory'" "1"
expect "rm" "rm $N/d1/x && ls $N/d1 | wc -l | tr -d ' '" "0"
expect "mv a file" "echo m > $N/m1; mv $N/m1 $N/m2 && cat $N/m2 && ls $N/m1 2>&1 | grep -c 'No such'" "m
1"
expect "mv across dirs" "mv $N/m2 $N/d1/m3 && cat $N/d1/m3" "m"
expect "mv replaces" "echo old > $N/r1; echo new > $N/r2; mv $N/r2 $N/r1 && cat $N/r1" "new"
expect "mv a directory" "mv $N/d1 $N/d3 && cat $N/d3/m3" "m"
expect "mv with a relative path" "cd $N/d3 && mv m3 ../m4 && cd / && cat $N/m4" "m"
expect "symlink, relative" "ln -s ../m4 $N/d3/sl && readlink $N/d3/sl && cat $N/d3/sl" "../m4
m"
expect "symlink, absolute" "ln -s /abs/target $N/sl2 && readlink $N/sl2" "/abs/target"
expect "rm a symlink" "rm $N/sl2 && ls $N/sl2 2>&1 | grep -c 'No such'" "1"
expect "hard link" "ln $N/m4 $N/hard && ls -l $N/m4 | awk '{print \$2}' && echo more >> $N/hard && cat $N/m4 | tr '\n' ' '" "2
m more "
expect "no hard link to a directory" "ln $N/d3 $N/dlink 2>/dev/null; echo \$?" "1"
expect "mkfifo refused" "mkfifo $N/fifo 2>&1 | grep -c 'Not supported'" "1"
expect "read after rm while open" "echo keep > $N/k; exec 3<$N/k; rm $N/k; cat <&3; exec 3<&-" "keep"
expect "write after rm while open" "echo w > $N/w; exec 4<>$N/w; rm $N/w; print -n data >&4 && echo ok; exec 4<&-" "ok"
expect "rm -r" "mkdir -p $N/t/a/b; echo 1 > $N/t/a/b/f; echo 2 > $N/t/g; rm -r $N/t && ls $N/t 2>&1 | grep -c 'No such'" "1"
# Paths through a symlinked directory: the client is redirected, as for open.
expect "create and remove through a dir symlink" \
    "ln -s d3 $N/dl && echo via > $N/dl/f && mkdir $N/dl/sub && cat $N/d3/f && rm $N/dl/f && rmdir $N/dl/sub && ls $N/d3 | tr '\n' ' '" \
    "via
sl "
# The old path of a rename and the existing file of a hard link come in the
# message's extra, which fs-9p can't redirect, but the C library resolves
# them through fs-9p first, so these are a real rename (the inode stays, not
# a copy) and a real link (two links).
expect "mv from behind a dir symlink" \
    "echo c > $N/d3/c; i=\$(ls -i $N/d3/c | awk '{print \$1}'); mv $N/dl/c $N/c2 && ls -i $N/c2 | awk -v i=\$i '{print \$1 == i}'" "1"
expect "ln from behind a dir symlink" \
    "echo l > $N/d3/l; ln $N/dl/l $N/l2 && ls -l $N/d3/l | awk '{print \$2}'; rm $N/d3/l $N/l2" "2"
expect_rc "hand ns to qnxuser" "chown qnxuser $N && chmod 1777 $N/d3" 0
expect "creator owns new dirs and symlinks" \
    "on -u qnxuser sh -c 'mkdir $N/qd; ln -s x $N/ql' && ls -ld $N/qd $N/ql | awk '{print \$3}' | tr '\n' ' '" \
    "qnxuser qnxuser "
expect "sticky dir protects others' files" \
    "on -u qnxuser sh -c 'echo t > $N/d3/byq'; on -u user1 sh -c 'rm -f $N/d3/byq' 2>&1 | grep -c 'not permitted'" "1"
expect "no mkdir in another user's dir" "on -u user1 sh -c 'mkdir $N/qd/x' 2>&1 | grep -c 'Permission denied'" "1"

echo "== search permission"
# Every directory on the way needs search (x) permission, not just the file.
S=$MNT/srch
expect_rc "setup" "mkdir -p $S/locked/sub && echo s > $S/locked/f && chmod 644 $S/locked/f && chmod 777 $S/locked/sub && chmod 700 $S/locked && ln -s f $S/locked/l" 0
expect "no read through a 0700 dir" "on -u qnxuser sh -c 'cat $S/locked/f' 2>&1 | grep -c 'Permission denied'" "1"
expect "no stat through a 0700 dir" "on -u qnxuser sh -c 'ls -l $S/locked/f' 2>&1 | grep -c 'Permission denied'" "1"
expect "no create below a 0700 dir" \
    "on -u qnxuser sh -c 'echo x > $S/locked/sub/new' 2>&1 | grep -c 'Permission denied'" "1"
# toybox readlink prints nothing when the call fails: check the status.
expect "no readlink through a 0700 dir" "on -u qnxuser sh -c 'readlink $S/locked/l; echo rc=\$?'" "rc=1"
expect "no rename below a 0700 dir" \
    "on -u qnxuser sh -c 'mv $S/locked/sub $S/moved' 2>&1 | grep -c 'Permission denied'" "1"
expect "root still reads" "cat $S/locked/f" "s"
expect_rc "0711: search but not list" "chmod 711 $S/locked" 0
expect "read through a 0711 dir" "on -u qnxuser sh -c 'cat $S/locked/f'" "s"
expect "no listing of a 0711 dir" "on -u qnxuser sh -c 'ls $S/locked' 2>&1 | grep -c 'Permission denied'" "1"

echo "== host-side changes and errors"
if [[ "$OPTS" == *cache=* ]]; then
    skip "host-side changes (the metadata cache delays them)"
else
    H=$MNT/host
    mkdir -p "$SHARE/host/hdir" "$SHARE/host/hcwd" "$SHARE/host/hro"
    echo secret >"$SHARE/host/hostonly"
    chmod 000 "$SHARE/host/hostonly"
    chmod 555 "$SHARE/host/hro"
    echo kept >"$SHARE/host/hdel"
    echo old >"$SHARE/host/hrep"
    echo f >"$SHARE/host/hfile"
    echo 12345 >"$SHARE/host/hsize"
    # QEMU runs as the host user: the host refuses what that user can't do,
    # even to guest root.
    if [ "$MODEL" = passthrough ]; then
        skip "host refusing QEMU (QEMU runs as root with passthrough)"
    else
        expect "host refuses a read" "cat $H/hostonly 2>&1 | grep -c 'Permission denied'" "1"
        expect "host refuses a create" "(echo x > $H/hro/f) 2>&1 | grep -c 'Permission denied'" "1"
    fi
    expect_rc "open, then the host deletes" "exec 3<$H/hdel" 0
    rm "$SHARE/host/hdel"
    expect "still readable after a host delete" "cat <&3; exec 3<&-" "kept"
    expect "gone by name" "ls $H/hdel 2>&1 | grep -c 'No such'" "1"
    expect_rc "open, then the host renames over it" "exec 3<$H/hrep" 0
    echo new >"$SHARE/host/hrep.tmp"
    mv "$SHARE/host/hrep.tmp" "$SHARE/host/hrep"
    expect "open fd keeps the old file" "cat <&3; exec 3<&-" "old"
    expect "a new open gets the new file" "cat $H/hrep" "new"
    rm -r "$SHARE/host/hdir"
    echo now-a-file >"$SHARE/host/hdir"
    expect "dir became a file: lookup" "ls $H/hdir/x 2>&1 | grep -c 'Not a directory'" "1"
    expect "dir became a file: create" "(echo y > $H/hdir/new) 2>&1 | grep -c 'Not a directory'" "1"
    rm "$SHARE/host/hfile"
    mkdir "$SHARE/host/hfile"
    expect "file became a dir: write" "(echo z > $H/hfile) 2>&1 | grep -c 'Is a directory'" "1"
    expect_rc "open, then the host appends" "exec 3<$H/hsize" 0
    echo more >>"$SHARE/host/hsize"
    expect "reads what the host appended" "cat <&3 | tr '\n' ' '; exec 3<&-" "12345 more "
    : >"$SHARE/host/hsize"
    expect "sees a host truncate" "wc -c <$H/hsize | tr -d ' '" "0"
    chmod 600 "$SHARE/host/hsize"
    expect "sees a host chmod" "ls -l $H/hsize | cut -c1-10" "-rw-------"
    expect_rc "cd into a dir" "cd $H/hcwd" 0
    rm -r "$SHARE/host/hcwd"
    expect "host removed the cwd" "ls . 2>&1 | grep -c 'No such'; cd /" "1"
    # The name is made in the guest: the console drops lines over 255 bytes.
    expect "name too long" "touch $MNT/\$(printf '%0300d' 0) 2>&1 | grep -c 'too long'" "1"

fi

echo "== exit criteria"
# The guest's /tmp is on its disk image and survives reboots: start clean.
expect "cp -r both ways, diff -r in the guest" \
    "rm -rf /tmp/tree && cp -r $MNT/tree /tmp/tree && cp -r /tmp/tree $MNT/tree2 && diff -r $MNT/tree /tmp/tree && echo same" "same"
host_expect "diff -r on the host" "$HDIFF $SHARE/tree $SHARE/tree2 && echo same" "same"
sh "$(dirname "$0")/workload.sh" "$SHARE/in" "$FS9P_VM/work-host" || fail "workload on the host"
expect "workload in the guest" "sh $MNT/in/workload.sh $MNT/in $MNT/work && echo ok" "ok"
host_expect "workload trees identical" "$HDIFF $FS9P_VM/work-host $SHARE/work && echo same" "same"
if [ -x "$SHARE/bin/xdev" ]; then
    expect "host-built binary writes to the share" \
        "$MNT/bin/xdev $MNT/in/msg.txt $MNT/xdev.out && echo ok" "ok"
    host_expect "host reads its output" "cat $SHARE/xdev.out" "host-built binary read: hello from the host"
else
    skip "host-built binary (qcc not on PATH)"
fi

echo "== read-only mount"
expect_rc "remount -o ro" "slay -f fs-9p; sleep 1; fs-9p -o ro$OPTS hostshare $MNT" 0
expect "ro: write refused" "(echo x > $MNT/w.txt) 2>&1 | grep -c 'Read-only'" "1"
expect "ro: chmod refused" "chmod 600 $MNT/hello.txt 2>&1 | grep -c 'Read-only'" "1"
expect "ro: df flags" "df -g $MNT | grep -c 'Flags.*\[nosuid, rdonly\]'" "1"
expect "ro: reads work" "cat $MNT/w.txt" "new"
expect_rc "remount rw" "slay -f fs-9p; sleep 1; fs-9p -o rw$OPTS hostshare $MNT" 0
expect "rw: df flags" "df -g $MNT | grep -c 'Flags.*\[nosuid\]'" "1"

echo "== trace"
# Expected errors: Twalk ENOENT (2) before creates and for missing names;
# Tunlinkat ENOTDIR (20), EISDIR (21) and ENOTEMPTY (39) from the rmdir/rm
# checks; Tlink EPERM (1) for the hard link to a directory; and the clients
# probing for existing names: Tmkdir EEXIST (17) from mkdir -p and tar,
# Tunlinkat ENOENT (2) from tar removing each path before extracting it;
# Tlopen and Tlcreate EACCES (13) where the host refuses QEMU.
check_trace ' id \(110 err 2\|76 err 2\|76 err 20\|76 err 21\|76 err 39\|70 err 1\|72 err 17\|12 err 13\|14 err 13\)$'

vm_stop
summary "WRITE"
