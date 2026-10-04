#!/bin/sh
# Parallel load on the share, run inside the guest; stress.sh copies it onto
# the share and checks the result. Each loop works on its own files, so any
# failure it reports is real; find and ls race with the other loops'
# deletions on purpose, and only have to keep running.
#
# usage: stress-guest.sh MNT SECONDS
# Prints "<loop> ok <iterations>" or "<loop> FAIL <what>" lines.
m=$1
secs=${2:-60}
end=$(($(date +%s) + secs))
w=$m/stress
rm -rf "$w"
mkdir -p "$w"
running() { [ "$(date +%s)" -lt $end ]; }

# Copy a source tree, compare it, remove it.
cpr() {
    n=0
    while running; do
        cp -r "$m/tree" "$w/cp.$1" || { echo "cp$1 FAIL cp"; return; }
        diff -r "$m/tree" "$w/cp.$1" >/dev/null || { echo "cp$1 FAIL diff"; return; }
        rm -r "$w/cp.$1" || { echo "cp$1 FAIL rm"; return; }
        n=$((n + 1))
    done
    echo "cp$1 ok $n"
}

# Write 4 MiB, check it, remove it.
ddl() {
    n=0
    want=$(dd if=/dev/zero bs=65536 count=64 2>/dev/null | cksum)
    while running; do
        dd if=/dev/zero of="$w/dd.$1" bs=65536 count=64 2>/dev/null || { echo "dd$1 FAIL write"; return; }
        [ "$(cksum <"$w/dd.$1")" = "$want" ] || { echo "dd$1 FAIL cksum"; return; }
        rm "$w/dd.$1" || { echo "dd$1 FAIL rm"; return; }
        n=$((n + 1))
    done
    echo "dd$1 ok $n"
}

# Run the same binary from the share as other loops do, concurrently.
execl() {
    n=0
    while running; do
        [ "$("$m/bin/id" -u)" = 0 ] || { echo "exec$1 FAIL"; return; }
        n=$((n + 1))
    done
    echo "exec$1 ok $n"
}

# Create, rename, link, remove small files.
small() {
    n=0
    d=$w/small.$1
    mkdir "$d" || { echo "small$1 FAIL mkdir"; return; }
    while running; do
        echo "$n" >"$d/a" && mv "$d/a" "$d/b" && ln -s b "$d/l" && [ "$(cat "$d/l")" = "$n" ] &&
            rm "$d/l" "$d/b" || { echo "small$1 FAIL at $n"; return; }
        n=$((n + 1))
    done
    rmdir "$d"
    echo "small$1 ok $n"
}

# Walk everything; errors from files vanishing meanwhile are expected.
findl() {
    n=0
    while running; do
        find "$m" >/dev/null 2>&1
        ls -lR "$w" >/dev/null 2>&1
        n=$((n + 1))
    done
    echo "find ok $n"
}

out=/tmp/stress.out
: >"$out"
cpr 1 >>"$out" 2>&1 &
cpr 2 >>"$out" 2>&1 &
ddl 1 >>"$out" 2>&1 &
ddl 2 >>"$out" 2>&1 &
if [ -x "$m/bin/id" ]; then
    execl 1 >>"$out" 2>&1 &
    execl 2 >>"$out" 2>&1 &
    execl 3 >>"$out" 2>&1 &
fi
small 1 >>"$out" 2>&1 &
small 2 >>"$out" 2>&1 &
findl >>"$out" 2>&1 &
wait
sort "$out"
