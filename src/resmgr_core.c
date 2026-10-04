/* fs-9p resource manager: attach at the mount point and serve requests. */
#define THREAD_POOL_PARAM_T dispatch_context_t

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/mount.h> /* _MOUNT_* */
#include <sys/procmgr.h>
#include <sys/stat.h>

#include "resmgr.h"

fs9p_fs_t fs9p_fs;

/* Linux file type bits (st_mode & 0170000, octal, as Rgetattr carries them)
 * to QNX S_IF*; explicit, not assumed equal. Unknown types show as regular
 * files rather than failing the stat. */
static mode_t mode_from_linux(uint32_t m)
{
    mode_t perm = (mode_t)(m & 07777u);
    switch (m & 0170000u) {
    case 0040000u:
        return S_IFDIR | perm;
    case 0100000u:
        return S_IFREG | perm;
    case 0120000u:
        return S_IFLNK | perm;
    case 0020000u:
        return S_IFCHR | perm;
    case 0060000u:
        return S_IFBLK | perm;
    case 0010000u:
        return S_IFIFO | perm;
    case 0140000u:
        return S_IFSOCK | perm;
    default:
        return S_IFREG | perm;
    }
}

/* The inverse, for Tsetattr: with security_model=mapped-xattr QEMU stores the
 * mode it is sent as the whole st_mode, so the type bits must be included
 * (observed: a chmod'ed directory turned into a regular file). */
uint32_t fs9p_mode_to_linux(mode_t m)
{
    uint32_t perm = (uint32_t)m & 07777u;
    switch (m & S_IFMT) {
    case S_IFDIR:
        return 0040000u | perm;
    case S_IFLNK:
        return 0120000u | perm;
    case S_IFCHR:
        return 0020000u | perm;
    case S_IFBLK:
        return 0060000u | perm;
    case S_IFIFO:
        return 0010000u | perm;
    case S_IFSOCK:
        return 0140000u | perm;
    default:
        return 0100000u | perm;
    }
}

void fs9p_attr_fill(iofunc_attr_t *attr, const p9_attr_t *a)
{
    attr->mode = mode_from_linux(a->mode);
    attr->uid = (uid_t)a->uid;
    attr->gid = (gid_t)a->gid;
    attr->nlink = (nlink_t)a->nlink;
    attr->nbytes = (off_t)a->size;
    /* inode 0 means "unused entry" to some tools; qid.path is never 0 on
     * QEMU, but guard anyway. */
    attr->inode = (ino_t)(a->qid.path ? a->qid.path : 1);
    attr->rdev = 0; /* QEMU can't open device nodes; none are created */
    attr->mtime = (time_t)a->mtime_sec;
    attr->mtime_ns = (unsigned)a->mtime_nsec;
    attr->atime = (time_t)a->atime_sec;
    attr->atime_ns = (unsigned)a->atime_nsec;
    attr->ctime = (time_t)a->ctime_sec;
    attr->ctime_ns = (unsigned)a->ctime_nsec;
    /* Times come from the server; stop iofunc from stamping "now" on them. */
    attr->flags &= ~(uint32_t)(IOFUNC_ATTR_MTIME | IOFUNC_ATTR_ATIME | IOFUNC_ATTR_CTIME |
                               IOFUNC_ATTR_DIRTY_MASK);
}

/*
 * Node lifetime. A node can have more than one OCB: mapping a file (exec of
 * a binary on the share) makes iofunc_mmap_default() attach a second OCB
 * for the memory manager, and both get closed (verified 2026-10-04: freeing
 * the node on the first close led to a double clunk and abort). Each OCB
 * holds a reference, taken and dropped in the mount's ocb_calloc/ocb_free
 * hooks. iofunc may still use the attribute until close returns, so the
 * last ocb_free only hands the node back to its caller (close_ocb, or the
 * open error path) through `released`, and the caller frees it.
 */
static _Thread_local fs9p_node_t *released;

