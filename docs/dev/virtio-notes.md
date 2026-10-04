# Virtio notes

The facts about QEMU's virtio-9p device on the `virt` machine and the QNX
APIs the transport uses, each checked in a guest: the evidence behind
{doc}`architecture`. Everything here was observed with QEMU 10.2.1
(`-M virt-8.2 -cpu cortex-a57`, TCG on an x86_64 host) and QNX SDP 8.0.

The first experiments used a small standalone program that found the
device and exchanged Tversion, Tattach and Tclunk; `src/transport_virtio.c`
replaced it, and it isn't part of this repository.

## The device on `virt`

QEMU's `virt` machine has 32 virtio-mmio transports of 0x200 bytes from
0x0a000000 and fills them from the top (slot 31) in command-line order. A
slot is identified by MagicValue 0x74726976 ("virt"), the Version register
(1 or 2) and DeviceID (9 for 9P; 0 for an empty slot). The vendor is
0x554d4551 ("QEMU"). With the test image's devices:

```
slot 31 @0x0a003e00: device 2 vendor 0x554d4551 version 1   (block)
slot 30 @0x0a003c00: device 9 vendor 0x554d4551 version 1   (9P)
slot 29 @0x0a003a00: device 4 vendor 0x554d4551 version 1   (rng)
```

- **IRQ = 48 + slot** on the GIC: slot 30 completes on IRQ 78, slot 28 on
  76 (two shares), and mkqnximage's disk driver uses `irq=79` for slot 31.
- **Features.** Legacy (MMIO version 1): `0x39000001`, bits 0
  `VIRTIO_9P_MOUNT_TAG`, 24 `NOTIFY_ON_EMPTY`, 27 `ANY_LAYOUT`, 28
  `INDIRECT_DESC`, 29 `EVENT_IDX`. Modern (version 2):
  `0x0000010130000001`, bits 0, 28, 29, 32 `VERSION_1`, 40 `RING_RESET`
  (virtio 1.2 §6 and §5.3.3). fs-9p accepts `MOUNT_TAG`, plus `VERSION_1`
  on modern devices.
