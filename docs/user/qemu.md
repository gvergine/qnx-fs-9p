# Configuring QEMU

The host side of a share is two QEMU options. Everything here applies to
`qemu-system-aarch64` with the `virt` machine.

## The share

```
-fsdev local,id=fs0,path=/some/dir,security_model=mapped-xattr
-device virtio-9p-device,fsdev=fs0,mount_tag=hostshare
```

- `-fsdev local,...` defines a filesystem backend: `path` is the host
  directory, `id` links it to the device, `security_model` decides how file
  ownership and modes are stored (next section).
- `-device virtio-9p-device,...` attaches it to the guest as a virtio-9p
  device on the virtio-mmio transport. `mount_tag` is the name fs-9p uses
  to find it: `fs-9p hostshare /mnt/host`.

Use `virtio-9p-device`, not `-virtfs`: on `virt`, `-virtfs` creates a PCI
device (`virtio-9p-pci`), which fs-9p doesn't support.

`-fsdev` also accepts `readonly=on`, which makes QEMU refuse every change
(fs-9p then reports `EROFS`). `fs-9p -o ro` does the same from the guest
side. Either works; `readonly=on` also protects against other guests or a
misconfigured fs-9p, and `-o ro` lets the guest see it (`df -g` shows
`rdonly`). Using both is the safest.

## Choosing a `security_model`

QEMU runs as an ordinary host user, so it cannot give files arbitrary
owners. The `security_model` decides what it does instead:

| Model | Owners and modes the guest sees | Use it when |
|---|---|---|
| `mapped-xattr` | Stored per file in `user.virtfs.*` extended attributes on the host; files the guest didn't create show their real host owner and mode. New files belong to the guest user who creates them. | The usual choice. The host filesystem must support user xattrs (ext4 was tested; xfs and btrfs have them too). |
| `mapped-file` | The same, stored in hidden `.virtfs_metadata` directories next to the files on the host. | The host filesystem has no xattrs. |
| `none` | The real host owner and mode. Everything the guest creates belongs to the host user running QEMU; `chown` fails with `EPERM`. | You want plain host files and don't need guest ownership. |
| `passthrough` | Real host owners, set for real (`chown` works). | QEMU runs as root. Then the host's own permissions no longer protect anything: only fs-9p's checks in the guest do. |

With `mapped-xattr` and `mapped-file`, symlinks the guest creates are stored
as small regular files on the host, and **symlinks that already exist on
the host can't be read** ("Too many levels of symbolic links or prefixes"). If the
share contains host symlinks the guest must follow, use `none`. See
{doc}`behaviour` for the details.

## Device order with mkqnximage images

QEMU's `virt` machine has 32 virtio-mmio slots and fills them from the top
in command-line order. Images made by `mkqnximage` start their disk and
random-number drivers at fixed slots, so keep this order:

| Order | Device | Slot | Expected by |
|---|---|---|---|
| 1 | `virtio-blk-device` (the image's disk) | 31 | `devb-virtio` in the image |
| 2 | `virtio-9p-device` (the share) | 30 | fs-9p (finds it by tag, any slot) |
| 3 | `virtio-rng-device` | 29 | `devr-virtio` in the image |
| 4+ | more `virtio-9p-device`s | 28, 27, ... | fs-9p |

fs-9p itself scans all slots for its mount tag, so with your own image only
your other drivers' expectations matter.

## Several shares

Give each its own `-fsdev` id and its own `mount_tag`, and run one fs-9p
per tag:

```
-fsdev local,id=fs0,path=/home/me/src,security_model=mapped-xattr
-device virtio-9p-device,fsdev=fs0,mount_tag=src
-fsdev local,id=fs1,path=/opt/tools,security_model=none,readonly=on
-device virtio-9p-device,fsdev=fs1,mount_tag=tools
```

```
fs-9p src /mnt/src
fs-9p -o ro tools /opt/tools
```

Mount tags must be unique: fs-9p serves one device per tag.

## Machine and QEMU version

fs-9p was tested with QEMU 10.2.1 on `-M virt-8.2` (TCG on x86_64), which
is what `mkqnximage` images boot with on current QEMU; `mkqnximage`'s own
`virt-4.2` no longer exists. Both virtio-mmio layouts work: QEMU's default
(version 1, "legacy") and version 2 (`-global
virtio-mmio.force-legacy=false`).

## Tracing (for troubleshooting)

QEMU can log every 9P request it receives:

```
-trace 'v9fs_*' -D /tmp/qemu-9p.log
```

Each request and reply appears as a `v9fs_...` line; failed requests as
`v9fs_rerror ... err N` with a Linux errno. See {doc}`troubleshooting`.
