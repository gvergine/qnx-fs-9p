/*
 * fs-9p resource manager internals.
 *
 * Every open (including the combine-message opens behind stat()) gets its
 * own node: an iofunc attribute filled from Tgetattr plus the 9P fid that
 * Twalk produced. Nodes are not shared between opens: the host can change
 * files behind our back, and without a data cache sharing would buy
 * nothing (the optional metadata cache, cache.h, is keyed by path).
 */
#ifndef FS9P_RESMGR_H
#define FS9P_RESMGR_H

#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include <sys/iofunc.h>

#include "client.h"

typedef struct {
    iofunc_attr_t attr; /* must be first: ocb->attr points here */
    uint32_t fid;       /* P9_NOFID until walked (an open served by the cache) */
    char *path;         /* mount-relative; for lazy walks and cache updates */
    bool opened;        /* Tlopen done (lazily, on first read/readdir) */
    uint32_t iounit;
    atomic_uint ocbs; /* OCBs bound to this node; see resmgr_core.c */
    rcvid_t fresh;    /* attributes fetched while serving this receive; 0: none */
} fs9p_node_t;

/* OCB extended with its node, so freeing it doesn't depend on ocb->attr. */
typedef struct {
    iofunc_ocb_t ocb; /* must be first */
    fs9p_node_t *node;
} fs9p_ocb_t;

/* The only global mutable state: one mounted share per process. */
typedef struct {
    fs9p_session_t *sess;
    const char *mount_tag;
    const char *mount_point; /* absolute, no trailing slash */
    iofunc_mount_t mount;
    iofunc_funcs_t mount_funcs; /* OCB allocation hooks */
    iofunc_attr_t root_attr;    /* handle passed to resmgr_attach() */
    resmgr_connect_funcs_t connect;
    resmgr_io_funcs_t io;
    dispatch_t *dpp;
    int mntid; /* resmgr_attach() id of the mount point */
    bool ro;   /* -o ro */
    int debug;
} fs9p_fs_t;

extern fs9p_fs_t fs9p_fs;

static inline fs9p_node_t *fs9p_node(iofunc_ocb_t *ocb)
{
    return (fs9p_node_t *)ocb->attr;
}

/* resmgr_core.c */
/* Signals that unmount and exit: SIGTERM, and SIGINT and SIGHUP unless
 * inherited as ignored. */
void fs9p_term_signals(sigset_t *set);
int fs9p_resmgr_run(fs9p_session_t *sess, const char *mount_tag, const char *mount_point, bool ro,
                    int debug);
void fs9p_attr_fill(iofunc_attr_t *attr, const p9_attr_t *a);
uint32_t fs9p_mode_to_linux(mode_t m);
fs9p_node_t *fs9p_node_released(void);
void fs9p_node_free(fs9p_node_t *node);
/* Walk to the node's file now if its open was served from the cache. */
int fs9p_node_walked(fs9p_node_t *node);

/* resmgr_connect.c */
void fs9p_connect_init(resmgr_connect_funcs_t *c);

/* resmgr_io.c */
void fs9p_io_init(resmgr_io_funcs_t *io);

#endif /* FS9P_RESMGR_H */
