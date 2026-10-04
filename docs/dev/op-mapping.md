# QNX resource manager operations to 9P2000.L

Reads, the write path and namespace operations.
Source: `src/resmgr_connect.c`, `src/resmgr_io.c`, `src/client.c`.

## Connect messages

| QNX message | Client calls | fs-9p | 9P |
|---|---|---|---|
| `_IO_CONNECT` open / combine (`stat()`, `lstat()`, `open()`, `opendir()`) | open, stat, lstat, opendir | Walk the path remainder from the root fid (for a non-root client one component at a time, with a Tgetattr and a search-permission check on each directory passed), Tgetattr, check with `iofunc_open()`, attach a per-open node with `iofunc_ocb_attach()`. A real `open()` (subtype `_IO_CONNECT_OPEN`) then opens the file on the server at once, so `O_TRUNC` takes effect at open and the file stays usable if someone removes it (`O_TRUNC`, `O_APPEND`, `O_SYNC`/`O_DSYNC` mapped to the Linux flags). The combine opens behind `stat()` and the like stay lazy. With `-o ro`, write access and `O_TRUNC` get `EROFS`. | Twalk (≤ 16 names each), Tgetattr, Tlopen |
| same, `O_CREAT` and the file is missing | open, creat, `>` in the shell | Walk the parent, Tgetattr it, require write and search permission (`iofunc_check_access()`), Tlcreate with the creator's egid, then Tsetattr the owner to the creator's euid/egid unless root (fs-9p attaches as uid 0). The new file opens without a check against its own mode, so `open(O_CREAT\|O_WRONLY, 0444)` works for the creator. | Twalk, Tgetattr, Tlcreate, Tsetattr, Tgetattr |
| same, with `cache=MS` and a fresh cache entry | stat, lstat, access | A combine open with no read or write access gets a node built from the cached attributes, with no fid (walked later only if an I/O message needs one); a name cached as missing gets `ENOENT` (unless `O_CREAT`). Non-root clients need every ancestor cached and searchable, or the open goes the normal way. | none |
| same, path crosses a symlink, or last component is a symlink and the caller follows | open, stat | Read the link, reply `_IO_CONNECT_RET_LINK` with the rewritten path | Twalk, Treadlink, Tclunk |
| `_IO_CONNECT_READLINK` | readlink | Walk, check that it is a symlink (`EINVAL` otherwise: with mapped-xattr QEMU's Treadlink just reads the file), read the target, reply link-reply form with the byte count as status | Twalk, Tgetattr, Treadlink, Tclunk |
| `_IO_CONNECT_MKNOD`, mode `S_IFDIR` | mkdir | Walk the parent, require write and search permission, Tmkdir with the creator's egid, then Tsetattr the owner unless root. Other file types (FIFOs, device nodes): `ENOTSUP`. | Twalk, Tgetattr, Tmkdir (Twalk, Tsetattr) |
| `_IO_CONNECT_UNLINK` | unlink, rmdir, rm | mode `S_IFDIR` means rmdir (`AT_REMOVEDIR`); unlink sends `S_IFLNK` ("don't follow"). Parent permission check; in a sticky directory only root, the directory's owner or the entry's owner may remove it. | Twalk, Tgetattr, Tunlinkat |
| `_IO_CONNECT_RENAME`, extra `_IO_CONNECT_EXTRA_RENAME` | rename, mv | The connect path is the new name, the extra the old one, both relative to the mount. Permission checks on both parents, sticky rule on the old one. | Twalk ×2, Tgetattr ×2, Trenameat |
| `_IO_CONNECT_LINK`, extra `_IO_CONNECT_EXTRA_SYMLINK` | symlink, ln -s | The extra is the target as written. Tsymlink with the creator's egid, then the owner. | Twalk, Tgetattr, Tsymlink (Twalk, Tsetattr) |
| `_IO_CONNECT_LINK`, extra `_IO_CONNECT_EXTRA_LINK` | link, ln | The extra is the existing file, relative to the mount. | Twalk ×2, Tgetattr, Tlink |
| any of these, a symlink inside the connect path | | Redirect the client (`_IO_CONNECT_RET_LINK`), as for open. A symlink inside the *extra* path (rename's old name, a hard link's source) can't be redirected, but the client library resolves that path through fs-9p first (an open with `S_IFLNK`, following the redirect) and sends the resolved path, so it never contains one (observed). fs-9p would answer `EXDEV`. | Twalk, Treadlink |
| any of these on the mount point itself | mkdir -p | Creating: `EEXIST` (mkdir -p relies on it). Removing or renaming: `EBUSY`. | none |

## I/O messages

| QNX message | Client calls | fs-9p | 9P |
|---|---|---|---|
| `_IO_READ` / `_IO_READ64`, file | read, pread (`_IO_XTYPE_OFFSET`) | Tlopen on first read. Loop over Treads of at most msize−11 (or iounit) bytes until the request is filled or EOF; each chunk is `resmgr_msgwrite()`'d from the transport buffer straight into the client. | Tlopen, Tread |
| `_IO_READ`, directory | readdir | Tlopen (`O_DIRECTORY`) on first read. One Treaddir from the cookie in `ocb->offset`; entries converted to 8-byte-aligned `struct dirent` until the client buffer is full; `ocb->offset` = cookie of the last entry kept. | Tlopen, Treaddir |
| `_IO_STAT` | fstat, and stat via combine | Refresh with Tgetattr, then `iofunc_stat_default()` | Tgetattr |
| `_IO_WRITE` / `_IO_WRITE64` | write, pwrite (`_IO_XTYPE_OFFSET`) | `iofunc_write_verify()`, then Twrites of at most msize−23 (or iounit) bytes; each chunk is `resmgr_msgread()` from the client's message straight into the request buffer. Write-through, no cache. With `O_APPEND` the host fd appends; the offset afterwards comes from a Tgetattr. | Twrite (Tgetattr for `O_APPEND`) |
| `_IO_SPACE` | ftruncate, truncate | `ftruncate()` arrives as `F_FREESP` with `len` 0 for both shrinking and growing (observed): the size becomes `start`. Freeing a range short of the end (a hole) is `ENOTSUP`; `F_ALLOCSP`/`F_GROWSP` only grow. Replies with the new size. | Tgetattr, Tsetattr |
| `_IO_CHMOD`, `_IO_CHOWN`, `_IO_UTIME` | chmod, chown, utime, touch | `iofunc_chmod()`/`iofunc_chown()`/`iofunc_utime()` check the POSIX rules (owner or root; chown restricted) and update the cached attribute; fs-9p sends the result, then re-reads the attribute. chmod sends the whole mode with the Linux type bits: with mapped-xattr QEMU stores it as the full `st_mode` (observed). | Tsetattr, Tgetattr |
| client unblock (signal or timeout) | | A long `_IO_READ`/`_IO_WRITE` checks `MsgInfo()` for `_NTO_MI_UNBLOCK_REQ` before each request and stops with the count so far, or `EINTR` | none |
| SIGTERM / SIGINT / SIGHUP to fs-9p | slay | `resmgr_detach()` of the mount point, wait for the request in flight, Tclunk the root, reset the device, release the tag claim, exit 0 | Tclunk |
| `_IO_SYNC` | fsync, fdatasync | `iofunc_sync_verify()` (needs `IOFUNC_PC_SYNC_IO` in the mount, else `EINVAL`, observed), Tfsync with `datasync` for `O_DSYNC` | Tfsync |
| `_IO_LSEEK` | lseek, rewinddir | `iofunc_lseek_default()`; for `SEEK_END` the size is re-read first, since the host may have changed it | Tgetattr for `SEEK_END` |
| `_IO_DEVCTL` `DCMD_FSYS_STATVFS` | statvfs, fstatvfs, df | Tstatfs on the open's fid, converted to `struct __msg_statvfs` (table below) | Tstatfs |
| `_IO_DEVCTL` `DCMD_FSYS_MOUNTED_ON`, `MOUNTED_AT` | df, mount | The mount tag, and the mount point | none |
| `_IO_DEVCTL` `DCMD_ALL_GETMOUNTFLAGS` | the process manager, on exec | `iofunc_devctl_default()` reports the mount flags: `_MOUNT_NOSUID` (setuid and setgid bits are ignored), plus `_MOUNT_READONLY` with `-o ro` | none |
| `_IO_DEVCTL`, other commands | | `iofunc_devctl_default()` | none |
| `_IO_MMAP` | mmap, exec of a binary on the share | `iofunc_mmap_default()`, which attaches a second OCB to the node for the memory manager. Pages are then read through that OCB with `_IO_READ`. The node and its fid are released with its last OCB (`ocb_calloc`/`ocb_free` hooks count them). | as `_IO_READ` |
| `_IO_CLOSE` (last close of an OCB) | close | `iofunc_close_ocb_default()`, then Tclunk the node's fid | Tclunk |

## Attribute mapping (Rgetattr to `iofunc_attr_t`)

| 9P | QNX |
|---|---|
| mode file type bits (Linux octal values) | `S_IF*` via an explicit table; unknown types show as regular files |
| mode & 07777 | permission bits |
| uid, gid, nlink, size | uid, gid, nlink, nbytes |
| qid.path | inode (1 if 0) |
| atime/mtime/ctime sec + nsec | atime/mtime/ctime + `_ns` |
| blocks, blksize, rdev | not used yet (see {doc}`../user/behaviour`) |

## Filesystem statistics (Rstatfs to `struct __msg_statvfs`)

| 9P | QNX |
|---|---|
| bsize | `f_bsize` and `f_frsize` (the block counts are in this unit) |
| blocks, bfree, bavail | `f_blocks`, `f_bfree`, `f_bavail` |
| files, ffree | `f_files`, `f_ffree`, and `f_favail` = ffree (9P has no separate count) |
| fsid (64 bits) | `f_fsid`: high and low halves XORed into 32 bits |
| namelen | `f_namemax` |
| type | not used; `f_basetype` is `"9p"` |
| | `f_flag` = `ST_NOSUID`, plus `ST_RDONLY` with `-o ro`, matching the mount flags |

## Errors

A partial Rwalk carries no error code. fs-9p reports `ENOTDIR` when the
walk stopped after something that isn't a directory (a file used as a
directory), and `ENOENT` otherwise.

