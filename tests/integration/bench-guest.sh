#!/bin/sh
# Filesystem benchmark run inside the guest; bench.sh copies it onto the
# share and runs it for each target directory. Prints one line per metric.
#
# usage: bench-guest.sh DIR [MIB]
#   Sequential write and read of a MIB-MiB file (default 32) with 1 MiB,
#   64 KiB and 4 KiB requests, then small-file operations on 500 files.
# Timing uses date +%s.%N; the arithmetic is done by awk.
d=$1
mib=${2:-32}
bytes=$((mib * 1048576))
nfiles=500

now() { date +%s.%N; }
# rate NAME BYTES START END
rate() {
    awk -v n="$1" -v by="$2" -v a="$3" -v b="$4" \
        'BEGIN { printf "%-22s %9.1f MB/s\n", n, by / (b - a) / 1048576 }'
}
# ops NAME COUNT START END
ops() {
    awk -v n="$1" -v c="$2" -v a="$3" -v b="$4" \
        'BEGIN { printf "%-22s %9.0f ops/s\n", n, c / (b - a) }'
}

for bs in 1048576 65536 4096; do
    count=$((bytes / bs))
    s=$(now)
    dd if=/dev/zero of="$d/bench.bin" bs=$bs count=$count 2>/dev/null
    e=$(now)
    rate "write bs=$bs" $bytes "$s" "$e"
    s=$(now)
    dd if="$d/bench.bin" of=/dev/null bs=$bs 2>/dev/null
    e=$(now)
    rate "read bs=$bs" $bytes "$s" "$e"
done
rm -f "$d/bench.bin"

if ! mkdir "$d/bench.d" 2>/dev/null; then
    echo "small-file tests skipped: no directories in $d"
    exit 0
fi
s=$(now)
i=0
while [ $i -lt $nfiles ]; do
    echo $i >"$d/bench.d/f$i"
    i=$((i + 1))
done
e=$(now)
ops "create small file" $nfiles "$s" "$e"
s=$(now)
ls -l "$d/bench.d" >/dev/null
e=$(now)
ops "stat (ls -l)" $nfiles "$s" "$e"
s=$(now)
ls -l "$d/bench.d" >/dev/null
e=$(now)
ops "stat again (ls -l)" $nfiles "$s" "$e"
s=$(now)
cat "$d/bench.d"/* >/dev/null
e=$(now)
ops "open+read+close" $nfiles "$s" "$e"
s=$(now)
rm -r "$d/bench.d"
e=$(now)
ops "unlink" $nfiles "$s" "$e"