- **Queue size:** `QueueNumMax` is 1024; fs-9p uses 16.
- **Config space:** `tag_len` (le16) then the tag bytes.
- **Both layouts work**, with the interrupt and with polling:

  | `run-qnx.sh -m` | MMIO version | Interrupt | Poll |
  |---|---|---|---|
  | `legacy` (QEMU's default) | 1 | OK | OK |
  | `modern` (`-global virtio-mmio.force-legacy=false`) | 2 | OK | OK |

## Initialization sequences

- **Version 1 (legacy, virtio 1.2 §4.2.4):**
  1. Status 0, ACK, DRIVER.
  2. `GuestPageSize = 4096`.
  3. Features (select 0 and 1).
  4. `QueueSel = 0`, read `QueueNumMax`, check `QueuePFN == 0`.
  5. `QueueNum`, `QueueAlign = 4096`, `QueuePFN = phys / 4096`.
  6. DRIVER_OK. No FEATURES_OK step.
- **Version 2 (modern, §4.2.3):**
  1. Status 0, then wait until it reads back 0. ACK, DRIVER.
  2. Write 64-bit features, then FEATURES_OK and re-check it.
  3. Read the mount tag, looping on `ConfigGeneration`.
  4. `QueueSel`, check `QueueReady == 0`, `QueueNum`.
  5. Write `QueueDesc`/`QueueDriver`/`QueueDevice` as low/high 32-bit pairs.
  6. `QueueReady = 1`, then DRIVER_OK.
- **Reading the tag before choosing a device:** a device at status 0 gets
  ACK|DRIVER, its tag is read (allowed during feature negotiation, virtio
  1.2 §3.1.1 step 4), and it's reset again if the tag isn't ours. A device
  with a nonzero status is past that step: its tag is only read.
- **Notify:** write the queue index to `QueueNotify` (0x050).
- **Interrupt acknowledge:** read `InterruptStatus` (0x060), write the
  same value to `InterruptACK` (0x064).
- **Reset** (status 0) waits in QEMU for the requests it is still
  processing, so afterwards nothing is written into the driver's memory.

## QNX APIs used

| Purpose | API | Notes |
|---|---|---|
| Register window | `mmap_device_memory(NULL, 32*0x200, PROT_READ\|PROT_WRITE\|PROT_NOCACHE, 0, 0x0a000000)` | One mapping covers all 32 slots. Needs `PROCMGR_AID_MEM_PHYS`. Registers below 0x100 use 32-bit volatile accesses; the config area 8/16-bit. |
| DMA memory | `mmap(NULL, len, PROT_READ\|PROT_WRITE, MAP_SHARED\|MAP_PHYS\|MAP_ANON, NOFD, 0)` + `mem_offset64()` | Physically contiguous; check `contig >= len`. `virt` has no IOMMU, so device addresses are guest physical. The 8.0 `mmap()` docs say `MAP_PHYS\|MAP_ANON` needs no ability; not checked on its own, since the register window needs `mem_phys` anyway. |
| Interrupt | `InterruptAttachThread(irq, 0)`, `InterruptWait(_NTO_INTR_WAIT_FLAGS_UNMASK, NULL)` | The calling thread becomes the interrupt thread; the kernel masks the line before waking it and `_UNMASK` re-arms it. The timeout argument must be NULL. Needs `PROCMGR_AID_INTERRUPT`. |
| Barriers | C11 `atomic_thread_fence()`; `__cpu_membarrier()` from `<sys/cpuinline.h>` | Release fence before publishing `avail->idx`, acquire after reading `used->idx`. `__cpu_membarrier()` (`dmb sy`, checked in the disassembly) between the index store and the notify write: a C11 fence (`dmb ish`) doesn't order normal memory against device memory. |
| Tag claim | `name_attach(NULL, "fs-9p/<tag>", 0)` | See "Device ownership". |

`InterruptUnblock()` is declared in `<sys/neutrino.h>` but not documented
in the SDP, so it isn't used: the interrupt thread is never joined, since
the transport only closes just before the process exits.

## Surprises

1. **Spurious interrupt-thread wakeups.** On the second request the thread
   often wakes with `InterruptStatus == 0`, and the real completion
   (status 1) follows. Probably the level-triggered line is still latched
   when `InterruptWait(_UNMASK)` re-arms right after the ACK write.
   Harmless, because completion is decided from `used->idx`, never from
   the wakeup.
2. **`__cpu_membarrier()` needs `<sys/cpuinline.h>`** on aarch64; on x86 it
   came in indirectly through other headers.
3. **mkqnximage's aarch64 runner is stale for QEMU 10**: `virt-4.2` is
   gone and `virt-7.2` is deprecated; `run-qnx.sh` uses `virt-8.2`. The
   image's fixed virtio slots also force a device order on the command line
   ({doc}`test-environment`).
4. **QEMU's startup errors go to the `-D` log**, not stderr, and so do its
   warnings (`9p: degraded performance: ... msize <= 8192`): trace parsers
   keep only lines starting with `v9fs_`.
5. **The SDP has no virtio headers**: register layouts and structures are
   written from the virtio 1.2 specification.

## Privileges

fs-9p runs as a non-root user (uid 1000, started with `on -u qnxuser -A
...`) with exactly three abilities. Removing any one makes startup fail
with `EPERM`:

- `PROCMGR_AID_MEM_PHYS`: `mmap_device_memory()` of the register window.
- `PROCMGR_AID_INTERRUPT`: `InterruptAttachThread()`.
- `PROCMGR_AID_PATHSPACE`: `resmgr_attach()` of the mount point (the
  resource manager's need, not the transport's).

No I/O privilege is needed with virtio-mmio.

## Device ownership

Nothing resets a virtio device when its driver dies: DRIVER_OK stays set
until the guest reboots. So the device status can't tell "in use" from
"left behind by a killed fs-9p"; a first version that treated a nonzero
status as "in use" couldn't restart after `slay`.

Ownership is a claim on the tag instead: `name_attach(NULL,
"fs-9p/<tag>", 0)`, which appears as `/dev/name/local/fs-9p/<tag>`. It is
taken before any device register is written and held until the process
exits. With the claim held, a device exporting our tag with a nonzero
status must have been left by a dead instance, so it is reset and taken
over. Devices of other tags with a nonzero status are only read.

Verified in the guest, with a test program and then with fs-9p:
- `name_attach()` of an existing name fails with `EEXIST`. Two processes
  started at the same moment: one succeeds and one gets `EEXIST`, in 5 of
  5 tries.
- The name is removed when its owner dies, by `slay` or `SIGKILL`.
- `/` in the name creates a directory: `ls /dev/name/local/fs-9p` lists
  the tags being served.
- No ability is needed: a non-root user without `pathspace` can attach a
  name under `/dev/name/local`.
- Nobody needs to service the name's channel: `ls -l` (stat) and `open()`
  of the name are answered by the path manager (`open()` fails with
  `EISDIR`) without a message reaching the owner. Only `name_open()` would
  reach it; not tested, since nothing uses it.
- Restart after `slay` and after `SIGKILL` during an 8 MiB read: the new
  instance finds the device at status 0x7, resets it, and reads correctly.
  The device's interrupt keeps working for the new instance.

## Why not virtio-pci

The first prototype targeted x86_64 and virtio-pci (QEMU `-virtfs`), and
worked there with both the legacy and the modern PCI interface. It was
dropped for virtio-mmio on aarch64 `virt` because:

- **No PCI support for `virt` was installed**: the SDP had `libpci` and
  `pci-server` for aarch64le but no aarch64 `pci_hw-*` module, so libpci
  couldn't reach config space. (An FDT-based module,
  `com.qnx.qnx800.target.pci.hw.fdt`, appeared later; it is untested.)
- **QNX's own drivers use virtio-mmio on `virt`**: mkqnximage's image runs
  `devb-virtio virtio smem=0xa003e00,irq=79` and `devs-vtnet_mmio`.
- **virtio-pci costs more on the target**: a running `pci-server`, the
  `PCI_HW_MODULE` environment variable in every libpci client (with the
  module's full path, `/proc/boot/...`), and, on machines without ECAM,
  I/O privilege, because libpci then reads config space through ports
  0xCF8/0xCFC inside the client (a crash in `pci_device_attach()` without
  it; found with `PCI_BASE_VERBOSITY=9` and `slog2info`).
- **The virtio core is the same**: features, status, the split ring, DMA
  memory, the interrupt thread and the barriers carried over unchanged;
  only discovery and register access differ.

The cost for users: `-fsdev` with `-device virtio-9p-device` instead of
`-virtfs`, which on `virt` creates a `virtio-9p-pci` device fs-9p doesn't
see. A virtio-pci transport could be added later behind `transport.h`,
reusing the virtio core; the notes above list what the PCI side needs.
