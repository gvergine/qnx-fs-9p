# Shared helpers for integration tests. Source from bash:
#   . "$(dirname "$0")/lib.sh"
#
# Drives a headless guest started by run-qnx.sh through its serial socket.
# Needs socat on the host.

FS9P_REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
FS9P_VM=${FS9P_VM_DIR:-$FS9P_REPO/vm}
FS9P_RUN=$FS9P_REPO/tests/integration/run-qnx.sh
_vm_seq=0

# vm_boot [run-qnx.sh options] [-- extra qemu args]
# Restarts the guest headless and waits for the end of the startup script.
vm_boot() {
    "$FS9P_RUN" -k
    sleep 1
    "$FS9P_RUN" -H "$@" || return 1
    local i
    for i in $(seq 1 180); do
        if grep -q 'Startup complete' "$FS9P_VM/serial.log" 2>/dev/null; then
            echo "guest booted in about $i s"
            sleep 2
            # Console echo and ksh line editing interleave with command output
            # under emulation, so turn off echo, the prompt and line editing:
            # the log then holds only output and the exit markers.
            printf 'stty -echo; PS1=; set +o emacs +o gmacs +o vi\r' |
                socat - "UNIX-CONNECT:$FS9P_VM/serial.sock"
            sleep 1
            return 0
        fi
        sleep 1
    done
    echo "vm_boot: guest did not finish booting" >&2
    return 1
}

vm_stop() {
    "$FS9P_RUN" -k
}

# vm_run CMD [timeout_s]
# Runs CMD in the guest shell, prints its output, returns its exit status
# (or 124 on timeout). CMD must be a single line, and the line sent (CMD
# plus the marker) must fit the console's 255-byte input line: longer
# lines are dropped and the command never runs (observed), so they are
# refused here.
vm_run() {
    local cmd=$1 timeout=${2:-60}
    local log=$FS9P_VM/serial.log
    _vm_seq=$((_vm_seq + 1))
    local mark="__FS9P_RC_${_vm_seq}_$$"
    local line="$cmd; echo $mark=\$?"
    if [ "${#line}" -gt 254 ]; then
        echo "vm_run: command line too long for the guest console (${#line} > 254): ${cmd:0:60}..." >&2
        return 125
    fi
    local start
    start=$(stat -c %s "$log")
    printf '%s; echo %s=$?\r' "$cmd" "$mark" | socat - "UNIX-CONNECT:$FS9P_VM/serial.sock"
    local i out
    for i in $(seq 1 $((timeout * 4))); do
        out=$(tail -c +$((start + 1)) "$log" | tr -d '\r')
        # Not anchored to a line start: output without a trailing newline
        # (dd count=4, tr '\n' ' ') leaves the marker mid-line.
        if grep -qE "${mark}=[0-9]+" <<<"$out"; then
            # Echo is off (vm_boot), so everything before the marker is output.
            printf '%s' "${out%%"${mark}"=*}"
            return "$(grep -oE "${mark}=[0-9]+" <<<"$out" | head -1 | cut -d= -f2)"
        fi
        sleep 0.25
    done
    echo "vm_run: timeout after ${timeout}s: $cmd" >&2
    return 124
}

# Trace lines only (QEMU warnings share the -D file).
vm_trace() {
    grep '^v9fs_' "$FS9P_VM/qemu-9p.log"
}

# --- assertions ---------------------------------------------------------------

fails=0
skips=0

pass() { echo "PASS  $1"; }
fail() {
    echo "FAIL  $1"
    [ -n "${2:-}" ] && sed 's/^/      /' <<<"$2"
    fails=$((fails + 1))
}
skip() {
    echo "SKIP  $1"
    skips=$((skips + 1))
}

# expect NAME GUEST_CMD EXPECTED_OUTPUT
expect() {
    local out
    out=$(vm_run "$2" 300)
    if [ "$out" = "$3" ]; then pass "$1"; else fail "$1" "expected: $3
got:      $out"; fi
}

# expect_rc NAME GUEST_CMD EXPECTED_RC
expect_rc() {
    local out rc
    out=$(vm_run "$2" 120)
    rc=$?
    if [ "$rc" = "$3" ]; then pass "$1"; else fail "$1" "rc $rc (want $3): $out"; fi
}

# Live fids from the trace: attach and fully successful walks create fids,
# clunk releases them. Several requests can be in flight, so a walk is
# matched with its reply (walk_return or rerror) by tag. Tversion starts a
# new session, and QEMU drops every fid of the previous one (a restarted
# fs-9p). Prints "LIVE <fids>" and "BADCLUNK <fid>" for clunks of fids that
# were not live.
trace_fids() {
    vm_trace | awk '
        function field(name,   i, a) {
            for (i = 1; i <= NF; i++) {
                if ($i == name) return $(i + 1)
                if (index($i, name "=") == 1) { split($i, a, "="); return a[2] }
            }
            return ""
        }
        /^v9fs_version / { delete live; delete nf; delete want; next }
        /^v9fs_attach / { live[field("fid")] = 1; next }
        /^v9fs_walk / { t = field("tag"); nf[t] = field("newfid"); want[t] = field("nwnames"); next }
        /^v9fs_walk_return / {
            t = field("tag")
            if (t in nf && field("nwnames") == want[t]) live[nf[t]] = 1
            delete nf[t]; delete want[t]; next
        }
        /^v9fs_rerror / { t = field("tag"); delete nf[t]; delete want[t]; next }
        /^v9fs_clunk / {
            f = field("fid")
            if (!(f in live)) print "BADCLUNK " f
            delete live[f]
        }
        END { s = ""; for (f in live) s = s " " f; print "LIVE" s }'
}

# The memory manager keeps its mapping of an executed binary (and so a fid)
# for a while after the process exits; wait up to 30 s for only the root
# fid to be live.
wait_fids_released() {
    local i
    for i in $(seq 1 30); do
        [ "$(trace_fids | sed -n 's/^LIVE *//p')" = "0" ] && return 0
        sleep 1
    done
    return 1
}

# check_trace ALLOWED: no v9fs_rerror except lines matching the grep pattern
# ALLOWED, no clunk of an unknown fid, and only the root fid left live.
check_trace() {
    local unexpected fids live
    unexpected=$(vm_trace | grep '^v9fs_rerror' | grep -v -- "$1" || true)
    if [ -z "$unexpected" ]; then pass "no unexpected rerror"; else fail "unexpected rerror" "$unexpected"; fi
    fids=$(trace_fids)
    if grep -q BADCLUNK <<<"$fids"; then fail "clunk of unknown fid" "$fids"; else pass "no bad clunk"; fi
    live=$(grep '^LIVE' <<<"$fids" | sed 's/^LIVE *//')
    if [ "$live" = "0" ]; then pass "no leaked fids (only root fid 0 live)"; else fail "leaked fids" "$live"; fi
}

# summary NAME: print the result line and exit 0 (all passed) or 1.
summary() {
    local note=""
    [ "$skips" -gt 0 ] && note=" ($skips skipped)"
    echo
    if [ "$fails" -eq 0 ]; then echo "$1: ALL PASS$note"; else echo "$1: $fails FAILED$note"; fi
    exit $((fails > 0))
}
