/*
 * Transport interface: the only coupling between the resource manager and
 * the virtio code (docs/dev/workflow.md, rule 5). The resource manager never sees
 * device registers, virtqueues or DMA memory; it encodes a request into a
 * buffer the transport lends it and gets a pointer to the reply back.
 *
 * Requests go through slots. Each slot has its own request and reply
 * buffers, so up to slots() requests can be in flight at once: take a
 * slot with get(), encode into buf(), send with rpc(), read the
 * reply, and give the slot back with put(). A slot belongs to one thread
 * from get() to put().
 *
 * Buffer lending avoids copying up to msize bytes in each direction: a
 * Tread reply can be MsgReply()'d straight from the slot's buffer.
 */
#ifndef FS9P_TRANSPORT_H
#define FS9P_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

struct fs9p_transport_ops {
    /* Take a free slot, waiting for one if all are busy. 0 or -errno. */
    int (*get)(void *impl, unsigned *slot);
    /* The slot's request buffer and its capacity. */
    uint8_t *(*buf)(void *impl, unsigned slot, size_t *cap);
    /* Send req_len bytes from the slot's buffer and wait for the reply. On
     * success resp and resp_len describe the reply bytes; they stay valid
     * until put(). Returns 0 or -errno. After -ETIMEDOUT the device may
     * still complete the request: the slot stays out of use after put()
     * until it does, and other slots carry on. */
    int (*rpc)(void *impl, unsigned slot, size_t req_len, const uint8_t **resp, size_t *resp_len);
    /* Give the slot back. */
    void (*put)(void *impl, unsigned slot);
    /* Number of slots (requests in flight at most). */
    unsigned (*slots)(void *impl);
    /* Largest message (either direction) the transport can carry. */
    uint32_t (*max_msize)(void *impl);
    /* Quiesce the device and release resources. The caller holds every
     * slot, so no request is in flight. */
    void (*close)(void *impl);
};

typedef struct {
    const struct fs9p_transport_ops *ops;
    void *impl;
} fs9p_transport_t;

/* --- virtio-mmio backend (transport_virtio.c) ------------------------------- */

/* At most this many slots: a 16-entry ring holds 8 two-descriptor chains. */
#define VIO9P_MAX_SLOTS 8u

typedef struct {
    uint32_t msize;      /* buffer size per direction (bytes) */
    unsigned slots;      /* requests in flight at most, 1 .. VIO9P_MAX_SLOTS */
    uint32_t timeout_ms; /* per-request reply timeout; 0 = wait forever */
    int irq;             /* -1 = derive from the slot */
    int poll;            /* nonzero: poll the used ring (debug only) */
    int verbose;         /* nonzero: log discovery and setup to stderr */
} vio9p_options_t;

/* Find the virtio-9p device exporting `mount_tag` on QEMU virt's virtio-mmio
 * transports, initialize it and set up its request queue. Only one process
 * may hold a tag (the name /dev/name/local/fs-9p/<tag>); a device left
 * behind by a killed instance is reset and reused. Returns 0 or -errno:
 * -ENODEV no such tag, -EBUSY tag held by another process,
 * -ENOMEM/-EIO/-EPERM on setup failures. */
int vio9p_open(const char *mount_tag, const vio9p_options_t *opt, fs9p_transport_t *out);

#endif /* FS9P_TRANSPORT_H */
