# Integration tests

QEMU-driven tests: each suite boots the QNX guest headless with
`run-qnx.sh`, runs commands in it through the serial console, and checks
the results and QEMU's `v9fs_*` trace (no unexpected errors, no leaked
fids).

```
source ~/qnx800/qnxsdp-env.sh
tests/integration/run-all.sh        # every suite that doesn't need root
tests/integration/write.sh          # or one suite
```

Setting up the environment (host packages, SDP packages, the guest image
in `vm/` with `bin/fs-9p`): `docs/dev/test-environment.md`.
What each suite covers, its options, reading failures and pitfalls:
`docs/dev/testing.md`.

| File | What |
|---|---|
| `read.sh` | Read-only mount: reading, listing, symlinks, running binaries, restarts. |
| `write.sh` | Read-write mount: the write path, namespace operations, permissions, host-side changes, cross-development. `FS9P_MOUNT_OPTS`, `FS9P_SECURITY_MODEL`. |
| `robustness.sh` | Interrupted transfers, clean unmount, request timeouts. |
| `cache.sh` | The metadata cache (`-o cache=`). |
| `stress.sh [SECONDS]` | Parallel load. `FS9P_MOUNT_OPTS`. |
| `diskfull.sh` | The host disk filling up; needs `vm/smallfs` (root, once). |
| `run-all.sh [LOG_DIR]` | All of the above that don't need root, in one pass, with a summary. |
| `bench.sh` | Benchmark, not a test. |
| `lib.sh` | Shared helpers: boot, `vm_run`, assertions, trace checks. |
| `run-qnx.sh` | Launch QEMU with the image in `vm/` and a virtio-9p share. |
| `workload.sh`, `stress-guest.sh`, `bench-guest.sh`, `progs/xdev.c` | Run by the suites. |
