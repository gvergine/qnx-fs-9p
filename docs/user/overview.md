# Overview

fs-9p lets a QNX 8.0 guest running under QEMU use a directory of the host
machine as if it were a local filesystem. Files are read and written on the
host as the guest uses them, so the usual workflow is: build on the host,
run and test in the guest, with no copying and no network.

## How it works

QEMU contains a 9P file server. Given `-fsdev` and
`-device virtio-9p-device` options, it exports a host directory to the guest
through a virtio device, under a name called the *mount tag*. fs-9p is the
guest side: a QNX resource manager that finds that device, speaks the
9P2000.L protocol to it, and serves the files at a path of your choice.

```
  QNX program            open(), read(), write(), stat() ...
       |  QNX messages
  fs-9p (resource manager, /mnt/host)
       |  9P2000.L requests
  virtio-9p device (virtio-mmio)
       |
  QEMU's 9P server  ---->  host directory
```

There is no network device, no NFS or SMB server, and no extra program on
the host: QEMU itself is the file server. Linux guests use the same QEMU
feature with `mount -t 9p`; fs-9p brings it to QNX.

## Requirements

- QNX Software Development Platform 8.0, with the QEMU `virt` packages from
  the QNX Software Center (`startup-qemu-virt` and the virtio drivers), to
  build fs-9p and the guest image.
- An **aarch64le** QNX 8.0 guest on QEMU's **`virt`** machine. QEMU usually
  emulates it (TCG) on an x86_64 Linux host. Tested with QEMU 10.2.1.
- The share attached as a `virtio-9p-device` (virtio-mmio). `-virtfs` and
  `virtio-9p-pci` are not supported.

x86_64 guests are not supported and were never tested (the build is
aarch64-only, and x86 QEMU machines put virtio-9p on PCI). Nor are PCI
devices, other hypervisors or network 9P servers.

## What works

- Reading and writing files, appending, truncating, `fsync`, files larger
  than 4 GiB.
- Creating and removing files and directories, renaming, symlinks, hard
  links, `chmod`, `chown`, `touch`, `df`.
- Running programs directly from the share.
- POSIX permissions checked in the guest, files owned by the user who
  creates them (with a suitable QEMU `security_model`, see {doc}`qemu`).
- Several requests in flight, an optional metadata cache, read-only mounts,
  clean unmount, recovery from timeouts, running without root.

What doesn't, and how fs-9p behaves at the edges, is in {doc}`behaviour`.

## Where to go next

- {doc}`quickstart`: a share mounted in a few minutes.
- {doc}`building`, {doc}`qemu` and {doc}`integrating` for a real system.
- {doc}`reference`: the `fs-9p` command.
- {doc}`troubleshooting` when something doesn't work.
