#!/bin/bash
# Benchmark: sequential throughput and small-file rates on the share,
# against the guest's own filesystems in the same VM (the QNX6 disk on
# virtio-blk and the RAM-backed /dev/shmem), since the test VM has no
# network for an scp baseline.
#
# usage: bench.sh [-m MSIZE[,MSIZE...]] [-o OPTS] [-S security_model] [-L] [-s MIB]
#   -m  msize values to mount the share with (default 524288)
#   -o  more fs-9p options, e.g. cache=1000
#   -S  QEMU security_model (default none)
#   -L  skip the local-filesystem baselines
#   -s  file size for the sequential tests in MiB (default 32; /data has
#       about 40 MB free in the default image)
set -uo pipefail
. "$(dirname "$0")/lib.sh"

SHARE=$FS9P_VM/share
MNT=/mnt/host
msizes=524288
opts=
model=none
locals=1
mib=32
while getopts "m:o:S:Ls:" o; do
    case $o in
    m) msizes=${OPTARG//,/ } ;;
    o) opts=,$OPTARG ;;
    S) model=$OPTARG ;;
    L) locals=0 ;;
    s) mib=$OPTARG ;;
    *) exit 2 ;;
    esac
done

rm -rf "$SHARE"
mkdir -p "$SHARE"
cp "$(dirname "$0")/bench-guest.sh" "$SHARE/"

vm_boot -S "$model" >/dev/null || exit 1
run() {
    echo "== $1"
    vm_run "sh $MNT/bench-guest.sh $2 $mib" 1800
}
vm_run "fs-9p hostshare $MNT" >/dev/null
if [ "$locals" = 1 ]; then
    vm_run "mkdir -p /data/bench /dev/shmem/bench" >/dev/null
    run "qnx6 disk (/data, virtio-blk)" /data/bench
    run "RAM (/dev/shmem)" /dev/shmem/bench
fi
for m in $msizes; do
    vm_run "slay -f fs-9p; sleep 1; fs-9p -o msize=$m$opts hostshare $MNT" >/dev/null
    run "9P share, msize $m$opts, security_model=$model" "$MNT"
done
vm_stop
