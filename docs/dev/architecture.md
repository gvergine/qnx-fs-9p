# Architecture and design

How fs-9p is built, and why. Read this before changing anything; the
details of each 9P message are in {doc}`op-mapping`, the virtio findings in
{doc}`virtio-notes`.

## Constraints

These shaped everything and still hold:

- **Target:** QNX 8.0, aarch64le only, QEMU's `virt` machine, the
  virtio-9p device on the virtio-mmio transport. x86_64 and virtio-pci were
  dropped ({doc}`virtio-notes`, "Why not virtio-pci"): on `virt`, MMIO
  needs no PCI server and no I/O privilege.
- **No network:** QEMU's own 9P server is the only peer. No TCP transport,
  no `diod`.
- **`lib9p` is portable:** standard C headers only, no I/O, no allocation,
  no `errno`; it builds and is tested on plain Linux.
- **One coupling point:** the resource manager talks to the virtio code
  only through `src/transport.h`; it never touches registers, rings or DMA
  memory.
- **Nothing copied from the QNX SDK:** headers are included from the
  installed SDP; no sample code.
- **Verified, not assumed:** any QNX or QEMU behaviour the headers don't
  settle was checked by experiment in the guest (see the end of this page).

Out of scope, on purpose: 9P dialects other than 9P2000.L (9P2000,
9P2000.u); 9P servers other than QEMU (`diod`, network servers: NFS and
CIFS already cover network sharing on QNX); authentication (`Tauth`),
extended attributes (`Txattrwalk`/`Txattrcreate`) and file locking
(`Tlock`/`Tgetlock`), which a host share doesn't need so far; prebuilt
binaries. Other hypervisors that offer virtio-9p over virtio-mmio may
work but are untested.

## Source map

| File | Role |
|---|---|
| `lib9p/include/lib9p/p9.h` | The codec API: message types, constants, encoders, decoders, id pool. |
| `lib9p/src/wire.h` | Little-endian byte cursors with a sticky failure flag. |
| `lib9p/src/encode.c`, `decode.c` | T-message encoders, R-message decoders. |
| `lib9p/src/idpool.c`, `p9.c` | Fid/tag pool; names and error strings. |
| `src/transport.h` | The transport interface (request slots). |
| `src/transport_virtio.c` | virtio-mmio discovery, device setup, virtqueue, interrupt thread, DMA, tag claim. |
| `src/client.c`, `client.h` | The 9P session: one call per 9P operation, fids, walks, timeouts. |
| `src/errno_map.c` | Linux errno numbers (in `Rlerror`) to QNX values, by name. |
| `src/resmgr_core.c` | Resource manager setup, attribute conversion, node lifetime, signals and shutdown. |
| `src/resmgr_connect.c` | Connect messages: open, create, readlink, mkdir, unlink, rename, link; permissions; symlink redirects. |
| `src/resmgr_io.c` | I/O messages: read, readdir, write, stat, lseek, chmod/chown/utime, truncate, sync, devctl, close. |
| `src/cache.c`, `cache.h` | The optional metadata cache (`cache=MS`). |
| `src/main.c` | Command line, startup order. |
| `src/fs-9p.use.in` | The `use` message (the version is filled in by CMake). |

## A request, end to end

`read(fd, buf, 1 MiB)` by a program in the guest, on a file opened earlier:

1. The C library sends an `_IO_READ` message to fs-9p's channel; the
   program is reply-blocked until fs-9p answers.
2. A thread of fs-9p's pool receives it; the resource manager library finds
   the OCB (open control block) for the file descriptor and calls
   `fs9p_read_h()` with the node's attribute locked.
