# Development workflow

The rules the code follows, how to change it safely, and how to find out
what QNX or QEMU really do when the documentation doesn't say.

## Repository layout

| Path | Contents |
|---|---|
| `lib9p/` | The portable 9P2000.L codec: `include/lib9p/p9.h`, `src/`. |
| `src/` | The resource manager, the 9P session, the virtio transport, the cache, `main.c`, the `use` message template. |
| `tests/unit/` | Host-only unit tests of `lib9p`. |
| `tests/integration/` | QEMU-driven suites, `run-qnx.sh`, helpers ({doc}`testing`). |
| `docs/` | This documentation (Sphinx): `user/` the User Guide, `dev/` the Developer Guide. |
| `packaging/fs-9p.build` | Lines to put fs-9p in a QNX image (installed). |
| `cmake/` | The QNX toolchain file and the consistency check. |
| `vm/`, `build-*/`, `.venv-docs/` | Local, ignored by git: the guest image, build trees, the docs toolchain. |

## Rules

These keep the code portable, safe against a misbehaving peer, and
honest. Break one only after deciding to, and record the decision in
{doc}`architecture` ("Decisions and why").

**lib9p**

1. Only `<stdint.h>`, `<stddef.h>`, `<string.h>`, `<stdbool.h>`. No QNX or
   POSIX headers; it builds and is tested on plain Linux.
2. No I/O, no `malloc`/`free`, no `errno`, no `printf`. Callers supply the
   buffers; functions return a byte count or a negative `P9_E_*` code.
3. Wire encoding byte by byte, little-endian, through the `wire.h` cursors.
   Never cast a struct onto a buffer.
4. Every length or count read from the wire is checked against what's left
   of the buffer before use. A malformed message is an error code, never an
   overrun; the fuzzer must keep `decode.c` at full line coverage.

**fs-9p**

5. The resource manager talks to the virtio code only through
   `src/transport.h`. No registers, rings or DMA memory outside
   `transport_virtio.c`.
