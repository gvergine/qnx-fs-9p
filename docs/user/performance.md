# Performance and tuning

## Tuning in short

- **Big files:** the default `msize` (512 KiB) is about as fast as it gets;
  1 MiB measured the same.
- **Several programs using the share at once:** keep `requests=4` (the
  default) or raise it to 8; `requests=1` makes them take turns.
- **Many `stat`s on a tree that rarely changes on the host:** `cache=MS`
  (for example `cache=5000`) roughly doubles repeated `ls -l` and similar.
- **Small reads and writes** (4 KiB) cost about the same as on a local disk
  in the guest; there is nothing to tune.

## How it was measured

Measured 2026-10-04 with `tests/integration/bench.sh`: QEMU 10.2.1, TCG
(aarch64 guest emulated on an x86_64 host), `-smp 2`, 1 GiB guest RAM,
`security_model=none`, share on an ext4 NVMe disk (host page cache warm).
Single runs; repeated runs of the same configuration varied by up to ±25%
for the large transfers, so read the numbers as approximate.

The baseline is the guest's own storage in the same VM: the QNX6 disk
(`/data`, virtio-blk) and RAM (`/dev/shmem`). The test VM has no network, so
there is no scp comparison.

## Sequential transfers (32 MiB file, `dd`)

| Request size | Share, msize 128 KiB | Share, msize 512 KiB (default) | QNX6 disk | RAM |
|---|---|---|---|---|
| write 1 MiB | 93–147 MB/s | 160–176 MB/s | 322 MB/s | 333 MB/s |
| read 1 MiB | 127–164 MB/s | 160–188 MB/s | 487 MB/s | 476 MB/s |
| write 64 KiB | 91–101 MB/s | 86–101 MB/s | 121 MB/s | 153 MB/s |
| read 64 KiB | 107–110 MB/s | 105–112 MB/s | 231 MB/s | 217 MB/s |
| write 4 KiB | 13 MB/s | 12–13 MB/s | 16 MB/s | 22 MB/s |
| read 4 KiB | 14 MB/s | 14–15 MB/s | 27 MB/s | 25 MB/s |

msize 256 KiB and 1 MiB were also measured: 1 MiB was within noise of 512
KiB. 512 KiB became the default (`-o msize=` changes it); it costs about
1 MiB of physically contiguous DMA memory per mount.

With small requests the share is close to local storage: per-request costs
(QNX message passing, the virtqueue round trip, emulation) dominate both.
Large transfers reach about 40-50% of local; each request is copied from
the client's message into the DMA buffer, and the guest's copies are
emulated.

## Small files (500 files of a few bytes)

| Operation | Share, msize 512 KiB | QNX6 disk |
|---|---|---|
| create (shell `echo > f`) | 428–475 /s | 691 /s |
| `stat` (`ls -l`) | 1259 /s | 2600 /s |
| open + read + close (`cat`) | 807–852 /s | 1127 /s |
| unlink (`rm -r`) | 498–514 /s | 1304 /s |

Every operation is a few 9P round trips (with no cache, each lookup walks
from the root): `stat` is Twalk + Tgetattr + Tclunk (a stat inside the
`stat()` combine message reuses the open's Tgetattr), `rm` is Twalk +
Tgetattr of the directory + Tunlinkat + Tclunk after the client's own
`lstat`. Non-root users pay one more Tgetattr per directory of the path for
the search-permission check.

## Concurrent requests

Two clients at once, each reading or writing its own 32 MiB file with 1 MiB
requests (msize 512 KiB), seconds for both to finish; one client alone
takes 0.17–0.19 s:

| | `requests=1` | `requests=4` (default) |
|---|---|---|
| 2 × read | 0.28–0.30 s | 0.17–0.19 s |
| 2 × write | 0.31–0.37 s | 0.18–0.20 s |

With one request in flight the second client only got the time the first
left over; with four, the two run side by side on the guest's two CPUs.
Two parallel small-file loops (create, `ls -l`, `rm` of 300 files each)
took the same time in both modes, about 1.5× one loop alone: they are
bound by the guest's CPUs, not by fs-9p.

## Metadata cache (`cache=MS`)

500 small files, `security_model=none`:

| | no cache (default) | `cache=5000` |
|---|---|---|
| `ls -l`, first time after writing the files | 1145 /s | 1132 /s |
| `ls -l` again | 1269 /s | 2423 /s |

The first listing gains nothing because writing a file drops its cache
entry; the repeated one sends no 9P request per file and runs at about
the speed of the local QNX6 disk (2600 /s). Creates, opens and unlinks are
unchanged: they always go to the server.

