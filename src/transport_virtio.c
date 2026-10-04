/*
 * virtio-9p transport over virtio-mmio on QEMU's aarch64 `virt` machine.
 *
 * See docs/dev/virtio-notes.md for the verified register layout, IRQ
 * mapping and QNX APIs. Supports MMIO version 1 (legacy, QEMU's default)
 * and 2.
 *
 * Requests go through slots (transport.h), each with its own buffers and
 * descriptor pair, so several can be in flight; the device may return
 * them in any order. Completion: InterruptAttachThread() binds the
 * *calling* thread, but rpc() runs on arbitrary resource-manager threads,
 * so a dedicated interrupt service thread (IST) acknowledges the device,
 * takes returned chains off the used ring and wakes the waiters through a
 * condition variable. Completion is always decided from used->idx, never
 * from the wakeup alone (spurious wakeups were observed).
 *
 * References: OASIS virtio 1.2 — 2.7 split virtqueues, 3.1 device init,
 * 4.2.2/4.2.3 MMIO, 4.2.4 legacy MMIO, 5.3 9P device.
 */
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/cpuinline.h> /* __cpu_membarrier() */
#include <sys/dispatch.h>  /* name_attach() */
#include <sys/mman.h>
#include <sys/neutrino.h>

#include "transport.h"

/* Ring fields and registers are little-endian; so is aarch64le. */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "transport_virtio assumes a little-endian CPU"
#endif

/* QEMU virt memory map (hw/arm/virt.c): 32 transports of 0x200 bytes. */
#define VIRT_MMIO_BASE  0x0a000000u
#define VIRT_MMIO_SIZE  0x200u
#define VIRT_MMIO_SLOTS 32u
#define VIRT_MMIO_IRQ0  48 /* slot n -> SPI 16+n -> QNX vector 48+n (verified) */

/* virtio-mmio registers (4.2.2; legacy-only ones from 4.2.4) */
#define VM_MAGIC          0x000u
#define VM_VERSION        0x004u
#define VM_DEVICE_ID      0x008u
#define VM_DEV_FEATURES   0x010u
#define VM_DEV_FEAT_SEL   0x014u
#define VM_DRV_FEATURES   0x020u
#define VM_DRV_FEAT_SEL   0x024u
#define VM_GUEST_PAGESIZE 0x028u /* legacy */
#define VM_QUEUE_SEL      0x030u
#define VM_QUEUE_NUM_MAX  0x034u
#define VM_QUEUE_NUM      0x038u
#define VM_QUEUE_ALIGN    0x03Cu /* legacy */
#define VM_QUEUE_PFN      0x040u /* legacy */
#define VM_QUEUE_READY    0x044u /* modern */
#define VM_QUEUE_NOTIFY   0x050u
#define VM_INT_STATUS     0x060u
#define VM_INT_ACK        0x064u
#define VM_STATUS         0x070u
#define VM_QUEUE_DESC     0x080u /* modern, low/high */
#define VM_QUEUE_DRIVER   0x090u /* modern, low/high */
#define VM_QUEUE_DEVICE   0x0A0u /* modern, low/high */
#define VM_CONFIG_GEN     0x0FCu /* modern */
#define VM_CONFIG         0x100u

#define VIRTIO_MMIO_MAGIC     0x74726976u
#define VIRTIO_ID_9P          9u
#define VIRTIO_9P_F_MOUNT_TAG (1ull << 0)
#define VIRTIO_F_VERSION_1    (1ull << 32)

#define VSTAT_ACK         1u
#define VSTAT_DRIVER      2u
#define VSTAT_DRIVER_OK   4u
#define VSTAT_FEATURES_OK 8u
#define VSTAT_FAILED      128u

#define VRING_DESC_F_NEXT  1u
#define VRING_DESC_F_WRITE 2u

/* Legacy QueuePFN wants page frames and the used ring on a QueueAlign
 * boundary; modern only needs 16/2/4 alignment, so one layout serves both. */