static IOFUNC_OCB_T *ocb_calloc_h(resmgr_context_t *ctp, IOFUNC_ATTR_T *attr)
{
    (void)ctp;
    fs9p_ocb_t *o = calloc(1, sizeof *o); /* one per open or mapping */
    if (o == NULL)
        return NULL;
    if (attr != &fs9p_fs.root_attr) {
        o->node = (fs9p_node_t *)attr;
        atomic_fetch_add(&o->node->ocbs, 1);
    }
    return &o->ocb;
}

static void ocb_free_h(IOFUNC_OCB_T *ocb)
{
    fs9p_ocb_t *o = (fs9p_ocb_t *)ocb;
    fs9p_node_t *node = o->node;
    free(o);
    if (node != NULL && atomic_fetch_sub(&node->ocbs, 1) == 1)
        released = node;
}

fs9p_node_t *fs9p_node_released(void)
{
    fs9p_node_t *node = released;
    released = NULL;
    return node;
}

void fs9p_node_free(fs9p_node_t *node)
{
    if (node->fid != P9_NOFID) {
        int rc = fs9p_clunk(fs9p_fs.sess, node->fid);
        if (rc != 0 && fs9p_fs.debug)
            fprintf(stderr, "fs-9p: clunk fid %u: %s\n", node->fid, strerror(-rc));
    }
    free(node->path);
    free(node);
}

int fs9p_node_walked(fs9p_node_t *node)
{
    if (node->fid != P9_NOFID)
        return EOK;
    /* Permissions were checked when it was opened. */
    uint32_t fid;
    int rc = -fs9p_walk(fs9p_fs.sess, node->path, &fid, NULL);
    if (rc == ENOTSUP)
        rc = ENOENT; /* the path now crosses a symlink: not the file opened */
    if (rc == EOK)
        node->fid = fid;
    return rc;
}

void fs9p_term_signals(sigset_t *set)
{
    sigemptyset(set);
    sigaddset(set, SIGTERM);
    /* SIGINT and SIGHUP only if they weren't inherited as ignored: a
     * background job of a shell without job control starts with SIGINT
     * ignored, and nohup ignores SIGHUP on purpose. */
    static const int maybe[] = {SIGINT, SIGHUP};
    for (size_t i = 0; i < sizeof maybe / sizeof maybe[0]; i++) {
        struct sigaction sa;
        if (sigaction(maybe[i], NULL, &sa) == 0 && sa.sa_handler != SIG_IGN)
            sigaddset(set, maybe[i]);
    }
}

/*
 * Clean unmount on SIGTERM (slay's default), SIGINT or SIGHUP: no new
 * lookups, wait for the request in flight, reset the device, release the
 * tag claim, exit. The device is reset only after its last request has
 * completed, so it never writes into memory the process gives back (the
 * hazard with SIGKILL, documented in docs/user/behaviour.md). Clients with
 * open files get errors once the process is gone.
 */
static void *shutdown_main(void *arg)
{
    (void)arg;
    sigset_t sigs;
    fs9p_term_signals(&sigs);
    int sig = 0;
    while (sigwait(&sigs, &sig) != 0)
        ;
    fs9p_fs_t *fs = &fs9p_fs;
    if (fs->debug)
        fprintf(stderr, "fs-9p: signal %d: unmounting %s\n", sig, fs->mount_point);
    (void)resmgr_detach(fs->dpp, fs->mntid, _RESMGR_DETACH_PATHNAME);
    fs9p_session_stop(fs->sess);
    exit(EXIT_SUCCESS);
}

