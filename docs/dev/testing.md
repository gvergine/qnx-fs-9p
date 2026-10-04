# Running the tests

Two kinds of tests: fast **unit tests** of `lib9p` on the host, and
**integration tests** that boot the QNX guest under QEMU, run commands in
it and check both the results and QEMU's 9P trace. The integration tests
need the environment from {doc}`test-environment`, with the image rebuilt
after the last `cmake --build build-qnx`. QEMU's trace is the independent
check that fs-9p speaks the protocol correctly. Everything runs on the
developer's machine: the integration tests need the SDP and QEMU, so there
is no CI (the unit tests alone could run anywhere).

## Unit tests

```
cmake -S . -B build-host -DFS9P_BUILD_TESTS=ON
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

| Test | What |
|---|---|
| `test_encode` | Golden byte vectors (Tversion and Tread from the spec), the field layout of every encoder, capacity limits, invalid arguments. |
| `test_decode` | Golden replies, the field layout of every decoder, framing (size, type, tag, trailing bytes), counts and lengths that overrun. |
| `test_idpool` | The fid/tag pool: allocation order, reuse, exhaustion, the tag range. |
| `test_fuzz` | Random and mutated replies into every decoder (must fail cleanly, never read past the end). Not coverage-guided (no libFuzzer on the host), but it reaches every line of `decode.c`. |
| `consistency` | Not a unit test: `CHANGELOG.md` has a section for the version in `CMakeLists.txt`, and every `-o` option in `main.c` appears in the `use` message and in the reference page ({doc}`docs`). |

All of them take under a second. Run them again under AddressSanitizer and
UndefinedBehaviorSanitizer before every commit that touches `lib9p`:

```
cmake -S . -B build-asan -DFS9P_BUILD_TESTS=ON -DFS9P_SANITIZE=ON
cmake --build build-asan && ctest --test-dir build-asan --output-on-failure
```

Line coverage (the decoder should stay at 100%: every error branch is a
malformed-reply case the fuzzer reaches):

```
cmake -S . -B build-cov -DFS9P_BUILD_TESTS=ON -DCMAKE_C_FLAGS=--coverage
cmake --build build-cov && ctest --test-dir build-cov
gcov -n build-cov/CMakeFiles/lib9p.dir/lib9p/src/*.c.gcda
```

## Integration tests

Each suite is a bash script in `tests/integration/` named after what it
tests, run from anywhere, with the SDP environment sourced (some checks
need `qcc` or `QNX_TARGET` and are skipped without them):

```
source ~/qnx800/qnxsdp-env.sh
tests/integration/write.sh
```

A suite stops any VM it finds running, boots the image headless with the
QEMU options it needs, mounts the share in the guest, prints one line per
check (`PASS`, `FAIL` with the expected and actual output, `SKIP`), checks
the trace, stops the VM and ends with `NAME: ALL PASS` (exit status 0) or
`NAME: N FAILED` (1). The suites share the VM directory, so run one at a
time.

| Suite | Covers | Time |
|---|---|---|
| `read.sh` | Read-only mount (`-o ro`, `security_model=none`): `ls -l`, `cat`, `cp`, `find`, `du`, `df`, an 8 MiB checksum, a directory larger than one Rreaddir, host-made symlinks to a file and a directory (read, followed, paths through them), `EROFS`, a binary run from the share (setuid ignored), restart after `slay` and after `SIGKILL`. | about 50 s |
| `write.sh` | Read-write (`mapped-xattr`): create, append, overwrite, 8 MiB copies against host checksums, truncate, writes past 4 GiB, fsync, chmod/chown/utime and their permission rules, owners of new files, mkdir/rmdir/rm/mv, symlinks, hard links, renames and links through a symlinked directory, sticky directories, files removed while open, search permission, the host changing or refusing things underneath, `cp -r` both ways with `diff -r`, the build-like workload compared with the same run on the host, a binary built on the host run from the share, `-o ro`. About 100 checks. | about 1 min |
| `robustness.sh` | Interrupting a 256 MiB read and write with SIGINT; clean unmount on SIGTERM and SIGINT (during a read, device reset found by the next instance); then a second boot with the share's reads throttled so one request outlives `timeout=2000`: `ETIMEDOUT`, no hang while the only slot is stale, recovery, other slots serving. | about 1.5 min (two boots) |
| `cache.sh` | The metadata cache (`cache=3000`): host changes hidden, then shown; changes made through fs-9p shown at once; permissions still checked; a repeated `ls -l` sends almost no walks. | about 35 s |
| `stress.sh [SECONDS]` | Ten loops in parallel for 60 s by default (`cp -r` + `diff`, `find`, `dd` + checksums, small-file churn, three processes running the same binary from the share); afterwards fs-9p must serve, a reference file must be intact, no fid leaked. | about 1.5 min |
| `diskfull.sh` | The host disk under the share filling up: `ENOSPC` on large and small writes, create and mkdir, `df` full, recovery after freeing space. Needs `vm/smallfs` ({doc}`test-environment`). | a minute or two (not timed) |
| `bench.sh` | Not a test: throughput and small-file rates on the share against the guest's QNX6 disk and RAM disk. Options: `-m MSIZE,...`, `-o OPTS`, `-S MODEL`, `-L` (no local baselines), `-s MIB`. Results are in {doc}`../user/performance`. | 2 to 10 min |

The guest image has no compiler or `make`, so `write.sh` checks
cross-development the way it is done in practice: a binary built on the
host with `qcc` runs from the share, and a build-like workload
(`workload.sh`: unpack, patch, edit in place, rename over, remove) leaves
the same tree in the guest as on the host.

Environment variables:

| Variable | Suites | Effect |
|---|---|---|
| `FS9P_MOUNT_OPTS` | `write`, `stress` | Extra fs-9p options, e.g. `cache=2000`. `write.sh` skips its host-side section with a cache (host changes may show late by design). |
| `FS9P_SECURITY_MODEL` | `write` | QEMU `security_model`: `mapped-xattr` (default), `mapped-file`, `passthrough` (QEMU as root, {doc}`test-environment`). Not `none`: every file then belongs to the host user, shown as `qnxuser`, the user the permission checks use as "someone else", so about ten checks fail by design. `read.sh` runs with `none`. |
| `FS9P_SMALLFS` | `diskfull`, `run-all` | The small filesystem (default `vm/smallfs`). |
| `FS9P_VM_DIR` | all | The VM directory (default `vm`). |

Helper files: `lib.sh` (booting, `vm_run`, assertions, trace checks),
`run-qnx.sh` (QEMU), `workload.sh` (the build-like workload),
`stress-guest.sh` and `bench-guest.sh` (the guest halves of `stress.sh` and
`bench.sh`, copied to the share), `progs/xdev.c` (built with `qcc` by
`write.sh`).

### Everything in one pass

```
tests/integration/run-all.sh [LOG_DIR]
```

runs every suite that doesn't need root, one after the other, in the
combinations that matter:

| Run | Command it stands for |
|---|---|
| read | `read.sh` |
| write | `write.sh` |
| write-cache | `FS9P_MOUNT_OPTS=cache=2000 write.sh` |
| write-mapfile | `FS9P_SECURITY_MODEL=mapped-file write.sh` |
| robustness | `robustness.sh` |
| cache | `cache.sh` |
| stress | `stress.sh` |
| stress-cache | `FS9P_MOUNT_OPTS=cache=2000 stress.sh` |
| diskfull | `diskfull.sh`, only when `vm/smallfs` is mounted |

Each suite's output goes to `LOG_DIR/<run>.log` (default `vm/test-logs/`),
and a summary with each run's time and result line comes at the end; the
exit status is 0 only if all passed. It warns first if `build-qnx/fs-9p`
is newer than the image. About 9 minutes without `diskfull`.

Before a release, add the run that needs QEMU as root
({doc}`test-environment`):

```
FS9P_SECURITY_MODEL=passthrough tests/integration/write.sh
```

Not timed yet. The times on this
page come from the last release run; {doc}`releasing` says how to update
them.

## How the trace checks work

`run-qnx.sh` gives QEMU `-trace 'v9fs_*' -D vm/qemu-9p.log`. At the end of
a suite, `check_trace` asserts:

- **No unexpected `v9fs_rerror`**: every error QEMU returned must match the
  suite's allow-list, a grep pattern of the errors it provokes on purpose
  (`' id 110 err 2$'`: walks of missing names; `76 err 39`: rmdir of a
  non-empty directory).
- **No clunk of an unknown fid**, and **only the root fid (0) still
  live**: `trace_fids` replays attaches, walks (matched with their replies
  by tag, since replies can come out of order) and clunks. A fid left live
  is a leak in fs-9p.

The memory manager keeps an executed binary mapped (and so its fid open)
for a while after the process exits; `wait_fids_released` waits up to 30 s
for that before the check.

## Reading a failure

1. The `FAIL` line shows the guest command's expected and actual output.
   Rerun the command by hand: `tests/integration/run-qnx.sh` (interactive,
   same share and trace) after the suite, or, as it is still set up,
   `vm/share` holds the files the suite created.
2. `vm/serial.log`: the whole guest console of the last boot, including
   fs-9p's own messages and anything the test's commands printed.
3. `vm/qemu-9p.log`: QEMU's view. Follow the failing operation by its tag
   (`v9fs_walk tag 2 id 110 ...`, then `v9fs_rerror tag 2 ... err 13`).
   Message ids and Linux errno numbers: {doc}`../user/troubleshooting`.
4. fs-9p's own log: in the guest, `slay fs-9p; fs-9p -o debug hostshare
   /mnt/host 2> /tmp/d.log &`, repeat the operation, read `/tmp/d.log`
   (every 9P message as a hex dump).
5. For a leaked fid: find its `newfid` in the walk that created it and the
   operation around it; the matching close or error path is missing a
   clunk.

## Pitfalls

- **The image is stale.** The image holds a copy of `build-qnx/fs-9p`;
  forgetting `mkqnximage --build` after a build tests the old binary.
  `use fs-9p` in the guest shows the version, but not the commit.
- **Console lines over 254 bytes** are dropped by the guest console and the
  command never runs. `vm_run` refuses them (status 125); put long logic in
  a script on the share and run that.
- **The guest's `/tmp` survives reboots** (it is on the disk image). A
  check that creates something in `/tmp` must remove it first, or the
  second run fails.
- **Background jobs** print a job notice (`[1] 1234`) into the captured
  output. Start them as `{ cmd & } 2>/dev/null`; that also discards the
  job's own error messages, so redirect those inside the braces
  (`{ cmd 2>/tmp/err & } 2>/dev/null`) when the check needs them.
- **Timing-based checks.** `robustness.sh` compares an interrupted
  transfer with a full one in the same VM (must take under 70%), `cache.sh`
  relies on sleeps around `cache=3000`, the timeout checks on throttling. A
  heavily loaded host can make them flaky: rerun before suspecting fs-9p.
- **QEMU's throttling allows a burst**: on a share started with
  `-F throttling.bps-read=...`, the first 128 KiB read is fast and only the
  following ones are slow. To have a request in flight for a while, read
  more than that.
- **After a passthrough run** (as root), `vm/` contains root-owned files:
  `sudo chown -R "$USER:" vm`.
- **A VM left running** by an interrupted suite is stopped by the next
  suite; stop it by hand with `run-qnx.sh -k`.

## Writing a new check

Use the helpers in `lib.sh`:

```
expect    "NAME" "GUEST_CMD" "EXPECTED_OUTPUT"
expect_rc "NAME" "GUEST_CMD" EXPECTED_STATUS
out=$(vm_run "GUEST_CMD" [TIMEOUT_S])     # then pass/fail/skip "NAME"
check_trace "ALLOWED_RERROR_PATTERN"
summary "SUITE"
```

Prepare host-side files directly in the share directory before or between
guest commands, and compare with host tools (`sha256sum`, `diff -r`).
Check the trace for the requests you expect when the point of the change
is what goes over the wire (`vm_trace | grep ...`).