6. Errors propagate as negative errno values and become the client's
   `errno` at the edge (the handler's return). Linux errno numbers from
   `Rlerror` are converted by name in `errno_map.c`; the numbers differ.
7. Allocate at startup. Per-request allocation needs a comment saying why.
   Large per-handler buffers are `static _Thread_local`, not on the stack
   and not plain `static` (handlers run on several threads at once).
8. Every fid that a walk, attach or create makes is clunked on every path,
   including errors. A leak shows in the trace checks.
9. No network code: no TCP transport, no `diod` ({doc}`architecture`,
   "Decisions and why").

**Everywhere**

10. Don't copy QNX SDK headers, samples or documentation text. Include the
    SDP headers; write original code.
11. Don't assume an API or a behaviour exists: check the SDP headers
    (`$QNX_TARGET/usr/include`) and the SDP documentation, or try it
    ({ref}`experiments`). Code that
    relies on something unconfirmed carries a `/* VERIFY: ... */` comment
    until a run confirms it. There are none left today.
12. Comments say why, not what; cite the spec or the experiment when a
    layout or an order isn't obvious.
13. Keep the dependencies few: the build needs CMake, a C compiler and the
    SDP; the docs Sphinx (pinned, in a venv); the tests bash, socat and
    QEMU. There is no CI. Adding a dependency, a build tool or a CI service
    is a decision to record.
14. Claim only what was run. A change is done when it was built and tested;
    say which tests ran and which couldn't (no QEMU, needs root).

## Style

- C11. Names: `p9_` for `lib9p`'s public API, `fs9p_` for the resource
  manager and session, `vio9p_` for the transport; types end in `_t`.
  Fixed-width integers on the wire path.
- 4-space indent, 100 columns, braces on the same line for control flow,
  on the next line for functions. `.clang-format` encodes this, and the
  tree is clean against it:

  ```
  clang-format -i src/*.c src/*.h lib9p/src/*.c lib9p/src/*.h lib9p/include/lib9p/*.h
  ```
- `lib9p` builds with `-std=c11 -Wall -Wextra -Wpedantic -Werror`, and so
  does everything else; a warning is a build failure.
- Short, single-purpose functions. Global state only in the few structs
  that need it (`fs9p_fs`, the cache table).

## Commits

Small and buildable: each commit builds in both modes and passes the unit
tests. For every change: build `build-host` and `build-qnx`, run the unit
tests (also under ASan when `lib9p` changed), rebuild the image and run
the integration suite of the area (all of them, `run-all.sh`, when in
doubt), and build the docs when they changed. The subject is a short imperative sentence ("Check search permission
on every directory of a path"); the body says why, and what was tested.
Code, tests and docs for one change can be separate commits, in that order.
`CHANGELOG.md` gets a line under the unreleased version for anything a user
would notice.

## Checklists

### Adding a 9P message

1. **Spec first.** Find the layout in the Linux kernel's 9P client
   (`include/net/9p/9p.h`, `net/9p/client.c`) and in QEMU (`hw/9pfs/9p.c`);
   there is no formal 9P2000.L spec beyond these.
2. **`lib9p`:** the type numbers in `p9.h`, an encoder (`encode.c`) and a
   decoder (`decode.c`), validating every field.
3. **Unit tests:** a field-layout test in `test_encode.c` and
   `test_decode.c`, the decoder added to `test_fuzz.c`. Run plain and with
   ASan; check `decode.c` coverage stays at 100%.
4. **Session:** a call in `client.c`/`client.h` in the style of the others
   (slot, encode, `call()`, decode, slot back); think about fids on every
   error path and on `ETIMEDOUT`.
5. **Resource manager:** the handler that uses it; permission checks with
   the iofunc helpers; cache invalidation in `cache.c` if it changes
   anything a lookup could have cached.
6. **Integration test** in the suite of the area, including the trace
   (the expected request, the allow-list if it fails on purpose).
7. **Docs:** {doc}`op-mapping`; the User Guide's behaviour page if users
   see a difference; `CHANGELOG.md`.

### Adding a mount option

1. Parse it in `main.c` (`strcmp(o, "name")`, validate the value, `bad
   option` otherwise).
2. Describe it in `src/fs-9p.use.in` (a line starting with a space and the
   name) and in `docs/user/reference.md` (`` `name` `` or `` `name=X` ``).
   The `consistency` test fails until both have it.
3. A check in the integration suite of the area; a `CHANGELOG.md` line.

(experiments)=
## Settling a question by experiment

Much of fs-9p's behaviour isn't specified anywhere: what a QNX utility
sends for `mkdir` or `rename`, which flags arrive, what QEMU does with an
odd request. The answers in {doc}`architecture` ("Behaviours found by
experiment") and {doc}`op-mapping` came from small experiments like these:

- **What does the client send?** Temporarily log the message fields in the
  handler (for a connect message: `subtype`, `file_type`, `extra_type`,
  `mode`, `ioflag`, the path and the extra data) to standard error, run
  `fs-9p -o debug` in the foreground in the guest, and run the utility.
- **What should the answer be?** Do the same on a native QNX filesystem in
  the same guest (the QNX6 `/data` or `/tmp`, or `/dev/shmem`) and compare
  errno values and results; fs-9p aims to behave like them.
- **What does QEMU do?** Look at the trace (`vm/qemu-9p.log`) for the
  request and its reply or `v9fs_rerror`; read `hw/9pfs/9p.c` for the why.
  The `security_model` changes the answers, so try more than one.
- **A small program** is often clearer than a utility: write it in C, build
  it on the host with `qcc -Vgcc_ntoaarch64le`, put it on the share and run
  it from the guest (`tests/integration/progs/xdev.c` is one that stayed).

Then remove the temporary logging, write the finding where the next reader
will look (a comment at the code that depends on it, {doc}`op-mapping` or
{doc}`architecture`), and add a check to an integration suite so a change
in QNX or QEMU shows up.

## Recording decisions and findings

A design decision and its reason go in {doc}`architecture` ("Decisions and
why"), in the same commit as the code, so the two never disagree. A QNX or
QEMU behaviour found by experiment goes where the next reader looks: a
comment at the code that relies on it, plus {doc}`virtio-notes` (every QNX
API the transport uses and every surprise), {doc}`op-mapping` (what a QNX
call sends and the 9P it becomes) or the architecture page's list of
behaviours found by experiment.