int fs9p_resmgr_run(fs9p_session_t *sess, const char *mount_tag, const char *mount_point, bool ro,
                    int debug)
{
    fs9p_fs_t *fs = &fs9p_fs;
    fs->sess = sess;
    fs->mount_tag = mount_tag;
    fs->mount_point = mount_point;
    fs->ro = ro;
    fs->debug = debug;

    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &fs->connect, _RESMGR_IO_NFUNCS, &fs->io);
    fs9p_connect_init(&fs->connect);
    fs9p_io_init(&fs->io);

    memset(&fs->mount, 0, sizeof fs->mount);
    /* CHOWN_RESTRICTED: only root changes a file's owner; the owner may
     * change its group to one of their own (POSIX, and what Linux does).
     * SYNC_IO: without it iofunc_sync_verify() fails fsync() with EINVAL
     * (observed); writes go straight to the server, and Tfsync flushes. */
    fs->mount.conf =
        IOFUNC_PC_SYMLINK | IOFUNC_PC_NO_TRUNC | IOFUNC_PC_CHOWN_RESTRICTED | IOFUNC_PC_SYNC_IO;
    /* iofunc_devctl_default() reports these for DCMD_ALL_GETMOUNTFLAGS, which
     * the process manager asks when it executes a file from the share. With
     * NOSUID the setuid and setgid bits are ignored. File ownership is
     * whatever the host says (with mapped-xattr, any host user can make a
     * file look root-owned and setuid), so honoring them would let the
     * share grant root in the guest (verified 2026-10-04). */
    fs->mount.flags = _MOUNT_NOSUID | (ro ? _MOUNT_READONLY : 0);
    fs->mount.blocksize = 4096; /* st_blksize; refined from Rstatfs later */
    /* nfuncs = 2: only ocb_calloc and ocb_free are ours; attr locking stays
     * the iofunc default. */
    memset(&fs->mount_funcs, 0, sizeof fs->mount_funcs);
    fs->mount_funcs.nfuncs = 2;
    fs->mount_funcs.ocb_calloc = ocb_calloc_h;
    fs->mount_funcs.ocb_free = ocb_free_h;
    fs->mount.funcs = &fs->mount_funcs;

    /* Only used as the resmgr_attach() handle: every open, the root
     * included, gets its own node in io_open. */
    iofunc_attr_init(&fs->root_attr, S_IFDIR | 0555, NULL, NULL);
    fs->root_attr.mount = &fs->mount;

    dispatch_t *dpp = dispatch_create();
    if (dpp == NULL)
        return -errno;
    fs->dpp = dpp;
    resmgr_attr_t rattr;
    memset(&rattr, 0, sizeof rattr);
    rattr.nparts_max = 1;
    rattr.msg_max_size = 2048;
    /* _RESMGR_FLAG_DIR: we accept every path at or below the mount point. */
    fs->mntid = resmgr_attach(dpp, &rattr, mount_point, _FTYPE_ANY, _RESMGR_FLAG_DIR, &fs->connect,
                              &fs->io, &fs->root_attr);
    if (fs->mntid == -1)
        return -errno;

    thread_pool_attr_t pattr;
    memset(&pattr, 0, sizeof pattr);
    pattr.handle = dpp;
    pattr.context_alloc = dispatch_context_alloc;
    pattr.block_func = dispatch_block;
    pattr.unblock_func = dispatch_unblock;
    pattr.handler_func = dispatch_handler;
    pattr.context_free = dispatch_context_free;
    /* Up to 8 threads: as many 9P requests as the transport has slots can be
     * in flight at once (-o requests=, at most 8), plus threads for calls
     * that need no request. */
    pattr.lo_water = 2;
    pattr.increment = 1;
    pattr.hi_water = 4;
    pattr.maximum = 8;
    thread_pool_t *tpp = thread_pool_create(&pattr, POOL_FLAG_EXIT_SELF);
    if (tpp == NULL)
        return -errno;

    /* Detach from the terminal only once everything is up, so setup errors
     * still reach the caller. procmgr_daemon() keeps the process (and its
     * interrupt thread) rather than forking: verified, reads after mount
     * complete (2026-10-03). In debug mode stay in the foreground. */
    if (!debug && procmgr_daemon(EXIT_SUCCESS, PROCMGR_DAEMON_NOCHDIR) == -1)
        return -errno;

    pthread_t shut;
    int prc = pthread_create(&shut, NULL, shutdown_main, NULL);
    if (prc != 0)
        return -prc;
    thread_pool_start(tpp); /* POOL_FLAG_EXIT_SELF: does not return */
    return -EIO;
}
