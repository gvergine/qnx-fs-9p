# Troubleshooting

Each entry gives the symptom, the cause and the fix. The last section
shows how to collect information when none of them fits.

## fs-9p doesn't start

### "fs-9p: TAG: no virtio-9p device with this mount tag"

QEMU has no `virtio-9p-device` with `mount_tag=TAG`.

- Compare the tag in the QEMU command with the one given to fs-9p; tags
  are case-sensitive.
- `-virtfs` creates a PCI device on `virt`, which fs-9p doesn't see: use
  `-fsdev` with `-device virtio-9p-device` ({doc}`qemu`).
- `fs-9p -o debug TAG /mnt/x` lists the virtio-9p devices it finds and
  their tags (`vio9p: slot 30: virtio-9p v1 tag "..."`).

### "fs-9p: TAG: Resource busy"

Another process already serves this tag: usually an fs-9p that is still
running. `pidin -p fs-9p` shows it, `ls /dev/name/local/fs-9p` lists the
tags in use. Unmount the old one with `slay fs-9p` first. To mount the same
host directory twice, give QEMU a second `-fsdev`/`-device` pair with its
own tag.

### "fs-9p: TAG: Operation not permitted" or "fs-9p: MOUNT_POINT: Operation not permitted"

fs-9p runs without root and lacks an ability: `mem_phys` or `interrupt`
(first form), `pathspace` (second form). Grant all three
({doc}`integrating`).

### "fs-9p: TAG: protocol setup failed: ..."

The device was found but QEMU refused the 9P session. "Invalid argument"
is QEMU's answer when the user running QEMU can't read the shared
directory itself: fix its permissions on the host (`ls -ld` the `-fsdev`
path). A path that doesn't exist at all stops QEMU at startup instead
("cannot initialize fsdev"). For anything else, look at QEMU's own
messages (with `-D file`).

## Errors from programs using the share

### "Permission denied" (EACCES)

There are two checks, and either can refuse:

- **fs-9p's check in the guest**, against the owner and mode that `ls -l`
  shows, including search (x) permission on every directory of the path.
  Guest root passes it.
- **The host**, because QEMU runs as an ordinary host user: what that user
  can't do, nobody in the guest can. Guest root gets "Permission denied"
  too.

If root in the guest succeeds, it's the guest check: fix the mode or owner
(`chmod`/`chown` in the guest). If root fails as well, fix the permissions
on the host, for the user running QEMU.

### "Operation not permitted" (EPERM) from `chown`

Only root may give a file to another user, and the owner may only change
the group to one of their own. With `security_model=none` even root can't:
QEMU can't change real host ownership ({doc}`qemu`).

### "Read-only file system" (EROFS)

fs-9p was started with `-o ro`, or QEMU with `readonly=on` on the `-fsdev`.
`df -g` shows the `rdonly` flag only in the first case: with `readonly=on`
alone, fs-9p doesn't know, and the error comes from QEMU. Add `-o ro` to
make it visible in the guest.

### "Too many levels of symbolic links or prefixes" (ELOOP) on host symlinks

With `security_model=mapped-xattr` or `mapped-file` QEMU can't read
symlinks that were created on the host. Use `security_model=none` if the
guest must follow host symlinks. Symlinks created from the guest work in
every model.

### "Connection timed out" (ETIMEDOUT)

A request took longer than `timeout=` (default 30 s) on the host: a slow
or stuck host filesystem, or a heavily loaded host. The share recovers by
itself once QEMU finishes the request; meanwhile other requests go on. If
it happens in normal operation, raise `timeout=`. What the timed-out call
did on the host (a write, a rename) may or may not have happened.

### "Not supported" (ENOTSUP) from `mkfifo` or `mknod`

FIFOs and device nodes can't be created on the share, by design.

### "Improper link" (EXDEV) from `ln`

Hard links can't cross filesystems: between the share and a guest
filesystem, or between two shares. (`mv` copies in that case, without an
error.)

### "No space left on device" (ENOSPC)

The host filesystem under the share is full. What was written before the
error is kept; reads keep working. ext4 refuses a large write while a
little space is still free, so a small write may still succeed after a big
one failed.

### "Bad file descriptor" (EBADF) on an open file, or the mount point vanished

fs-9p isn't running any more: it was unmounted (`slay fs-9p`) or killed.
Files that were open on the share give "Bad file descriptor", paths below
the mount point "No such file or directory". Start fs-9p again; programs
must open their files again.

## Things that look wrong

### Files belong to an unexpected user

The owner comes from the QEMU `security_model` ({doc}`qemu`). With `none`,
everything belongs to the host user running QEMU; with `mapped-xattr`,
files created on the host show their host owner, files created in the guest
their creator. The guest's `/etc/passwd` turns the numbers into names, so a
host uid 1000 shows as whoever has uid 1000 in the guest.

### A setuid program runs with the caller's identity

The share is mounted `nosuid`: setuid and setgid bits are shown but
ignored, so a file on the host can't give anyone root in the guest
({doc}`behaviour`).

### Changes made on the host show up late

fs-9p runs with `cache=MS`: attributes and missing names are kept for up to
`MS` milliseconds. Use a smaller value, or no cache.

### `df` figures differ from the host's

QEMU reports the host filesystem with a larger block size, and rounds the
counts down to it; QNX `df` also counts "Used" differently from Linux `df`
({doc}`behaviour`).

### A big `ls -l` or `find` is slow

Without a cache, every lookup asks the host, and for non-root users every
directory of a path is checked too. `cache=MS` helps with repeated lookups
({doc}`performance`).

### "Resource busy" right after `slay fs-9p`, or fs-9p doesn't exit

`slay` returns at once and the mount point goes at once, but fs-9p waits
for the requests in flight before it resets the device and exits, and it
keeps the tag until then. A new fs-9p for the same tag fails with "Resource
busy" meanwhile; `pidin -p fs-9p` still shows the old one. Usually that is
a moment; a request on a slow host filesystem can take longer, and one that
never finishes keeps fs-9p (and QEMU's reset) waiting. In a script, wait
for `/dev/name/local/fs-9p/TAG` to disappear before starting again.
`slay -s KILL fs-9p` ends fs-9p at once (see the caveat in
{doc}`behaviour`).

## Collecting information

- `use fs-9p`: the version.
- `pidin -p fs-9p` and `ls /dev/name/local/fs-9p`: what is running.
- `df -g /mnt/host`: what fs-9p reports for the share, with its flags.
- `fs-9p -o debug TAG MOUNT_POINT 2> log`, after `slay fs-9p`: fs-9p in
  the foreground, logging the device setup and every 9P message as a hex
  dump.
- QEMU's `-trace 'v9fs_*' -D file`: every request QEMU receives. A failed
  request shows as `v9fs_rerror tag T id I err E`, where `I` is the 9P
  request type (110 walk, 12 open, 116 read, 118 write, 14 create, 72
  mkdir, 76 unlink, 74 rename, 26 setattr, 24 getattr) and `E` a Linux
  errno (2 ENOENT, 13 EACCES, 17 EEXIST, 28 ENOSPC, ...).
