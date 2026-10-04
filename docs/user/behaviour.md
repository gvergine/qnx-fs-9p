# Behaviour and limitations

How fs-9p behaves where a share differs from a local disk, and what it
doesn't do. Everything here was tested with QEMU 10.2.1; the tests are in
the Developer Guide ({doc}`../dev/testing`).

## Writing

The share is read-write unless mounted with `-o ro`. Creating files,
writing, appending, truncating, `chmod`, `chown`, `touch`, `mkdir`,
`rmdir`, `rm`, `mv`, symlinks and hard links work.

- **Write-through:** every `write()` goes to the host before it returns;
  there is no write-back cache. `fsync()` additionally asks the host to
  flush the file to disk.
- **Append:** with `O_APPEND` the host file is opened for appending, so
  appends from the guest and from host programs don't overwrite each
  other.
- **Special files:** `mkfifo` and `mknod` fail with `ENOTSUP` ("Not
  supported"). A FIFO or device node on a host share would mean nothing to
  QNX.
- **Paths through symlinks** work as on a local filesystem, for every
  operation: fs-9p hands the path back to the client to resolve, and both
  names of a `rename()` or a hard link are resolved that way.
- **Removed while open:** a file that is open keeps working after it's
  removed, as long as it was opened with `open()` (not just `stat()`'d).
- **Full host disk:** writes fail with `ENOSPC` ("No space left on device"),
  keeping what was written before; `mkdir` fails the same way, while reads,
  listings and removes keep working, and writing works again once space is
  freed (tested on a 32 MiB ext4 share). ext4 itself refuses a large write
  while a little space is still free (it can't reserve enough), so small
  writes may still succeed after a big one failed.
- **Truncate:** 9P can only set a file's size. Freeing a range that
  doesn't reach the end of the file (punching a hole) fails with `ENOTSUP`,
  and `posix_fallocate()` only extends the file without reserving space.

## Read-only mount

With `-o ro`, opening for write, `O_TRUNC`, creating a file, `rm`,
`mkdir`, `mv`, `ln`, `chmod`, `chown` and `touch` fail with `EROFS`
("Read-only file system"), and `df -g` shows `rdonly`.

Note: `rm` (without `-f`) on an interactive terminal sees a non-writable
file and asks for confirmation before it tries the unlink, which then fails
with `EROFS`.

## QEMU `security_model` and symlinks

With `security_model=mapped-xattr` (the usual recommendation for running
QEMU unprivileged), QEMU reads a symlink's target by opening the host file
with `O_NOFOLLOW`, because in that model the guest's symlinks are stored as
regular files. Real symlinks that already exist on the host can't be read
that way: `Treadlink` fails with `ELOOP`. `ls -l` then prints
"Too many levels of symbolic links or prefixes", and `fs-9p` can't follow those links.
This is QEMU behaviour, and a Linux guest sees the same thing.

With `security_model=none` (or `passthrough`, which needs root) host
symlinks work: `ls -l` shows the target, `readlink` returns it, and fs-9p
follows them (see below).

## Symlink following

The 9P server doesn't cross symlinks during Twalk. fs-9p resolves them by
redirecting the client: when an open reaches a symlink (mid-path, or as the
last component of a following lookup), fs-9p reads the target and replies
`_IO_CONNECT_RET_LINK` with the rewritten path. The client library then
looks the new path up again.

- A relative target is resolved against the link's directory under the
  mount point.
- An absolute target names a path **in the guest**, not on the host, the
  same as in a Linux guest. A link to `/home/user/x` on the host therefore
  points at `/home/user/x` in QNX.
- `lstat()` (sent with `mode = S_IFLNK`) and `open(O_NOFOLLOW)` don't
  follow.
- Loop detection (`ELOOP`) is left to the client library's link limit.

The reply formats were found by experiment, because the SDP docs don't
describe them. See {doc}`../dev/op-mapping`.

## Ownership and permissions

`uid`, `gid` and mode bits come straight from the server. The guest's
`/etc/passwd` maps the numbers to names (e.g. host uid 1000 shows as
`qnxuser` in the default mkqnximage image). fs-9p checks permissions
against those values as a local filesystem would: opening a file, creating
one (write and search permission on the directory), `chmod` (owner or
root), `chown` (root only; the owner may change the group to one of their
own). QEMU, running as the host user, enforces host permissions in
addition, so a guest root user still can't write files the host user
can't.

New files belong to the guest user who creates them. What the host records
depends on QEMU's `security_model`:

| security_model | Owner and mode of new files | `chown` |
|---|---|---|
| `mapped-xattr` (recommended for unprivileged QEMU) | The creator's uid/gid and the mode, stored in `user.virtfs.*` xattrs; the host file belongs to the host user | Works; stored in the xattrs |
| `mapped-file` | As `mapped-xattr`, stored in `.virtfs_metadata` directories on the host (hidden from the guest) | Works; stored in the metadata files |
| `none` | The host user who runs QEMU; the guest sees that uid | Fails with `EPERM` unless it changes nothing |
| `passthrough` | Real host ownership; needs QEMU running as root | Real `chown` (tested 2026-10-04 with QEMU as root). QEMU, as root, also ignores host permissions: only fs-9p's own checks apply |

There are no options to remap uids or gids.

Search (x) permission is checked on every directory a path passes
through, for non-root users: a file inside a directory the user can't
search can't be opened, created, renamed or `stat()`'d, even with its full
path. This costs one extra request per directory on each lookup by a
non-root user (root, who may search any directory, skips it); with
`cache=MS` those lookups are cached too ({doc}`performance`).

## Running binaries from the share; setuid

QNX binaries on the share can be run directly (`/mnt/host/build/mytest`).
The mount is `nosuid`: setuid and setgid bits are ignored when a binary on
the share is run, although `ls -l` still shows them. File ownership comes
from the host, and with `mapped-xattr` any host user can make a file look
root-owned and setuid (an xattr on their own file). Honoring the bits would
let the share grant root in the guest; this was verified before the mount
was made `nosuid`.

After a binary from the share exits, the process manager keeps its mapping
of the file open for a few seconds, so one fid stays in use on the server
for that time.

## Disk usage

`st_blocks` (and so `du`) is derived from the file size rounded to the
mount's block size, not taken from the host's allocated block count.
Totals can differ from `du` on the host, for example for directories full
of tiny files or for sparse files. Overriding it needs the per-client stat
format conversion, which is internal to `iofunc_stat_default()`.

## df

`df` shows the mount tag as the filesystem and `9p` as the type. The
numbers come from the host filesystem holding the share, with two
differences from `df` on the host:

- QEMU multiplies the block size so that it fits the negotiated msize
  (520192 bytes for a 4096-byte host filesystem at the default 512 KiB)
  and rounds the block counts down to it. Totals can be up to one such
  block lower than on the host.
- QNX `df` computes "Used" as total minus available, so it includes the
  blocks the host reserves for root. Linux `df` uses total minus free.

## Changes made on the host

There is no cache, so changes made on the host show at once in the guest:
new or removed files, sizes, modes, a directory replaced by a file. Files
the guest has open behave as local files do on Linux, because QEMU keeps
the host file open: a file removed or renamed over on the host stays
readable through descriptors opened before, and reads see what the host
appends. A guest process whose current directory is removed on the host
gets "No such file or directory" for relative paths.

## Metadata cache (`cache=MS`)

By default nothing is cached: every `stat()` asks the server again and
every open walks the path from the root, so host changes show at once.

`-o cache=MS` keeps file attributes, and names found missing, for up to MS
milliseconds (at most 4096 entries):

- `stat()`, `lstat()`, `ls -l` and the like of a cached path send no
  request at all; an open of a name known to be missing fails at once
  (useful for builds probing include paths).
- Changes made through fs-9p update or drop the affected entries at once
  (writes, creates, removes, renames of files and whole directories,
  chmod/chown/utime, links). Changes made on the host may show up to MS
  late: a new host file can look missing, a modified one keep its old size
  and times, for up to MS.
- File contents are never cached; reads and writes always go to the host.
- Search permission for non-root users is checked from cached directory
  attributes, which can likewise be up to MS old.

A repeated `ls -l` of 500 files runs about twice as fast with
`cache=5000` ({doc}`performance`).

## Requests in flight

Up to `requests=` 9P requests (default 4, at most 8) are in flight at once,
each with its own buffers (2 × msize of memory each, so 4 MiB at the
defaults). A slow request ties up only its own slot. A single program still
issues one request at a time.

## Unmount and restart

`slay fs-9p` unmounts (SIGTERM; Ctrl-C does the same for an instance in
the foreground, and SIGHUP too unless it is ignored, as under `nohup`).
fs-9p removes the mount point, waits for the request in flight, resets the
device, releases the tag claim and exits with status 0. Programs that still
have files open get errors from then on. A new fs-9p for the same tag can
be started straight away.

`slay -s KILL fs-9p`, or a crash, skips all that. A new instance still
takes over (it resets the device the old one left behind), but if a
request was in flight, QEMU finishes it and writes the reply into the dead
process's buffer, which the kernel may already have given to another
process. Prefer plain `slay`.

## Interrupting a program

A signal to a program waiting on the share takes effect between two
requests: a long read or write (split into msize-sized requests) stops at
the next one and returns what it transferred, or fails with `EINTR` if
nothing was. The request in flight itself is not cancelled. 9P's Tflush
would not help with QEMU: according to its source (`v9fs_flush` in
`hw/9pfs/9p.c`, not verified by experiment), QEMU answers a flush only
after the flushed request has finished.

## One fs-9p per mount tag

The fs-9p serving a tag holds the name `/dev/name/local/fs-9p/<tag>`, so
`ls /dev/name/local/fs-9p` lists the tags being served. A second fs-9p for
the same tag fails with "Resource busy". Any process may create names
under `/dev/name/local`, so another process holding that name blocks
fs-9p in the same way. Two QEMU devices with the same mount tag can't both
be mounted; give each share its own tag.

## Timeouts

A request with no reply within `timeout=` (default 30 s) fails with
`ETIMEDOUT` ("Connection timed out"). QEMU may still be working on it, so
its slot stays out of use until QEMU finishes; the other slots go on
serving, and the mount recovers by itself. If every slot is held that way,
new requests fail with `ETIMEDOUT` after the timeout instead of hanging,
and `close()` returns at once (its Tclunk is sent later). Whatever the
timed-out request did on the host (a write, a rename) may or may not have
happened.

After `slay fs-9p`, fs-9p waits for the requests in flight before it
exits (`slay` itself returns at once); for one that QEMU never finishes (a
hung host filesystem), the device reset waits too, so fs-9p may not exit. `slay -s KILL` then works, with the caveat above.