`Rlerror` carries Linux errno numbers. `src/errno_map.c` maps them by name
to QNX values (117 names, generated from the Linux asm-generic headers);
anything else becomes `EIO`. Transport failures come through as `EIO`
(`ETIMEDOUT` for a timeout).

## Reply formats found by experiment

The SDP docs don't specify these. Each was confirmed against the QNX 8.0
client library in the aarch64 guest (2026-10-04):

1. **readlink:** the client's reply buffer is laid out as
   `struct _io_connect_link_reply` + 16 `struct _io_connect_entry` +
   `PATH_MAX` (`reply_max` = 1556). The working reply is the header
   (`nentries` = 0, `path_len` = len + 1) followed by the NUL-terminated
   target, with the byte count as a plain status.
   - Plain bytes at offset 0: readlink returns an empty string.
   - Status `_IO_CONNECT_RET_LINK`: `EINVAL`.
2. **Symlink redirect on open:** the same header and path, with status
   `_IO_CONNECT_RET_FLAG | _IO_CONNECT_RET_LINK`, and `file_type` and
   `eflag` copied from the request. The client resolves the returned
   absolute path again.
3. **lstat vs stat:** `lstat()` sends the open with `connect.mode` =
   `S_IFLNK` (0120000), while `stat()` and `open()` send `mode` = 0. Neither
   sets `O_NOFOLLOW` in `ioflag`.