3. `read_file()` splits the request into chunks of at most msize − 11 bytes
   (or the server's `iounit`), and for each one calls `fs9p_read()`.
4. `fs9p_read()` takes a request slot from the transport, encodes a Tread
   into the slot's request buffer, and calls `rpc()`.
5. The transport puts the slot's two descriptors (request, reply) on the
   virtqueue's available ring, notifies the device and waits.
6. QEMU reads the request, reads the file on the host straight into the
   slot's reply buffer, puts the chain on the used ring and raises the
   interrupt.
7. The interrupt thread acknowledges the device, takes the chain off the
   used ring, marks the slot done and wakes the waiter.
8. `fs9p_read()` decodes the Rread and hands its data to a "sink", which
   `resmgr_msgwrite()`s it straight from the reply buffer into the
   program's buffer at the right offset. No intermediate copy.
9. After the last chunk, `read_file()` sets the byte count and the library
   replies to the program, which unblocks.

## lib9p: the protocol codec

9P2000.L in brief: every message is `size[4] type[1] tag[2]` followed by
its body, all little-endian; an R-message's type is its T-message's type
+ 1; strings are `len[2]` plus bytes, without a NUL; a qid is 13 bytes,
`type[1] version[4] path[8]`. `Rlerror` carries a Linux errno. The message
numbers are in `p9.h`; the layouts come from the Linux client (see
"References" below), since 9P2000.L has no formal specification.

- **Encoders** (`p9_enc_t*`) write a complete message into a caller's
  buffer and return its length, or `P9_E_NOSPACE`/`P9_E_INVAL`. They never
  write past the capacity they are given. `p9_enc_twrite()` writes only the
  header and reserves the payload, so the caller can put the data there
  first (straight from the client's message).
- **Decoders** (`p9_dec_r*`) validate the header and the expected type,
  bounds-check every length and count, reject trailing bytes, and return
  views (pointers into the message) for strings and data. A malformed reply
  gives an error code, never an overrun. `p9_dirent_next()` iterates the
  packed entries of an Rreaddir.
- **`wire.h`** cursors have a sticky failure flag: once a read or write
  would cross the end, nothing more happens and the caller checks the flag
  once at the end. All integers are explicit little-endian, byte by byte.
- **Id pool** (`p9_idpool_*`): a bitmap allocator over caller storage that
  hands ids out round-robin, so a just-freed fid isn't reused at once (this
  keeps QEMU traces readable and makes use-after-clunk bugs visible).

Layouts follow the Linux 9P client; the kernel headers confirm the message
numbers, open flags and the setattr structure; QEMU confirmed the rest by
accepting and acting on the messages.

## The virtio-mmio transport

### Interface

`src/transport.h` lends buffers through **request slots**: `get()` a free
slot, encode into `buf()`, `rpc()` sends it and returns a pointer to the
reply (valid until `put()`), `put()` gives the slot back. Several slots let
several requests be in flight (`-o requests=`, default 4, at most 8).

### Finding the device

`vio9p_open()`:

1. **Claims the tag** with `name_attach(NULL, "fs-9p/<tag>", 0)` before
   touching any register. `name_attach()` refuses a duplicate (`EEXIST` →
   `EBUSY`) and the process manager removes the name when its owner dies.
   This is how fs-9p tells "in use" from "left behind by a killed
   instance": the device status can't (DRIVER_OK stays set).
2. **Maps all 32 virtio-mmio slots** (`mmap_device_memory()`, needs
   `mem_phys`) and scans them from the top, as QEMU fills them.
3. **Probes each virtio-9p device** (magic, version 1 or 2, device id 9): a
   free one (status 0) gets ACK|DRIVER, its tag is read (allowed during
   feature negotiation, virtio 1.2 §3.1.1 step 4) and it is reset if the tag
   differs. A device with a nonzero status is past that step and is only
   read. If it has our tag, the claim proves its owner is dead: it is reset
   and taken over.
4. **Negotiates features** (the mount-tag feature, `VIRTIO_F_VERSION_1` on
   modern devices), sets up queue 0 and starts the interrupt thread, then
   sets DRIVER_OK.

The two MMIO layouts are handled separately: version 1 (legacy, QEMU's
default) uses GuestPageSize and QueuePFN, version 2 the 64-bit queue
address registers and FEATURES_OK.

### Queue and memory

- The ring has 16 entries: 8 two-descriptor chains. Slot *i* owns
  descriptors 2*i* (request, device-readable) and 2*i*+1 (reply,
  device-writable), set up once; a request only writes its length and the
  available-ring entry.
- DMA memory comes from `mmap(MAP_SHARED|MAP_PHYS|MAP_ANON)` with
  `mem_offset64()` for the physical address. `virt` has no IOMMU, so
  device addresses are guest physical. The ring and each slot's buffers
  (2 × msize) are separate regions, so no single large contiguous block is
  needed.
- Memory ordering (aarch64): a C11 release fence before publishing the
  available index; `__cpu_membarrier()` (`dmb sy`) between that store and
  the notify write, because the notify register is device memory and a C11
  fence doesn't order against it; an acquire fence after reading the used
  index, before reading the used elements and the reply.

### Completion

`InterruptAttachThread()` binds the calling thread, but requests run on
resource-manager threads, so a dedicated **interrupt thread** waits with
`InterruptWait(_NTO_INTR_WAIT_FLAGS_UNMASK)`, acknowledges InterruptStatus,
calls `reap()` (take every returned chain off the used ring, mark its slot
done, broadcast) and loops. Waiters also call `reap()` before sleeping,
under the same lock, so a completion is never missed and a spurious wakeup
(observed) costs nothing. Completion is always decided from the
used ring, never from the wakeup itself. Chains come back in any order;
the head descriptor names the slot.

### Timeouts

A request with no reply within `timeout=` returns `ETIMEDOUT`. The device
may still complete it, and QEMU reads Twrite data straight from the request
buffer, so the slot is marked **stale**: it stays out of use until `reap()`
sees its chain return, then frees itself. The other slots carry on. Waiting
for a free slot is bounded by the same timeout, so a mount whose slots are
all stale fails fast instead of hanging.

### Close

`close()` resets the device first (QEMU's reset waits for its own requests
to finish, so nothing is written into our memory afterwards), frees the DMA
memory and releases the tag claim. The register window stays mapped and the
interrupt thread isn't joined, because `close()` runs just before exit.

## The 9P session (`client.c`)

- **One public call per 9P operation** (`fs9p_walk`, `fs9p_read`,
  `fs9p_lcreate`, ...). Each takes a transport slot for its whole
  encode-send-decode sequence; the slot number + 1 is the request's tag
  (unique among outstanding requests). `Tversion` uses `P9_NOTAG`.
- **`call()`** sends what the caller encoded, checks the reply's tag and
  type, and turns `Rlerror` into a negative QNX errno through
  `errno_map.c` (Linux and QNX numbers differ: `ENOTEMPTY` is 39 vs 93).
- **The session attaches once, as uid 0**, at startup (Tversion, then
  Tattach of the export root, fid 0).
- **Fids** come from a 1024-entry pool with its own small lock. Every fid
  created by a walk is clunked. A fid involved in a walk or clunk that
  timed out is **quarantined** (never handed out again), because the
  server may or may not hold it.
- **Walks** take up to 16 names per Twalk. A partial walk carries no error
  code, so it is interpreted: stopped at a symlink with more path behind it
  → `-ENOTSUP` and the offset of the link, so the caller can redirect;
  stopped after a file → `-ENOTDIR`; otherwise `-ENOENT`.
- **`fs9p_walk_check()`** walks one name at a time and calls back with
  each directory's attributes before searching it: the resource manager
  uses it to check search permission for non-root clients.
- **Sinks and sources:** reads hand reply data to a callback while the slot
  is held (no copy), writes get their payload from a callback that copies
  from the client's message straight into the request buffer.
- **Deferred clunks:** a Tclunk that can't get a slot (all stale) is queued
  and sent by the next call that gets one, so `close()` never hangs.

## The resource manager

### Startup (`main.c`, `resmgr_core.c`)

1. Parse options; block SIGTERM, SIGINT and SIGHUP before any thread
   exists (every thread inherits the mask).
2. Open the transport, start the session, set up the cache.
3. `fs9p_resmgr_run()`: `dispatch_create()`, `resmgr_attach()` of the mount
   point (`_FTYPE_ANY`, `_RESMGR_FLAG_DIR`: we own every path below it), a
   thread pool (2 to 8 threads), `procmgr_daemon()` (detaches from the
   terminal and returns status 0 to the parent; it doesn't fork, so the
   interrupt thread survives), a shutdown thread, then the pool takes over.

### Nodes and OCBs

Every open gets its own **node**: an `iofunc_attr_t` (first, so
`ocb->attr` points at the node) filled from Tgetattr, the 9P fid, the path,
whether it's open on the server, and a count of OCBs. Nodes are not shared
between opens: with no cache to keep coherent, sharing would buy nothing.

A node can have **more than one OCB**: when a file is mapped (every exec of
a binary on the share), `iofunc_mmap_default()` attaches a second OCB for
the memory manager. The mount's `ocb_calloc`/`ocb_free` hooks count OCBs
per node; the last `ocb_free` hands the node back through a thread-local
variable, and `close_ocb` (or the open error path) clunks the fid and frees
it once iofunc is done with the attribute. Freeing on the first close led
to a double free and an abort (found by running a binary from the share).

### Open (`fs9p_open_h`)

1. With `-o ro`, refuse write access and `O_TRUNC`.
2. Get the client's credentials (`iofunc_client_info_ext()`, with groups).
3. Metadata cache, if on: a name known missing fails at once; a
   `stat()`-like open (combine message, no read/write access) of a cached
   path gets a node without a fid, and no request at all.
4. Walk the path (`walk_as()`: per-directory search checks for non-root).
   A symlink in the middle → reply `_IO_CONNECT_RET_LINK` with the rewritten
   path; the client library resolves it again ("redirect").
5. Missing and `O_CREAT` → `create_file()`.
6. Tgetattr. A symlink as the last component is followed (redirect) unless
   `lstat()` (connect mode `S_IFLNK`) or `O_NOFOLLOW`.
7. `iofunc_open()` checks permissions; then a real `open()` (connect
   subtype `_IO_CONNECT_OPEN`) opens the file on the server at once
   (Tlopen): `O_TRUNC` must take effect at open, and a file removed while
   open must stay usable (QEMU keeps the host file open). `stat()`-like
   opens stay lazy.
8. `iofunc_ocb_attach()`.

**Create:** walk the parent and check write and search permission on it
(`open_parent()`), Tlcreate with the creator's egid, then Tsetattr the
owner to the creator unless root (the session is attached as uid 0, so the
server records root otherwise). The new file opens without a check against
its own mode: `open(O_CREAT|O_WRONLY, 0444)` must work for its creator.

### Namespace operations

`mkdir` (mknod with `S_IFDIR`), `rmdir`/`unlink` (unlink; `S_IFDIR` means
rmdir), `rename` (old path in the extra), `symlink` and `link` (link, with
the target or the existing file in the extra) all start with
`open_parent()`: walk the directory, redirect on a symlink, check write and
search permission. In a sticky directory only root, the directory's owner
or the entry's owner may remove or rename it. A path in the *extra*
(rename's old name, a link's source) can't be redirected, but the C library
resolves it through fs-9p before sending the request (observed with
`-o debug`: an open of the old path, the redirect, then the rename with
the resolved path), so renames and links through symlinked directories
work; fs-9p's `EXDEV` for a symlink there is only a safety net. FIFOs and device nodes: `ENOTSUP`. Creating at
the mount point itself gives `EEXIST` (`mkdir -p` relies on it), removing
or renaming it `EBUSY`.

### I/O

- **read:** chunks of at most msize − 11 bytes (or `iounit`), each
  delivered with `resmgr_msgwrite()`; between chunks, `MsgInfo()` tells
  whether the client asked to unblock (a signal) and the read stops with
  what it has. `pread` uses `_IO_XTYPE_OFFSET`. At most 1 GiB per call.
- **readdir:** one Treaddir from the cookie kept in `ocb->offset`; entries
  converted to 8-byte-aligned `struct dirent` in a 4 KiB buffer, as many as
  fit the client's buffer; `rewinddir()` is an lseek to 0.
- **write:** write-through, chunks of at most msize − 23 bytes, data read
  from the client's message into the request buffer; the same unblock
  check. With `O_APPEND` the host file was opened for appending; the new
  offset comes from a Tgetattr.
- **stat:** a stat inside the same receive as its open (a `stat()` combine
  message) reuses the open's attributes; otherwise Tgetattr (or the cache).
- **chmod/chown/utime:** `iofunc_chmod()` and friends check the POSIX rules
  (chown restricted) and update the cached attribute; fs-9p sends the result
  in a Tsetattr and re-reads. chmod sends the whole mode with the Linux file
  type bits: with mapped-xattr QEMU stores it as the full `st_mode`.
- **truncate:** `ftruncate()` arrives as `F_FREESP` with length 0 for
  growing and shrinking alike; the size becomes `start`. Punching a hole
  is `ENOTSUP`.
- **sync:** Tfsync; the mount advertises `IOFUNC_PC_SYNC_IO`, without which
  `iofunc_sync_verify()` fails `fsync()` with `EINVAL`.
- **devctl:** `DCMD_FSYS_STATVFS` (Tstatfs), `DCMD_FSYS_MOUNTED_ON`/`_AT`
  (the tag and the mount point, for `df` and `mount`); everything else to
  `iofunc_devctl_default()`, which reports the mount flags (`_MOUNT_NOSUID`,
  plus `_MOUNT_READONLY` with `ro`) when the process manager asks during an
  exec.

### Permissions and ownership

fs-9p enforces POSIX permissions in the guest, using the owners and modes
the server reports, with the iofunc helpers (`iofunc_open()`,
`iofunc_check_access()`, `iofunc_chmod()`, ...) and the client's
credentials. Search permission is checked on every directory of a path for
non-root clients. QEMU adds the host's own checks for its host user. The
mount is `nosuid`: ownership comes from the host, and with mapped-xattr any
host user can make a file look root-owned and setuid.

### Metadata cache (`cache.c`)

Off by default. A bounded table (4096 entries, oldest evicted) keyed by
mount-relative path, holding attributes or "missing", each valid for `MS`
milliseconds. Filled by every walk-and-getattr and every ENOENT lookup, and
by the per-directory search checks. Changes made through fs-9p drop or
replace the entries they affect (a write drops the file's entry, a rename
or rmdir drops whole subtrees, creates and removes drop the parent's).
Non-root opens use cache hits only if every ancestor is cached and
searchable. File data is never cached.

### Signals and shutdown

The shutdown thread waits in `sigwait()` for SIGTERM, and for SIGINT and
SIGHUP unless they were inherited as ignored (`nohup`, background jobs of
non-interactive shells). Then: `resmgr_detach()` of the mount point, stop
the session (take every slot, which waits for requests in flight, giving up
on stale ones after the timeout), Tclunk the root, close the transport
(device reset), `exit(0)`. The slots are never given back, so no thread can
touch the closed transport before the process is gone.

## Concurrency and locking

| Lock | Protects |
|---|---|
| The transport's mutex (`struct vio9p.lock`) | Slots' flags, the rings' indexes, the interrupt-thread handshake. Held briefly; not across a wait (condition variable). |
| A request slot | Its buffers, from `get()` to `put()`: one thread at a time. |
| The fid pool lock | The fid bitmap and the deferred clunks. |
| The cache mutex | The cache table. |
| The node's attribute lock (iofunc, via `lock_ocb`) | Serializes I/O handlers on one node (an open and its mmap OCB). |

There is no global session lock: different clients' requests run in
parallel up to the number of slots. Buffers used by handlers are
thread-local (`_Thread_local`), not static.

## Decisions and why

| Decision | Why |
|---|---|
| aarch64 `virt` + virtio-mmio only | The audience; no PCI server or I/O privilege needed; what QNX's own drivers use on `virt`. |
| No TCP transport, no diod | Virtio is the product; QEMU's trace is the independent validator. |
| One attach as uid 0, owner set with Tsetattr after create | Simple, works with every security model; per-user attaches would add fids and code for the same result. |
| Permissions checked in the guest | QEMU checks only as its host user; the guest must enforce guest users' rights. |
| `nosuid` always | Host-controlled ownership must not grant root in the guest. A `suid` option can come if someone needs it. |
| Real opens are eager, `stat()` opens lazy | Files must survive removal while open, `O_TRUNC` must apply at open; stat stays cheap. |
| Write-through, no data cache | Host and guest see the same bytes at all times; simple. |
| Metadata cache opt-in | Correctness by default; speed for trees that don't change behind the guest's back. |
| No Tflush | QEMU answers a flush only after the flushed request finished (QEMU source, `v9fs_flush`), so it wouldn't unblock anyone sooner. Not verified by experiment. |
| No read-ahead | Reads already go out in msize-sized requests; with no data cache there is nowhere to keep it. |
| msize 512 KiB, 4 slots by default | Measured ({doc}`../user/performance`). |
| Tag claim with `name_attach()` | Survives the owner's death, unlike the device status; no ability needed. |
| A resource manager, not a plugin | No QNX plugin framework fits a file-level remote filesystem (checked in the SDP 8.0 docs: `fs-*.so` modules are io-blk block-device filesystems; `fs-nfs3` is a standalone resource manager). |
| Keep the transport interface | Cheap, keeps `lib9p` and the resource manager independent of virtio, and leaves room for another transport or a fake one in tests. |
| No handling of the device disappearing | A virtio device can't go away without its VM ending; a device reset by QEMU is not a case that occurs. |
| Source-only distribution, Apache-2.0 | Integrators build with their own SDP license; Apache-2.0 for its explicit patent grant. |

## Behaviours found by experiment

The SDK headers didn't settle these; each was observed in the guest (QNX
8.0) or in QEMU 10.2.1's behaviour:

- readlink and symlink-redirect reply formats, `lstat()` sending connect
  mode `S_IFLNK` ({doc}`op-mapping`).
- The connect encodings of mkdir, rmdir, unlink, rename, symlink, link and
  mkfifo ({doc}`op-mapping`).
- `ftruncate()` as `F_FREESP` with length 0; the client applying the umask
  before sending the mode.
- Paths arriving canonical: the client side resolves `.`, `..` and
  repeated slashes before a request reaches fs-9p (`/mnt/host//a/..//f`
  arrives as `f`; `/mnt/host/..` never reaches it). fs-9p skips empty
  components anyway and would pass a `..` to QEMU as a walk name.
- The C library resolving rename's old path and a hard link's existing
  file through fs-9p (following redirects) before it sends the request.
- After fs-9p exits, open file descriptors give `EBADF`; `slay` returns
  before fs-9p has finished its shutdown.
- QEMU answering Tattach with `EINVAL` when it can't read the shared
  directory, and refusing to start when the path doesn't exist.
- `fsync()` needing `IOFUNC_PC_SYNC_IO`.
- A mapped file having a second OCB; the process manager asking
  `DCMD_ALL_GETMOUNTFLAGS` on exec.
- `procmgr_daemon()` not forking.
- `name_attach()` semantics: `EEXIST` on duplicates, removal on death, no
  ability needed, opens answered by the path manager.
- Abilities needed without root: `mem_phys`, `interrupt`, `pathspace`.
- virtio-mmio: IRQ = 48 + slot; QEMU filling slots from the top; physical
  addresses equal device addresses ({doc}`virtio-notes`).
- QEMU: Treadlink under mapped-xattr reads any file (so readlink checks the
  type first); chmod under mapped-xattr stores the whole mode (so the type
  bits are sent); special files can't be opened (`ENXIO`); a partial Rwalk
  has no error code.

## Known gaps

- Tflush and read-ahead (decided against, above).
- `uid=`/`gid=` mapping options and a `suid` option: only if needed.
- A request QEMU never finishes keeps its slot, and fs-9p's exit after
  `slay` waits for it (QEMU's reset waits too).
- Security-policy (`secpol`) setups are untested.

## The build

One `CMakeLists.txt` for both builds. The QNX build is recognized by the
toolchain file (`CMAKE_SYSTEM_NAME` is `QNX`), and QNX-only targets are
guarded, so a plain host `cmake` never fails: it builds `lib9p` and its
tests. `lib9p` is an internal static library (installed only with
`FS9P_INSTALL_LIB=ON`); `fs-9p` links it and gets its `use` message from
`usemsg` after linking. The `consistency` ctest and the optional `docs`
target are described in {doc}`docs`.

## References

- 9P2000.L: the Linux kernel's `Documentation/filesystems/9p.rst`, its
  `include/net/9p/9p.h` and `net/9p/client.c` (message layouts), the Plan 9
  manual's intro(5) for the base protocol, and QEMU's `hw/9pfs/9p.c` for
  the server's behaviour.
- Virtio 1.2 (OASIS): 2.7 split virtqueues, 3.1 device initialization,
  4.2 the MMIO transport (4.2.4 its legacy interface), 5.3 the 9P device.
- QEMU: the virtio-9p device documentation and the `v9fs_*` trace events.
- QNX 8.0: the Resource Manager guide (`dispatch`, `resmgr_attach`,
  `iofunc`), `mmap_device_memory()`, `mem_offset64()`,
  `InterruptAttachThread()`, `InterruptWait()`, `mkqnximage`.