#define VQ_PAGE 4096u
/* 16 entries: VIO9P_MAX_SLOTS two-descriptor chains. */
#define VQ_LIMIT 16u

#define TAG_MAX 256u

/* Claim name under /dev/name/local: one fs-9p per mount tag. */
#define CLAIM_PREFIX "fs-9p/"

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
};

/* One request slot: its buffers and where its descriptor chain is. All
 * flags are under vio9p.lock. */
struct vio_req {
    uint8_t *mem; /* request buffer, then reply buffer: 2 x bufsize */
    uint64_t phys;
    size_t mem_len;
    bool taken;   /* owned by a caller between get() and put() */
    bool pending; /* chain handed to the device and not returned yet */
    bool done;    /* returned by the device; len is valid */
    bool stale;   /* timed out: becomes free when the device returns it */
    uint32_t len; /* reply length the device wrote */
};

struct vio9p {
    vio9p_options_t opt;
    name_attach_t *claim;   /* held for the life of the process */
    volatile uint8_t *win;  /* all slots */
    volatile uint8_t *regs; /* our slot */
    unsigned slot;
    uint32_t version;
    int irq;

    /* Ring memory (physically contiguous) and the request slots. Slot i
     * owns descriptors 2i (request) and 2i+1 (reply). */
    uint8_t *ring;
    uint64_t ring_phys;
    size_t ring_len;
    uint32_t bufsize;
    struct vio_req reqs[VIO9P_MAX_SLOTS];
    unsigned nreqs;

    uint16_t qsize;
    volatile struct vring_desc *desc;
    volatile uint16_t *avail;    /* flags, idx, ring[qsize], used_event */
    volatile uint16_t *used_hdr; /* flags, idx */
    volatile struct vring_used_elem *used_ring;
    uint16_t avail_idx;
    uint16_t last_used;
    bool broken; /* the device returned something it didn't own: give up */
    bool closed;

    /* Everything above that changes at run time is under `lock`; `cond`
     * signals completions (from the IST or a polling waiter) and freed
     * slots. */
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool ist_ready;
    int ist_err;
    bool stop;
    pthread_t ist;
};

static size_t align_up(size_t v, size_t a)
{
    return (v + a - 1) & ~(a - 1);
}

static size_t vring_used_off(uint16_t n)
{
    return align_up(16u * n + 2u * (3u + n), VQ_PAGE);
}

static size_t vring_bytes(uint16_t n)
{
    return vring_used_off(n) + align_up(2u * 3u + 8u * n, VQ_PAGE);
}

/* Registers below 0x100 are 32-bit only (4.2.2); config allows 8/16-bit. */
static uint32_t rd32(const struct vio9p *v, uint32_t off)
{
    return *(volatile uint32_t *)(v->regs + off);
}

static void wr32(const struct vio9p *v, uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(v->regs + off) = val;
}

static void wr64(const struct vio9p *v, uint32_t off, uint64_t val)
{
    wr32(v, off, (uint32_t)val);
    wr32(v, off + 4, (uint32_t)(val >> 32));
}

static uint8_t cfg8(const struct vio9p *v, uint32_t off)
{
    return *(volatile uint8_t *)(v->regs + VM_CONFIG + off);
}

static uint16_t cfg16(const struct vio9p *v, uint32_t off)
{
    return *(volatile uint16_t *)(v->regs + VM_CONFIG + off);
}