4. **umask:** the client library applies the umask before sending:
   `connect.mode` arrived as 0644 under umask 022 and 0600 under 077, so
   fs-9p passes it to Tlcreate unchanged.
5. **Namespace messages:** what `mkdir`, `rmdir`, `rm`, `mv`, `ln -s` and
   `ln` send (debug log of the connect fields, 2026-10-04):

   | Call | Subtype | mode | extra_type | extra |
   |---|---|---|---|---|
   | mkdir | `MKNOD` (5) | `S_IFDIR` + permissions, umask applied | none | none |
   | rmdir | `UNLINK` (3) | `S_IFDIR` | none | none |
   | unlink | `UNLINK` (3) | `S_IFLNK` | none | none |
   | rename | `RENAME` (4) | `S_IFLNK` | `EXTRA_RENAME` (9) | old path, mount-relative, NUL included |
   | symlink | `LINK` (7) | `S_IFLNK` + 0755 | `EXTRA_SYMLINK` (2) | target as written |
   | link | `LINK` (7) | 0 | `EXTRA_LINK` (13) | existing file, mount-relative |
   | mkfifo | `MKNOD` (5) | `S_IFIFO` + permissions | none | none |

   A rename or link whose other path is on another filesystem never
   reaches fs-9p: the client library fails it with `EXDEV`.
