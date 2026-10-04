# fs-9p

fs-9p is a QNX resource manager that speaks 9P2000.L to QEMU's
virtio-9p device on the aarch64 `virt` machine, over the virtio-mmio transport. It is meant to be used for mounting a host directory in a QNX 8.0 guest running under QEMU, with no need of networking. 

fs-9p is made for everyday development, not just for getting files into
a VM. Because it starts from the IFS and needs nothing but `procnto` and
the C runtime libraries, the guest image can stay a small boot image, while your
programs, their shared libraries, tools and data live in a host directory
and run straight from the share: rebuild on the host and run in the guest
at once, with no image to rebuild and nothing to copy. It behaves like a
POSIX filesystem (permissions and ownership, symlinks and hard links,
atomic renames, truncate, fsync), with the few differences listed in the
User Guide.

```
host:   qemu-system-aarch64 -M virt ... \
          -fsdev local,id=fs0,path=/some/dir,security_model=mapped-xattr \
          -device virtio-9p-device,fsdev=fs0,mount_tag=hostshare
guest:  fs-9p hostshare /mnt/host
guest:  slay fs-9p              # unmount
```

**aarch64 guests only.** fs-9p is built for and tested on aarch64le QNX
8.0 guests on QEMU's `virt` machine (QEMU 8.2 or later), usually emulated
on an x86_64 host. x86_64 guests are not supported and were never tested:
the build is aarch64-only, and on x86 QEMU machines the virtio-9p device
sits on PCI (`-virtfs`), which fs-9p doesn't drive.

- Reads, writes, create, truncate, `chmod`/`chown`/`utime`, `fsync`,
  `mkdir`, `rmdir`, `rename`, symlinks, hard links, `df`, running programs
  from the share.
- POSIX permissions enforced in the guest; the share is mounted `nosuid`.
- Several requests in flight, signals interrupt long transfers, clean
  unmount, recovery from timeouts, an optional metadata cache.
- Runs without root with three abilities (`mem_phys`, `interrupt`,
  `pathspace`).

Requires QNX SDP 8.0 to build. Version and changes:
[CHANGELOG.md](CHANGELOG.md).

## Quickstart

Build and install (Linux host, QNX SDP 8.0 in `~/qnx800`, CMake 3.20 or
later):

```
source ~/qnx800/qnxsdp-env.sh
cmake -S . -B build-qnx -DCMAKE_TOOLCHAIN_FILE=cmake/qnx8-aarch64le.cmake
cmake --build build-qnx
cmake --install build-qnx --prefix ~/fs-9p-install
```

Put `~/fs-9p-install/sbin/fs-9p` in your QNX image (for an mkqnximage
image, a line in `local/snippets/system_files.custom`), start QEMU with the
`-fsdev`/`-device` options above, and run `fs-9p hostshare /mnt/host` in
the guest. The User Guide's Quickstart walks through every step, including
the full QEMU command line.

## Documentation

The documentation is in `docs/` (Markdown) and builds into HTML with
Sphinx, a docs-only dependency:

```
python3 -m venv .venv-docs && .venv-docs/bin/pip install -r docs/requirements.txt
cmake -S . -B build-qnx -DCMAKE_TOOLCHAIN_FILE=cmake/qnx8-aarch64le.cmake
cmake --build build-qnx --target docs     # build-qnx/docs/html/index.html
```

`cmake --install` installs it to `share/doc/fs-9p/html/`. On the target,
`use fs-9p` shows the options.

- **User Guide** ([docs/user/](docs/user/index.md)), for system
  integrators: [quickstart](docs/user/quickstart.md),
  [building](docs/user/building.md), [QEMU setup](docs/user/qemu.md),
  [adding fs-9p to a QNX system](docs/user/integrating.md),
  [the fs-9p reference](docs/user/reference.md),
  [behaviour and limitations](docs/user/behaviour.md),
  [performance](docs/user/performance.md), [examples](docs/user/examples.md),
  [troubleshooting](docs/user/troubleshooting.md).
- **Developer Guide** ([docs/dev/](docs/dev/index.md)), for maintainers:
  [architecture](docs/dev/architecture.md),
  [9P message mapping](docs/dev/op-mapping.md),
  [virtio notes](docs/dev/virtio-notes.md),
  [test environment](docs/dev/test-environment.md),
  [running the tests](docs/dev/testing.md),
  [workflow](docs/dev/workflow.md), [releasing](docs/dev/releasing.md),
  [updating the docs](docs/dev/docs.md).

## Credits

- 9P comes from Plan 9 from Bell Labs; 9P2000.L is the Linux variant, and
  the Linux kernel's 9P client (`include/net/9p/9p.h`, `net/9p/client.c`,
  `net/9p/protocol.c`) is its de facto specification. fs-9p's message
  layouts, flag and attribute values, and the Linux errno numbers it
  translates were taken from those sources and the kernel's
  `asm-generic/errno*.h`. No Linux code is included: the protocol library
  was written from scratch.
- QEMU's 9P server is fs-9p's peer, and its `v9fs_*` trace events are what
  the tests check fs-9p against.
- The virtio transport follows the OASIS Virtual I/O Device (VIRTIO)
  specification, version 1.2.

## License

Apache-2.0. See [LICENSE](LICENSE). fs-9p is distributed as source; build
it with your own QNX SDP.

QNX is a trademark of BlackBerry Limited. fs-9p is an independent project,
not affiliated with or endorsed by BlackBerry or QNX.
