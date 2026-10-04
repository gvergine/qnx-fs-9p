# The test environment

How to set up, from nothing, the machine that builds fs-9p and runs its
tests: a Linux host with the QNX SDP, a QNX 8.0 aarch64le guest image built
with mkqnximage, and QEMU. Everything here was last done on Ubuntu with
QEMU 10.2.1 (TCG on x86_64), QNX SDP 8.0, CMake 4.2, GCC 15 and socat 1.8.

## 1. Host packages

```
sudo apt install qemu-system-arm socat cmake gcc clang-format e2fsprogs python3-venv
```

| Package | Used for |
|---|---|
| `qemu-system-arm` | `qemu-system-aarch64`. Needs the `log` trace backend (Ubuntu's has it) and the `virt-8.2` machine type. |
| `socat` | The test scripts talk to the guest console and the QEMU monitor through UNIX sockets. |
| `cmake`, `gcc` | The host build: `lib9p` and its unit tests. CMake 3.20 or later. |
| `clang-format` | Formatting (`.clang-format`). |
| `e2fsprogs` | `mkfs.ext4` for the disk-full test only. |
| `python3-venv` | The documentation toolchain ({doc}`docs`). |

## 2. QNX SDP 8.0 and the virt packages

Install the SDP with the QNX Software Center into `~/qnx800` (the docs and
scripts assume that path; adjust `source` lines if yours differs). Then
install these packages with the Software Center (package ids as they appear
under `~/qnx800/.packages/metadata/`):

| Package id | Provides |
|---|---|
| `com.qnx.qnx800.host.mkqnximage` | `mkqnximage` |
| `com.qnx.qnx800.target.qemuvirt` | `startup-qemu-virt`, the aarch64 `virt` board startup |
| `com.qnx.qnx800.target.driver.virtio` | An empty group package that brings the next three |
| `com.qnx.qnx800.target.driver.virtio.devb` | `devb-virtio`, the image's disk driver |
| `com.qnx.qnx800.target.driver.virtio.devc` | `devc-virtio` |
| `com.qnx.qnx800.target.driver.virtio.startup` | `startup-armv8_fm` |
| `com.qnx.qnx800.target.net.devsvirtio` | `devs-vtnet_mmio.so`; mkqnximage checks for it even though the test VM has no network |

mkqnximage only offers `--arch=aarch64le` once the virtio packages are
installed. Every shell that builds or tests needs:

```
source ~/qnx800/qnxsdp-env.sh
```

(`QNX_HOST`, `QNX_TARGET`, `qcc` and `mkqnximage` on `PATH`.) Some tests
use `QNX_TARGET` (toybox from the SDP) or `qcc`, and skip those checks
without them.

## 3. Build fs-9p

```
cmake -S . -B build-host -DFS9P_BUILD_TESTS=ON && cmake --build build-host
cmake -S . -B build-qnx -DCMAKE_TOOLCHAIN_FILE=cmake/qnx8-aarch64le.cmake
cmake --build build-qnx
```

The tests use `build-qnx/fs-9p` (through the image, next step) and
`build-host` for the unit tests. `build-*` directories are ignored by git.

## 4. The guest image

The image lives in `vm/` (ignored by git):

```
mkdir -p vm && cd vm
mkqnximage --type=qemu --arch=aarch64le --graphics=no --noprompt
```

This creates `vm/local/snippets/` with empty `*.custom` files. Add the
fs-9p binary from the build tree to the image's `/system/bin`, with an
absolute path, in `vm/local/snippets/system_files.custom`:

```
[perms=0755] bin/fs-9p=/ABSOLUTE/PATH/TO/REPO/build-qnx/fs-9p
```

and rebuild:

```
(cd vm && mkqnximage --build --noprompt)
```

**The image holds a copy**: rebuild it after every `cmake --build
build-qnx`, or the tests run the old binary. Stop the VM first.

The default image has 2 CPUs, the `qnxuser` account (used by the
non-root checks), a QNX6 `/data` partition with about 40 MB free (the
benchmark's local baseline), and `/tmp` on the disk image, so files in the
guest's `/tmp` **survive reboots**.

## 5. Launching QEMU: `tests/integration/run-qnx.sh`

mkqnximage's own runner (`mkqnximage --run`) can't be used: it always adds
bridge networking (and fails without a configured bridge), and its machine
type `virt-4.2` no longer exists in QEMU 10. `run-qnx.sh` starts QEMU from
the built `vm/` with no network device:

```
tests/integration/run-qnx.sh             # interactive, console on stdio (Ctrl-a x quits)
tests/integration/run-qnx.sh -m modern   # virtio-mmio version 2 for every device
tests/integration/run-qnx.sh -S none     # another security_model
tests/integration/run-qnx.sh -H          # headless, for scripts
tests/integration/run-qnx.sh -k          # stop the headless VM
tests/integration/run-qnx.sh -n          # print the QEMU command only
```

Defaults: VM directory `<repo>/vm` (`-v` or `$FS9P_VM_DIR`), share
`<vm>/share` (`-s`), tag `hostshare` (`-t`), `security_model=mapped-xattr`
(`-S`), trace log `<vm>/qemu-9p.log` (`-l`, truncated at each start),
extra fsdev options with `-F` (e.g. `-F throttling.bps-read=16384`). The
machine is `virt-8.2`, the CPU and memory follow the image's options
(`vm/output/options`), with `-accel tcg,thread=multi`.

Headless mode daemonizes QEMU and creates:

- `<vm>/serial.sock`: the guest console, also logged to `<vm>/serial.log`.
  By hand: `printf 'ls /\r' | socat - UNIX-CONNECT:vm/serial.sock`.
- `<vm>/monitor.sock`: the QEMU monitor (`-k` sends `quit` there).

QEMU writes its own startup errors to the `-D` log, not stderr;
`run-qnx.sh` prints that log when QEMU fails to start. Booting to
`Startup complete` takes about 20 s under TCG; the startup script's
complaint that `vtnet0` doesn't exist is harmless.

Then, in the guest: `fs-9p hostshare /mnt/host`.

### virtio-mmio slots

QEMU `virt` has 32 virtio-mmio transports (0x200 bytes each from
0x0a000000) and fills them **from the top, in command-line order**. The
mkqnximage image hardcodes two of them, so `run-qnx.sh` adds the devices in
this order:

| Slot | Address | IRQ | Device | Used by |
|---|---|---|---|---|
| 31 | 0x0a003e00 | 79 | virtio-blk | `devb-virtio virtio smem=0xa003e00,irq=79` |
| 30 | 0x0a003c00 | 78 | virtio-9p | fs-9p (mkqnximage's runner puts virtio-net here) |
| 29 | 0x0a003a00 | 77 | virtio-rng | `devr-virtio mem=0xa003a00` |

fs-9p scans all slots, so its own slot doesn't matter, but the image's
disk and rng drivers break if those devices move.

### Legacy and modern virtio-mmio

QEMU's virtio-mmio defaults to `force-legacy=true` (MMIO version 1).
`-global virtio-mmio.force-legacy=false` (`run-qnx.sh -m modern`) switches
every virtio-mmio device to version 2. The image's drivers work in both
modes; so does fs-9p.

### The QEMU trace

`run-qnx.sh` passes `-trace 'v9fs_*' -D <log>`. The events:

```
qemu-system-aarch64 -trace help | grep '^v9fs'
```

QEMU 10.2.1 has 47, including `v9fs_walk`, `v9fs_open` (also for Tlopen),
`v9fs_getattr`, `v9fs_read`, `v9fs_clunk`, `v9fs_rerror` and their
`_return` variants, but **none for Tstatfs, Tunlinkat or Trenameat**: tests
check those through their effects. QEMU warnings go to the same file, so
parsers keep only lines starting with `v9fs_`.

## 6. Setups that need root

Two things need root on the host. Both are only for the final test pass
before a release ({doc}`releasing`).

### A small filesystem for the disk-full test

```
truncate -s 32M vm/smallfs.img
mkfs.ext4 -q -m 0 vm/smallfs.img
mkdir -p vm/smallfs
sudo mount -o loop vm/smallfs.img vm/smallfs
sudo chown "$USER:" vm/smallfs
```

`diskfull.sh` refuses to run unless `vm/smallfs` (or `$FS9P_SMALLFS`) is a
mount point of at most 200 MiB. Afterwards: `sudo umount vm/smallfs`.

### QEMU as root, for `security_model=passthrough`

`passthrough` makes QEMU create files with the guest's owners, which only
root can do:

```
source ~/qnx800/qnxsdp-env.sh
sudo -E env "PATH=$PATH" FS9P_SECURITY_MODEL=passthrough \
    tests/integration/write.sh 2>&1 | tee /tmp/write-passthrough.log
sudo chown -R "$USER:" vm     # the root run leaves root-owned files behind
```

## 7. A Linux reference guest

If fs-9p and QEMU ever disagree about the protocol, the Linux 9P client is
the known-good reference: boot an aarch64 Linux guest on `-M virt` with the
same `-fsdev`/`-device virtio-9p-device` options and

```
mount -t 9p -o trans=virtio,version=9p2000.L,msize=262144 hostshare /mnt/host
```

and compare its `v9fs_*` trace with fs-9p's. This was never needed so far,
so the procedure is untested and no script exists for it.