static void logv(const struct vio9p *v, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void logv(const struct vio9p *v, const char *fmt, ...)
{
    if (!v->opt.verbose)
        return;
    va_list ap;
    va_start(ap, fmt);
    fputs("vio9p: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* --- discovery ---------------------------------------------------------------- */

/* Read the mount tag into buf (NUL-terminated). Only valid once the device
 * offered VIRTIO_9P_F_MOUNT_TAG. */
static int read_tag(const struct vio9p *v, char *buf, size_t cap)
{
    uint16_t len;
    uint32_t gen = 0;
    do {
        if (v->version == 2)
            gen = rd32(v, VM_CONFIG_GEN);
        len = cfg16(v, 0);
        if ((size_t)len + 1 > cap || VM_CONFIG + 2u + len > VIRT_MMIO_SIZE)
            return -EIO;
        for (uint16_t i = 0; i < len; i++)
            buf[i] = (char)cfg8(v, 2u + i);
    } while (v->version == 2 && gen != rd32(v, VM_CONFIG_GEN));
    buf[len] = '\0';
    return 0;
}

static uint64_t dev_features(const struct vio9p *v)
{
    wr32(v, VM_DEV_FEAT_SEL, 0);
    uint64_t f = rd32(v, VM_DEV_FEATURES);
    wr32(v, VM_DEV_FEAT_SEL, 1);
    return f | (uint64_t)rd32(v, VM_DEV_FEATURES) << 32;
}

static int reset_device(const struct vio9p *v)
{
    wr32(v, VM_STATUS, 0);
    /* Modern devices must read back 0 before the driver continues (4.2.3.1). */
    for (int i = 0; rd32(v, VM_STATUS) != 0; i++) {
        if (i == 1000)
            return -EIO;
        usleep(1000);
    }
    return 0;
}

/*
 * One fs-9p per mount tag. The device status can't tell a live owner from
 * one that was killed (DRIVER_OK stays set), so ownership is the name
 * fs-9p/<tag> under /dev/name/local instead: name_attach() refuses a
 * duplicate with EEXIST, and the name goes away when its owner dies
 * (both verified 2026-10-04). Taken before any device register is written.
 *
 * The channel behind the name is never serviced. stat(), ls -l and open()
 * of the name are answered by the path manager without reaching it
 * (verified); only name_open() would, and nothing uses that.
 */
static int claim_tag(struct vio9p *v, const char *tag)
{
    char name[sizeof CLAIM_PREFIX + TAG_MAX];
    snprintf(name, sizeof name, CLAIM_PREFIX "%s", tag);
    v->claim = name_attach(NULL, name, 0);
    if (v->claim == NULL)
        return errno == EEXIST ? -EBUSY : -errno;
    return 0;
}

/*
 * Probe one slot: is it the virtio-9p device exporting `want`?
 * Returns 1 (ours, left in ACK|DRIVER state), 0 (not ours: untouched, or
 * reset back to 0), or -EIO (ours but the reset failed).
 *
 * Reading device config is allowed while the driver negotiates features
 * (3.1.1 step 4), so a free device (status 0) is claimed with ACK|DRIVER
 * first and released with a reset if the tag differs. A device with a
 * nonzero status is past that step: its tag is read without writing
 * anything. If the tag is ours, the caller's claim proves no live fs-9p
 * drives it, so it was left by a dead instance and is reset and taken
 * over. Otherwise it belongs (or belonged) to another tag's instance and is
 * left alone. Two instances for different tags probing the same free
 * device at the same moment can still race; not handled.
 */
static int probe_slot(struct vio9p *v, const char *want)
{
    if (rd32(v, VM_MAGIC) != VIRTIO_MMIO_MAGIC || rd32(v, VM_DEVICE_ID) != VIRTIO_ID_9P)
        return 0;
    v->version = rd32(v, VM_VERSION);
    if (v->version != 1 && v->version != 2)
        return 0;

    char tag[TAG_MAX];
    uint32_t status = rd32(v, VM_STATUS);
    if (status == 0) {
        wr32(v, VM_STATUS, VSTAT_ACK);
        wr32(v, VM_STATUS, VSTAT_ACK | VSTAT_DRIVER);
    }
    bool has_tag = (dev_features(v) & VIRTIO_9P_F_MOUNT_TAG) != 0;
    if (!has_tag || read_tag(v, tag, sizeof tag) != 0)
        tag[0] = '\0';
    logv(v, "slot %u: virtio-9p v%" PRIu32 " tag \"%s\" status 0x%" PRIx32 "\n", v->slot,
         v->version, tag, status);

    if (strcmp(tag, want) != 0) {
        if (status == 0)
            (void)reset_device(v);
        return 0;
    }
    if (status != 0) {
        logv(v, "slot %u: left by a previous instance, resetting\n", v->slot);
        if (reset_device(v) != 0)
            return -EIO;
        wr32(v, VM_STATUS, VSTAT_ACK);
        wr32(v, VM_STATUS, VSTAT_ACK | VSTAT_DRIVER);
    }
    return 1;
}

static int find_device(struct vio9p *v, const char *want)
{
    /* PROT_NOCACHE: device memory. Needs PROCMGR_AID_MEM_PHYS. */
    void *m = mmap_device_memory(NULL, VIRT_MMIO_SLOTS * VIRT_MMIO_SIZE,
                                 PROT_READ | PROT_WRITE | PROT_NOCACHE, 0, VIRT_MMIO_BASE);
    if (m == MAP_FAILED)
        return -errno;
    v->win = m;

    /* QEMU fills slots from the top, so scan downwards. */
    for (unsigned i = VIRT_MMIO_SLOTS; i-- > 0;) {
        v->slot = i;
        v->regs = v->win + i * VIRT_MMIO_SIZE;
        int rc = probe_slot(v, want);
        if (rc != 0)
            return rc == 1 ? 0 : rc;
    }
    v->regs = NULL;
    return -ENODEV;
}

/* --- DMA memory ----------------------------------------------------------------- */

/* Physically contiguous, normal cacheable memory (QEMU DMA is coherent);
 * MAP_PHYS|MAP_ANON needs no special ability. virt has no IOMMU: device
 * addresses are guest physical (verified). */
static int dma_map(size_t len, uint8_t **mem, uint64_t *phys)
{
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_PHYS | MAP_ANON, NOFD, 0);
    if (p == MAP_FAILED)
        return -errno;
    off64_t ph;
    size_t contig;
    if (mem_offset64(p, NOFD, len, &ph, &contig) == -1) {
        int e = errno;
        munmap(p, len);
        return -e;
    }
    if (contig < len || (ph & (VQ_PAGE - 1)) != 0) {
        munmap(p, len);
        return -ENOMEM;
    }
    memset(p, 0, len);
    *mem = p;
    *phys = (uint64_t)ph;
    return 0;
}

/* The ring, and each slot's buffers in a region of its own (one large
 * contiguous block is harder for the memory manager to find). */
static int dma_alloc(struct vio9p *v)
{
    v->ring_len = vring_bytes(v->qsize);
    int rc = dma_map(v->ring_len, &v->ring, &v->ring_phys);
    if (rc != 0)
        return rc;
    v->desc = (volatile struct vring_desc *)v->ring;
    v->avail = (volatile uint16_t *)(v->ring + 16u * v->qsize);
    v->used_hdr = (volatile uint16_t *)(v->ring + vring_used_off(v->qsize));
    v->used_ring = (volatile struct vring_used_elem *)(v->ring + vring_used_off(v->qsize) + 4u);
    for (unsigned i = 0; i < v->nreqs; i++) {
        struct vio_req *r = &v->reqs[i];
        r->mem_len = 2u * (size_t)v->bufsize;
        rc = dma_map(r->mem_len, &r->mem, &r->phys);
        if (rc != 0)
            return rc;
        /* The chain never changes, only the request length. */
        v->desc[2 * i].addr = r->phys;
        v->desc[2 * i].flags = VRING_DESC_F_NEXT;
        v->desc[2 * i].next = (uint16_t)(2 * i + 1);
        v->desc[2 * i + 1].addr = r->phys + v->bufsize;
        v->desc[2 * i + 1].len = v->bufsize;
        v->desc[2 * i + 1].flags = VRING_DESC_F_WRITE;
        v->desc[2 * i + 1].next = 0;
    }
    return 0;
}

static void dma_free(struct vio9p *v)
{
    for (unsigned i = 0; i < v->nreqs; i++)
        if (v->reqs[i].mem != NULL)
            munmap(v->reqs[i].mem, v->reqs[i].mem_len);
    if (v->ring != NULL)
        munmap(v->ring, v->ring_len);
}

/* --- device init (device left in ACK|DRIVER by probe_slot) ---------------------- */

static int negotiate(struct vio9p *v)
{
    uint64_t dev = dev_features(v);
    uint64_t want = VIRTIO_9P_F_MOUNT_TAG;
    if (v->version == 2) {
        if (!(dev & VIRTIO_F_VERSION_1))
            return -EIO;
        want |= VIRTIO_F_VERSION_1;
    } else {
        wr32(v, VM_GUEST_PAGESIZE, VQ_PAGE);
    }
    uint64_t drv = dev & want;
    wr32(v, VM_DRV_FEAT_SEL, 0);
    wr32(v, VM_DRV_FEATURES, (uint32_t)drv);
    wr32(v, VM_DRV_FEAT_SEL, 1);
    wr32(v, VM_DRV_FEATURES, (uint32_t)(drv >> 32));
    logv(v, "features: device 0x%016" PRIx64 " driver 0x%016" PRIx64 "\n", dev, drv);
    if (v->version == 2) {
        wr32(v, VM_STATUS, VSTAT_ACK | VSTAT_DRIVER | VSTAT_FEATURES_OK);
        if (!(rd32(v, VM_STATUS) & VSTAT_FEATURES_OK))
            return -EIO;
    }
    return 0;
}

static int setup_queue(struct vio9p *v)
{
    wr32(v, VM_QUEUE_SEL, 0);
    uint32_t in_use = v->version == 1 ? rd32(v, VM_QUEUE_PFN) : rd32(v, VM_QUEUE_READY);
    uint32_t max = rd32(v, VM_QUEUE_NUM_MAX);
    if (max < 2 || in_use != 0)
        return -EIO;
    v->qsize = (uint16_t)(max < VQ_LIMIT ? max : VQ_LIMIT);
    if (v->nreqs > v->qsize / 2u)
        v->nreqs = v->qsize / 2u;
    int rc = dma_alloc(v);
    if (rc != 0)
        return rc;
    wr32(v, VM_QUEUE_NUM, v->qsize);
    if (v->version == 1) {
        if (v->ring_phys / VQ_PAGE > UINT32_MAX)
            return -ENOMEM;
        wr32(v, VM_QUEUE_ALIGN, VQ_PAGE);
        wr32(v, VM_QUEUE_PFN, (uint32_t)(v->ring_phys / VQ_PAGE));
    } else {
        wr64(v, VM_QUEUE_DESC, v->ring_phys);
        wr64(v, VM_QUEUE_DRIVER, v->ring_phys + 16u * v->qsize);
        wr64(v, VM_QUEUE_DEVICE, v->ring_phys + vring_used_off(v->qsize));
        wr32(v, VM_QUEUE_READY, 1);
    }
    logv(v, "queue 0: size %u (max %" PRIu32 "), %u slots of 2 x %" PRIu32 " bytes\n", v->qsize,
         max, v->nreqs, v->bufsize);
    return 0;
}

/* --- completions ------------------------------------------------------------- */

/*
 * Take every chain the device has returned off the used ring and mark its
 * slot done; wake the waiters. Called with the lock held, by the IST after
 * an interrupt and by waiters themselves (so a missed or spurious wakeup
 * costs nothing: completion is always decided from used->idx). Chains come
 * back in any order; the head descriptor identifies the slot.
 */
static void reap(struct vio9p *v)
{
    /* Acquire: read used->idx before the used elements and the replies. */
    uint16_t idx = v->used_hdr[1];
    atomic_thread_fence(memory_order_acquire);
    bool any = false;
    while (v->last_used != idx) {
        volatile struct vring_used_elem *e = &v->used_ring[v->last_used % v->qsize];
        uint32_t id = e->id, len = e->len;
        v->last_used++;
        unsigned i = id / 2u;
        if ((id & 1u) != 0 || i >= v->nreqs || !v->reqs[i].pending || len > v->bufsize) {
            v->broken = true; /* not a chain we gave it */
            continue;
        }
        struct vio_req *r = &v->reqs[i];
        if (r->stale) {
            /* Its caller gave up; the buffers are ours again. */
            r->stale = false;
            r->pending = false;
        } else {
            r->len = len;
            r->done = true;
        }
        any = true;
    }
    if (any)
        pthread_cond_broadcast(&v->cond);
}

/* --- interrupt service thread ------------------------------------------------- */

static void *ist_main(void *arg)
{
    struct vio9p *v = arg;
    /* The calling thread becomes the IST (PROCMGR_AID_INTERRUPT). */
    int id = InterruptAttachThread(v->irq, 0);
    pthread_mutex_lock(&v->lock);
    v->ist_err = id == -1 ? errno : 0;
    v->ist_ready = true;
    pthread_cond_broadcast(&v->cond);
    pthread_mutex_unlock(&v->lock);
    if (id == -1)
        return NULL;

    for (;;) {
        if (InterruptWait(_NTO_INTR_WAIT_FLAGS_UNMASK, NULL) == -1) {
            if (errno == EINTR)
                continue;
            break;
        }
        /* Acknowledge what the device raised (bit 0 used buffer, bit 1 config
         * change) so the level-triggered line drops before the next unmask. */
        uint32_t st = rd32(v, VM_INT_STATUS);
        if (st)
            wr32(v, VM_INT_ACK, st);
        pthread_mutex_lock(&v->lock);
        bool stop = v->stop;
        reap(v);
        pthread_mutex_unlock(&v->lock);
        if (stop)
            break;
    }
    InterruptDetach(id);
    return NULL;
}

static int start_ist(struct vio9p *v)
{
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    int rc = pthread_cond_init(&v->cond, &ca);
    pthread_condattr_destroy(&ca);
    if (rc != 0)
        return -rc;
    if ((rc = pthread_mutex_init(&v->lock, NULL)) != 0)
        return -rc;
    if (v->opt.poll)
        return 0;

    if ((rc = pthread_create(&v->ist, NULL, ist_main, v)) != 0)
        return -rc;
    /* The IST must be attached before the first notify, or the completion
     * interrupt of the first request could be missed. */
    pthread_mutex_lock(&v->lock);
    while (!v->ist_ready)
        pthread_cond_wait(&v->cond, &v->lock);
    rc = v->ist_err;
    pthread_mutex_unlock(&v->lock);
    if (rc != 0) {
        pthread_join(v->ist, NULL);
        return -rc;
    }
    return 0;
}

/* --- transport ops -------------------------------------------------------------- */

static bool req_free(const struct vio_req *r)
{
    return !r->taken && !r->pending;
}

static void deadline_after(struct timespec *ts, uint32_t ms);
static bool past(const struct timespec *dl);

static int vio_get(void *impl, unsigned *slot)
{
    struct vio9p *v = impl;
    /* Waiting for a slot is bounded by the request timeout too: if every
     * slot is held by a request the device doesn't finish (stale after a
     * timeout), callers get ETIMEDOUT instead of hanging. */
    struct timespec dl = {0, 0};
    if (v->opt.timeout_ms)
        deadline_after(&dl, v->opt.timeout_ms);
    pthread_mutex_lock(&v->lock);
    for (;;) {
        if (v->closed || v->broken) {
            pthread_mutex_unlock(&v->lock);
            return -EIO;
        }
        for (unsigned i = 0; i < v->nreqs; i++) {
            if (req_free(&v->reqs[i])) {
                v->reqs[i].taken = true;
                pthread_mutex_unlock(&v->lock);
                *slot = i;
                return 0;
            }
        }
        /* All busy: a put() or a stale slot coming back wakes us. In poll
         * mode nobody reaps for a stale slot, so look again now and then. */
        if (v->opt.timeout_ms && past(&dl)) {
            pthread_mutex_unlock(&v->lock);
            return -ETIMEDOUT;
        }
        if (v->opt.poll) {
            reap(v);
            pthread_mutex_unlock(&v->lock);
            usleep(100);
            pthread_mutex_lock(&v->lock);
        } else if (v->opt.timeout_ms) {
            (void)pthread_cond_timedwait(&v->cond, &v->lock, &dl);
        } else {
            pthread_cond_wait(&v->cond, &v->lock);
        }
    }
}

static uint8_t *vio_buf(void *impl, unsigned slot, size_t *cap)
{
    struct vio9p *v = impl;
    *cap = v->bufsize;
    return v->reqs[slot].mem;
}

static void deadline_after(struct timespec *ts, uint32_t ms)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
    ts->tv_sec += ms / 1000u;
    ts->tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

static bool past(const struct timespec *dl)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec > dl->tv_sec || (now.tv_sec == dl->tv_sec && now.tv_nsec >= dl->tv_nsec);
}

static int vio_rpc(void *impl, unsigned slot, size_t req_len, const uint8_t **resp,
                   size_t *resp_len)
{
    struct vio9p *v = impl;
    struct vio_req *r = &v->reqs[slot];
    if (req_len < 7 || req_len > v->bufsize)
        return -EINVAL;
    struct timespec dl = {0, 0};
    if (v->opt.timeout_ms)
        deadline_after(&dl, v->opt.timeout_ms);

    pthread_mutex_lock(&v->lock);
    if (v->closed || v->broken) {
        pthread_mutex_unlock(&v->lock);
        return -EIO;
    }
    v->desc[2 * slot].len = (uint32_t)req_len;
    v->avail[2 + (v->avail_idx % v->qsize)] = (uint16_t)(2 * slot);
    r->pending = true;
    r->done = false;
    /* Release: descriptors and ring entry visible before idx (2.7.13). */
    atomic_thread_fence(memory_order_release);
    v->avail_idx++;
    v->avail[1] = v->avail_idx;
    /* The idx store (normal memory) must reach the device before the notify
     * write (device memory); a C11 fence (dmb ish) does not order against
     * device memory, __cpu_membarrier() (dmb sy) does. */
    __cpu_membarrier();
    wr32(v, VM_QUEUE_NOTIFY, 0);

    int rc = 0;
    for (;;) {
        reap(v);
        if (r->done || v->broken)
            break;
        if (v->opt.poll) {
            if (v->opt.timeout_ms && past(&dl)) {
                rc = ETIMEDOUT;
                break;
            }
            pthread_mutex_unlock(&v->lock);
            usleep(100);
            pthread_mutex_lock(&v->lock);
            continue;
        }
        rc = v->opt.timeout_ms ? pthread_cond_timedwait(&v->cond, &v->lock, &dl)
                               : pthread_cond_wait(&v->cond, &v->lock);
        if (rc == ETIMEDOUT) {
            reap(v); /* the completion may have raced the timeout */
            break;
        }
        rc = 0;
    }
    if (r->done) {
        r->pending = false;
        *resp = r->mem + v->bufsize;
        *resp_len = r->len;
        rc = 0;
    } else if (rc == ETIMEDOUT) {
        /* The device still owns the chain and may complete it later: the
         * slot stays out of use until reap() sees it come back. */
        r->stale = true;
        rc = -ETIMEDOUT;
    } else {
        rc = -EIO; /* broken */
    }
    pthread_mutex_unlock(&v->lock);
    return rc;
}

static void vio_put(void *impl, unsigned slot)
{
    struct vio9p *v = impl;
    pthread_mutex_lock(&v->lock);
    v->reqs[slot].taken = false;
    v->reqs[slot].done = false;
    pthread_cond_broadcast(&v->cond);
    pthread_mutex_unlock(&v->lock);
}

static unsigned vio_slots(void *impl)
{
    return ((struct vio9p *)impl)->nreqs;
}

static uint32_t vio_max_msize(void *impl)
{
    return ((struct vio9p *)impl)->bufsize;
}

static void vio_close(void *impl)
{
    struct vio9p *v = impl;
    pthread_mutex_lock(&v->lock);
    v->stop = true;
    v->closed = true;
    pthread_mutex_unlock(&v->lock);
    /* The caller holds every slot, but a stale (timed-out) request may
     * still be with the device. Reset first: the device stops touching the
     * rings and buffers before we unmap them. */
    (void)reset_device(v);
    dma_free(v);
    (void)name_detach(v->claim, 0);
    /* The IST may still be blocked in InterruptWait() and would read the
     * registers if one more interrupt arrived, so the register window stays
     * mapped and the IST is not joined: close() runs just before exit. */
}

static const struct fs9p_transport_ops vio_ops = {
    .get = vio_get,
    .buf = vio_buf,
    .rpc = vio_rpc,
    .put = vio_put,
    .slots = vio_slots,
    .max_msize = vio_max_msize,
    .close = vio_close,
};

/* --- open ----------------------------------------------------------------------- */

int vio9p_open(const char *mount_tag, const vio9p_options_t *opt, fs9p_transport_t *out)
{
    if (mount_tag == NULL || mount_tag[0] == '\0' || strlen(mount_tag) >= TAG_MAX ||
        opt->msize < 4096u)
        return -EINVAL;
    struct vio9p *v = calloc(1, sizeof *v); /* once per mount, at startup */
    if (v == NULL)
        return -ENOMEM;
    v->opt = *opt;
    v->bufsize = (uint32_t)align_up(opt->msize, VQ_PAGE);
    v->nreqs = opt->slots < 1 ? 1 : opt->slots > VIO9P_MAX_SLOTS ? VIO9P_MAX_SLOTS : opt->slots;

    int rc = claim_tag(v, mount_tag);
    if (rc == 0)
        rc = find_device(v, mount_tag);
    if (rc == 0) {
        v->irq = opt->irq >= 0 ? opt->irq : VIRT_MMIO_IRQ0 + (int)v->slot;
        logv(v, "using slot %u (0x%08x), %s, irq %d\n", v->slot,
             VIRT_MMIO_BASE + v->slot * VIRT_MMIO_SIZE, v->version == 1 ? "legacy" : "modern",
             v->irq);
        rc = negotiate(v);
    }
    if (rc == 0)
        rc = setup_queue(v);
    if (rc == 0)
        rc = start_ist(v);
    if (rc == 0) {
        wr32(v, VM_STATUS, rd32(v, VM_STATUS) | VSTAT_DRIVER_OK);
        out->ops = &vio_ops;
        out->impl = v;
        return 0;
    }

    if (v->regs != NULL)
        wr32(v, VM_STATUS, rd32(v, VM_STATUS) | VSTAT_FAILED);
    if (v->regs != NULL)
        (void)reset_device(v);
    dma_free(v);
    if (v->win != NULL)
        munmap_device_memory((void *)v->win, VIRT_MMIO_SLOTS * VIRT_MMIO_SIZE);
    if (v->claim != NULL)
        (void)name_detach(v->claim, 0);
    free(v);
    return rc;
}
