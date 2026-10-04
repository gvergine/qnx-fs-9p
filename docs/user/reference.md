# fs-9p

Mount a host directory exported by QEMU's virtio-9p device

## Syntax

```
fs-9p [-o option[,option...]] mount_tag mount_point
```

## Runs on

QNX OS 8.0, aarch64le, in a guest on QEMU's `virt` machine.

## Options

`-o option[,option...]`
: One or more of the options below, separated by commas. `-o` may be given
  more than once.

`ro`
: Mount read-only. Anything that would change the share fails with
  `EROFS`, and `df -g` shows the `rdonly` flag.

`rw`
: Mount read-write. This is the default.

`cache=MS`
: Keep file attributes, and names found missing, for up to `MS`
  milliseconds (0 to 3600000). Default 0: no cache, every lookup asks the
  host. With a cache, changes made on the host may show up to `MS` late;
  changes made through fs-9p always show at once, and file contents are
  never cached. See {doc}`behaviour`.

`msize=N`
: The largest 9P message to ask for, in bytes (at least 4096; default
  524288). QEMU may agree to less. Larger messages make big transfers
  faster and use more memory.

`requests=N`
: How many 9P requests may be in flight at once, 1 to 8 (default 4). Each
  takes 2 × msize bytes of memory (4 MiB at the defaults).

`timeout=MS`
: Fail a request that gets no reply within `MS` milliseconds (default
  30000); the call then fails with `ETIMEDOUT`. 0 waits forever.

`debug`
: Stay in the foreground and log the device setup and every 9P message
  (hex dump) to standard error. For troubleshooting; slow.

`poll`
: Poll for completions instead of using the device interrupt. For
  troubleshooting interrupt problems; uses CPU.

`irq=N`
: Use interrupt `N` instead of the one implied by the device's virtio-mmio
  slot (48 + slot on `virt`). For unusual machine configurations.

`mount_tag`
: The tag QEMU gives the share (`mount_tag=` of `-device
  virtio-9p-device`).

`mount_point`
: The absolute path to mount it at, other than `/`. It doesn't need to
  exist.

## Description

fs-9p finds the virtio-9p device whose mount tag is `mount_tag`, sets it
up, negotiates 9P2000.L with QEMU, and serves the shared directory at
`mount_point` as a QNX resource manager. Once the share is mounted, fs-9p
returns (with status 0) and keeps running in the background.

Programs then use the share like any filesystem. Permissions are checked
in the guest against the owners and modes the host reports, as on a local
filesystem; QEMU additionally can't do what its own host user can't. The
mount is `nosuid`. Behaviour in detail, and all limitations:
{doc}`behaviour`.

Only one fs-9p can serve a mount tag. While it runs, it holds the name
`/dev/name/local/fs-9p/mount_tag`; a second fs-9p for the same tag fails
with "Resource busy".

`slay fs-9p` (SIGTERM) unmounts cleanly: fs-9p removes the mount point,
waits for the requests in flight, resets the device and exits. SIGINT and
SIGHUP do the same, unless they were ignored when fs-9p started. Programs
that still have files open on the share get errors afterwards.

Without root, fs-9p needs the abilities `mem_phys`, `interrupt` and
`pathspace` ({doc}`integrating`).

## Examples

Mount the share tagged `hostshare` at `/mnt/host`:

```
fs-9p hostshare /mnt/host
```

Mount a toolchain read-only, with a 10-second metadata cache:

```
fs-9p -o ro,cache=10000 tools /opt/tools
```

Run as `qnxuser`:

```
on -u qnxuser -A nonroot,allow,mem_phys -A nonroot,allow,interrupt \
    -A nonroot,allow,pathspace fs-9p hostshare /mnt/host
```

Troubleshoot in the foreground, with every 9P message logged:

```
fs-9p -o debug hostshare /mnt/host 2> /tmp/fs-9p.log
```

Unmount:

```
slay fs-9p
```

## Exit status

0
: The share was mounted (fs-9p then continues in the background), or
  fs-9p was unmounted cleanly by a signal.

1
: fs-9p couldn't start; the reason is printed on standard error.

## Errors

At startup, on standard error:

`usage: fs-9p [-o options] mount_tag mount_point (see 'use fs-9p')`
: Wrong arguments.

`fs-9p: bad option 'X'`
: An unknown option, or a value out of range.

`fs-9p: mount_point must be an absolute path other than /`
: As it says.

`fs-9p: TAG: no virtio-9p device with this mount tag`
: QEMU has no `virtio-9p-device` with that `mount_tag`.

`fs-9p: TAG: Resource busy`
: Another fs-9p already serves this tag.

`fs-9p: TAG: Operation not permitted`
: The abilities `mem_phys` or `interrupt` are missing.

`fs-9p: MOUNT_POINT: Operation not permitted`
: The ability `pathspace` is missing.

`fs-9p: TAG: protocol setup failed: ...`
: The device was found, but 9P version negotiation or attaching the share
  failed ("Invalid argument": QEMU can't read the shared directory).

Afterwards, errors reach programs as `errno` values from their calls. The
common ones are explained in {doc}`troubleshooting`.

## Caveats

- Host symlinks can't be read with `security_model=mapped-xattr` or
  `mapped-file` ({doc}`qemu`).
- `slay -s KILL fs-9p` skips the clean unmount; see {doc}`behaviour`.
- FIFOs and device nodes can't be created on the share (`ENOTSUP`).

## See also

{doc}`qemu`, {doc}`integrating`, {doc}`behaviour`, {doc}`troubleshooting`;
`on`, `slay`, `use`, `df`, `mount` in the QNX Utilities Reference.
