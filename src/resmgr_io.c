/* fs-9p I/O handlers: read (files and directories), write, stat, attribute
 * changes, truncate, sync, statvfs, close. */
#include <dirent.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <limits.h>

#include <sys/dcmd_blk.h> /* DCMD_FSYS_* */
#include <sys/iofunc.h>
#include <sys/neutrino.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include "cache.h"
#include "resmgr.h"

/* Directory entries returned per readdir message; the client asks again. */
#define FS9P_DIRBUF 4096u
/* Cap one read reply so the status fits a 32-bit ssize; short reads are fine. */
#define FS9P_READ_CAP 0x40000000u

/*
 * Has the client asked to unblock (a signal, or a timeout) or gone away?
 * It stays reply-blocked until we answer, so a long transfer checks this
 * between chunks; otherwise a signal would wait for the whole transfer.
 */
static bool client_unblocking(resmgr_context_t *ctp)
{
    struct _msg_info mi;
    if (MsgInfo(ctp->rcvid, &mi) == -1)
        return true; /* gone */
    return (mi.flags & _NTO_MI_UNBLOCK_REQ) != 0;
}

/* Tlopen on first data access, so stat()-only opens cost no server open. */
static int ensure_open(fs9p_node_t *node)
{
    if (node->opened)
        return EOK;
    int rc = fs9p_node_walked(node);
    if (rc != EOK)
        return rc;
    uint32_t flags = P9_DOTL_RDONLY;
    if (S_ISDIR(node->attr.mode))
        flags |= P9_DOTL_DIRECTORY;
    rc = -fs9p_lopen(fs9p_fs.sess, node->fid, flags, &node->iounit);
    if (rc == EOK)
        node->opened = true;
    return rc;
}

/* --- files ---------------------------------------------------------------------- */

struct read_ctx {
    resmgr_context_t *ctp;
    size_t written;
};

/* Copy one Rread payload straight from the transport buffer into the
 * client's reply buffer. */
static int read_sink(void *arg, const uint8_t *data, uint32_t len)
{
    struct read_ctx *rc = arg;
    if (resmgr_msgwrite(rc->ctp, data, len, rc->written) == -1)
        return -errno;
    rc->written += len;
    return 0;
}

static int read_file(resmgr_context_t *ctp, io_read_t *msg, iofunc_ocb_t *ocb, uint32_t xtype)
{
    fs9p_node_t *node = fs9p_node(ocb);
    uint64_t off;
    if (xtype == _IO_XTYPE_OFFSET)
        off = (uint64_t)((struct _xtype_offset *)(&msg->i + 1))->offset; /* pread() */
    else if (xtype == _IO_XTYPE_NONE)
        off = (uint64_t)ocb->offset;
    else
        return ENOSYS;

    int rc = ensure_open(node);
    if (rc != EOK)
        return rc;

    uint64_t want = _IO_READ_GET_NBYTES(msg);
    if (want > FS9P_READ_CAP)
        want = FS9P_READ_CAP;
    uint32_t chunk = fs9p_io_max(fs9p_fs.sess);
    if (node->iounit != 0 && node->iounit < chunk)
        chunk = node->iounit;

    /* Fill the whole request so read() of a regular file is not short
     * unless EOF is reached; each Tread is a separate locked exchange, so
     * other clients interleave between chunks. */
    struct read_ctx rctx = {ctp, 0};
    while (rctx.written < want) {
        if (client_unblocking(ctp)) {
            if (rctx.written == 0)
                return EINTR;
            break; /* report what was read, as an interrupted read does */
        }
        uint64_t left = want - rctx.written;
        uint32_t n = left < chunk ? (uint32_t)left : chunk;
        int got = fs9p_read(fs9p_fs.sess, node->fid, off + rctx.written, n, read_sink, &rctx);
        if (got < 0) {
            if (rctx.written == 0)
                return -got;
            break; /* report what we have; the error repeats on the next read */
        }
        if ((uint32_t)got < n)
            break; /* EOF */
    }
    if (xtype == _IO_XTYPE_NONE)
        ocb->offset += (off_t)rctx.written;
    _IO_SET_READ_NBYTES(ctp, (long)rctx.written);
    return _RESMGR_NPARTS(0); /* data already delivered with resmgr_msgwrite() */
}

/* --- directories ------------------------------------------------------------------ */

struct dir_ctx {
    uint8_t buf[FS9P_DIRBUF];
    size_t cap; /* min(client buffer, sizeof buf) */
    size_t used;
    uint64_t next; /* cookie after the last entry we kept */
    unsigned kept;
};

#define ROUND8(n) (((n) + 7u) & ~(size_t)7u)

/* Convert packed 9P entries into QNX struct dirent records (8-byte aligned,
 * NUL-terminated names) until the client's buffer is full. Entries that
 * don't fit are dropped; the next readdir resumes from `next`. */
static int dir_sink(void *arg, const uint8_t *data, uint32_t len)
{
    struct dir_ctx *dc = arg;
    size_t pos = 0;
    p9_dirent_t e;
    int rc;
    while ((rc = p9_dirent_next(data, len, &pos, &e)) == 1) {
        size_t reclen = ROUND8(offsetof(struct dirent, d_name) + e.name.len + 1);
        if (dc->used + reclen > dc->cap || e.name.len > INT16_MAX)
            break;
        struct dirent *d = (struct dirent *)(dc->buf + dc->used);
        memset(d, 0, reclen);
        d->d_ino = (ino_t)(e.qid.path ? e.qid.path : 1);
        d->d_offset = (off_t)e.offset;
        d->d_reclen = (int16_t)reclen;
        d->d_namelen = (int16_t)e.name.len;
        memcpy(d->d_name, e.name.ptr, e.name.len);
        dc->used += reclen;
        dc->next = e.offset;
        dc->kept++;
    }
    return rc < 0 ? -EIO : 0;
}

static int read_dir(resmgr_context_t *ctp, io_read_t *msg, iofunc_ocb_t *ocb, uint32_t xtype)
{
    if (xtype != _IO_XTYPE_NONE && xtype != _IO_XTYPE_READDIR)
        return ENOSYS;
    fs9p_node_t *node = fs9p_node(ocb);
    int rc = ensure_open(node);
    if (rc != EOK)
        return rc;

    /* 4 KiB on a pool thread's stack would be fine too; thread-local keeps
     * the handler's stack use small and needs no per-request allocation. */
    static _Thread_local struct dir_ctx dc;
    uint64_t want = _IO_READ_GET_NBYTES(msg);
    dc.cap = want < sizeof dc.buf ? (size_t)want : sizeof dc.buf;
    dc.used = 0;
    dc.kept = 0;
    /* The 9P readdir cookie lives in ocb->offset, so rewinddir() (an lseek
     * to 0 handled by the iofunc default) restarts the listing. */
    dc.next = (uint64_t)ocb->offset;

    int got = fs9p_readdir(fs9p_fs.sess, node->fid, (uint64_t)ocb->offset, (uint32_t)dc.cap,
                           dir_sink, &dc);
    if (got < 0)
        return -got;
    if (got > 0 && dc.kept == 0)
        return EINVAL; /* client buffer can't hold even one entry */
    if (dc.used > 0 && resmgr_msgwrite(ctp, dc.buf, dc.used, 0) == -1)
        return errno;
    ocb->offset = (off_t)dc.next;
    _IO_SET_READ_NBYTES(ctp, (long)dc.used);
    return _RESMGR_NPARTS(0);
}

static int fs9p_read_h(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb)
{
    int rc = iofunc_read_verify(ctp, msg, ocb, NULL);
    if (rc != EOK)
        return rc;
    uint32_t xtype = msg->i.xtype & _IO_XTYPE_MASK;
    if (S_ISDIR(ocb->attr->mode))
        return read_dir(ctp, msg, ocb, xtype);
    return read_file(ctp, msg, ocb, xtype);
}

/* --- stat, lseek, close -------------------------------------------------------- */

/* Re-read the node's attributes from the server (and update the cache). */
static int refresh(fs9p_node_t *node)
{
    int rc = fs9p_node_walked(node);
    if (rc != EOK)
        return rc;
    p9_attr_t a;
    rc = -fs9p_getattr(fs9p_fs.sess, node->fid, P9_GETATTR_BASIC, &a);
    if (rc == EOK) {
        fs9p_attr_fill(&node->attr, &a);
        fs9p_cache_put(node->path, &a);
    }
    return rc;
}

/* The same, but a fresh cache entry will do (cache=MS). */
static int refresh_cached(fs9p_node_t *node)
{
    bool exists;
    p9_attr_t a;
    if (fs9p_cache_get(node->path, &exists, &a) && exists) {
        fs9p_attr_fill(&node->attr, &a);
        return EOK;
    }
    return refresh(node);
}

static int fs9p_stat_h(resmgr_context_t *ctp, io_stat_t *msg, RESMGR_OCB_T *ocb)
{
    fs9p_node_t *node = fs9p_node(ocb);
    /* Within the same receive as the open (a stat() combine message) the
     * attributes are a few microseconds old: skip the second Tgetattr. Any
     * later fstat() asks the server again. */
    if (node->fresh != 0 && node->fresh == ctp->rcvid) {
        node->fresh = 0;
    } else {
        node->fresh = 0;
        int rc = refresh_cached(node);
        if (rc != EOK)
            return rc;
    }
    return iofunc_stat_default(ctp, msg, ocb);
}

/* SEEK_END needs the current size, which the host may have changed. */
static int fs9p_lseek_h(resmgr_context_t *ctp, io_lseek_t *msg, RESMGR_OCB_T *ocb)
{
    if (msg->i.whence == SEEK_END) {
        int rc = refresh_cached(fs9p_node(ocb));
        if (rc != EOK)
            return rc;
    }
    return iofunc_lseek_default(ctp, msg, ocb);
}

/* The node goes with its last OCB, which may not be this one (resmgr_core.c). */
static int fs9p_close_ocb_h(resmgr_context_t *ctp, void *reserved, RESMGR_OCB_T *ocb)
{
    int rc = iofunc_close_ocb_default(ctp, reserved, ocb);
    fs9p_node_t *node = fs9p_node_released();
    if (node != NULL)
        fs9p_node_free(node);
    return rc;
}

/* --- devctl -------------------------------------------------------------------- */

/* Replies are built in a thread-local buffer, not in place: in a combine
 * message (statvfs() on a path) the devctl follows the connect message in
 * the receive buffer, so the room left after it isn't fixed. */
struct devctl_reply {
    struct _io_devctl_reply hdr;
    union {
        struct __msg_statvfs vfs;
        char str[PATH_MAX];
    } data;
};
_Static_assert(offsetof(struct devctl_reply, data) == sizeof(struct _io_devctl_reply),
               "devctl data must follow the reply header directly");
static _Thread_local struct devctl_reply dreply;

static int devctl_reply(resmgr_context_t *ctp, uint32_t nbytes)
{
    memset(&dreply.hdr, 0, sizeof dreply.hdr);
    dreply.hdr.nbytes = nbytes;
    return _RESMGR_PTR(ctp, &dreply, sizeof dreply.hdr + nbytes);
}

/* NUL-terminated string reply; refused rather than truncated if it doesn't
 * fit the client's buffer (df passes 1024 bytes). */
static int devctl_str(resmgr_context_t *ctp, const io_devctl_t *msg, const char *s)
{
    size_t n = strlen(s) + 1;
    if (n > sizeof dreply.data.str || n > msg->i.nbytes)
        return EOVERFLOW;
    memcpy(dreply.data.str, s, n);
    return devctl_reply(ctp, (uint32_t)n);
}

/* statvfs()/fstatvfs(), and so df: Tstatfs on this open's fid, so a host
 * mount inside the share reports its own numbers. */
static int statvfs_h(resmgr_context_t *ctp, fs9p_node_t *node)
{
    p9_statfs_t st;
    int rc = fs9p_node_walked(node);
    if (rc == EOK)
        rc = -fs9p_statfs(fs9p_fs.sess, node->fid, &st);
    if (rc != EOK)
        return rc;

    struct __msg_statvfs *v = &dreply.data.vfs;
    memset(v, 0, sizeof *v);
    /* Rstatfs has one block size, and the block counts are in its units. */
    v->f_bsize = st.bsize;
    v->f_frsize = st.bsize;
    v->f_blocks = st.blocks;
    v->f_bfree = st.bfree;
    v->f_bavail = st.bavail;
    v->f_files = st.files;
    v->f_ffree = st.ffree;
    v->f_favail = st.ffree; /* 9P has no separate count for unprivileged users */
    v->f_fsid = (uint32_t)(st.fsid ^ (st.fsid >> 32));
    snprintf(v->f_basetype, sizeof v->f_basetype, "9p");
    v->f_flag = ST_NOSUID | (fs9p_fs.ro ? ST_RDONLY : 0); /* as the mount flags */
    v->f_namemax = st.namelen;
    return devctl_reply(ctp, sizeof *v);
}

static int fs9p_devctl_h(resmgr_context_t *ctp, io_devctl_t *msg, RESMGR_OCB_T *ocb)
{
    if (fs9p_fs.debug)
        fprintf(stderr, "fs-9p: devctl 0x%08x nbytes %u\n", (unsigned)msg->i.dcmd,
                (unsigned)msg->i.nbytes);
    switch ((unsigned)msg->i.dcmd) {
    case DCMD_FSYS_STATVFS:
        return statvfs_h(ctp, fs9p_node(ocb));
    /* df's "Filesystem" and "Mounted on" columns. The source is the mount
     * tag, as a Linux guest shows for a 9p mount. */
    case DCMD_FSYS_MOUNTED_ON:
        return devctl_str(ctp, msg, fs9p_fs.mount_tag);
    case DCMD_FSYS_MOUNTED_AT:
        return devctl_str(ctp, msg, fs9p_fs.mount_point);
    default:
        return iofunc_devctl_default(ctp, msg, ocb);
    }
}

/* --- write --------------------------------------------------------------------- */

struct write_ctx {
    resmgr_context_t *ctp;
    size_t data_off; /* where the data starts in the client's message */
    size_t done;
};

/* Copy the next chunk of the client's data from its message straight into
 * the Twrite request. */
static int write_source(void *arg, uint8_t *dst, uint32_t len)
{
    struct write_ctx *wc = arg;
    ssize_t n = resmgr_msgread(wc->ctp, dst, len, wc->data_off + wc->done);
    if (n < 0)
        return -errno;
    return (size_t)n == len ? 0 : -EFAULT;
}

static int fs9p_write_h(resmgr_context_t *ctp, io_write_t *msg, RESMGR_OCB_T *ocb)
{
    if (fs9p_fs.ro)
        return EROFS;
    int rc = iofunc_write_verify(ctp, msg, ocb, NULL);
    if (rc != EOK)
        return rc;
    fs9p_node_t *node = fs9p_node(ocb);
    if (!node->opened)
        return EBADF; /* opens with write access open on the server at once */

    uint32_t xtype = msg->i.xtype & _IO_XTYPE_MASK;
    uint64_t off;
    size_t data_off = sizeof msg->i;
    if (xtype == _IO_XTYPE_OFFSET) { /* pwrite() */
        off = (uint64_t)((struct _xtype_offset *)(&msg->i + 1))->offset;
        data_off += sizeof(struct _xtype_offset);
    } else if (xtype == _IO_XTYPE_NONE) {
        off = (uint64_t)ocb->offset;
    } else {
        return ENOSYS;
    }

    uint64_t want = _IO_WRITE_GET_NBYTES(msg);
    if (want > FS9P_READ_CAP)
        want = FS9P_READ_CAP;
    uint32_t chunk = fs9p_write_max(fs9p_fs.sess);
    if (node->iounit != 0 && node->iounit < chunk)
        chunk = node->iounit;

    /* Write-through, one Twrite per chunk; other clients interleave between
     * chunks. A short or failed chunk ends the write with what was done. */
    struct write_ctx wc = {ctp, data_off, 0};
    int err = EOK;
    while (wc.done < want) {
        if (client_unblocking(ctp)) {
            if (wc.done == 0)
                return EINTR;
            break;
        }
        uint64_t left = want - wc.done;
        uint32_t n = left < chunk ? (uint32_t)left : chunk;
        int got = fs9p_write(fs9p_fs.sess, node->fid, off + wc.done, n, write_source, &wc);
        if (got < 0) {
            err = -got;
            break;
        }
        wc.done += (uint32_t)got;
        if ((uint32_t)got < n)
            break;
    }
    if (wc.done == 0 && err != EOK)
        return err;
    fs9p_cache_forget(node->path); /* its size and times changed */

    if (ocb->ioflag & O_APPEND) {
        /* The host appended wherever the end was; ask where that is now. */
        if (refresh(node) == EOK && xtype == _IO_XTYPE_NONE)
            ocb->offset = node->attr.nbytes;
    } else {
        uint64_t end = off + wc.done;
        if (end > (uint64_t)node->attr.nbytes)
            node->attr.nbytes = (off_t)end;
        if (xtype == _IO_XTYPE_NONE)
            ocb->offset = (off_t)end;
    }
    _IO_SET_WRITE_NBYTES(ctp, (long)wc.done);
    return EOK;
}

/* --- attribute changes, truncate, sync ------------------------------------------ */

/* Send changes that iofunc_chmod() and friends validated (permissions, the
 * POSIX rules) and applied to the cached attribute, then re-read it from
 * the server either way so the cache shows what the host really did. */
static int setattr_commit(fs9p_node_t *node, const p9_setattr_t *sa)
{
    int rc = fs9p_node_walked(node);
    if (rc == EOK)
        rc = -fs9p_setattr(fs9p_fs.sess, node->fid, sa);
    fs9p_cache_forget(node->path);
    (void)refresh(node);
    return rc;
}

static int fs9p_chmod_h(resmgr_context_t *ctp, io_chmod_t *msg, RESMGR_OCB_T *ocb)
{
    if (fs9p_fs.ro)
        return EROFS;
    fs9p_node_t *node = fs9p_node(ocb);
    int rc = iofunc_chmod(ctp, msg, ocb, &node->attr);
    if (rc != EOK)
        return rc;
    p9_setattr_t sa = {0};
    sa.valid = P9_SETATTR_MODE;
    sa.mode = fs9p_mode_to_linux(node->attr.mode);
    return setattr_commit(node, &sa);
}

static int fs9p_chown_h(resmgr_context_t *ctp, io_chown_t *msg, RESMGR_OCB_T *ocb)
{
    if (fs9p_fs.ro)
        return EROFS;
    fs9p_node_t *node = fs9p_node(ocb);
    int rc = iofunc_chown(ctp, msg, ocb, &node->attr);
    if (rc != EOK)
        return rc;
    p9_setattr_t sa = {0};
    sa.valid = P9_SETATTR_UID | P9_SETATTR_GID;
    sa.uid = (uint32_t)node->attr.uid;
    sa.gid = (uint32_t)node->attr.gid;
    return setattr_commit(node, &sa);
}

static int fs9p_utime_h(resmgr_context_t *ctp, io_utime_t *msg, RESMGR_OCB_T *ocb)
{
    if (fs9p_fs.ro)
        return EROFS;
    fs9p_node_t *node = fs9p_node(ocb);
    int rc = iofunc_utime(ctp, msg, ocb, &node->attr);
    if (rc != EOK)
        return rc;
    /* For "now", iofunc may only mark the times stale; make them real. */
    (void)iofunc_time_update(&node->attr);
    p9_setattr_t sa = {0};
    sa.valid = P9_SETATTR_ATIME | P9_SETATTR_ATIME_SET | P9_SETATTR_MTIME | P9_SETATTR_MTIME_SET;
    sa.atime_sec = (uint64_t)node->attr.atime;
    sa.atime_nsec = node->attr.atime_ns;
    sa.mtime_sec = (uint64_t)node->attr.mtime;
    sa.mtime_nsec = node->attr.mtime_ns;
    return setattr_commit(node, &sa);
}

/* ftruncate() and friends. 9P can only set the size, so freeing a range
 * that doesn't reach the end of the file (punching a hole) isn't supported,
 * and allocating space only grows the file (no reservation). */
static int fs9p_space_h(resmgr_context_t *ctp, io_space_t *msg, RESMGR_OCB_T *ocb)
{
    if (fs9p_fs.ro)
        return EROFS;
    int rc = iofunc_space_verify(ctp, msg, ocb, NULL);
    if (rc != EOK)
        return rc;
    fs9p_node_t *node = fs9p_node(ocb);
    rc = refresh(node);
    if (rc != EOK)
        return rc;
    if (fs9p_fs.debug)
        fprintf(stderr, "fs-9p: space subtype %u whence %d start %llu len %llu (size %lld)\n",
                (unsigned)msg->i.subtype, (int)msg->i.whence, (unsigned long long)msg->i.start,
                (unsigned long long)msg->i.len, (long long)node->attr.nbytes);

    uint64_t size = (uint64_t)node->attr.nbytes;
    uint64_t base;
    switch (msg->i.whence) {
    case SEEK_SET:
        base = 0;
        break;
    case SEEK_CUR:
        base = (uint64_t)ocb->offset;
        break;
    case SEEK_END:
        base = size;
        break;
    default:
        return EINVAL;
    }
    uint64_t start = base + msg->i.start, len = msg->i.len, newsize;
    switch (msg->i.subtype) {
    case F_FREESP: /* the *64 variants have the same values on 64-bit QNX */
        if (len != 0 && start + len < size)
            return ENOTSUP;
        newsize = start;
        break;
    case F_GROWSP:
    case F_ALLOCSP:
        newsize = start + len > size ? start + len : size;
        break;
    default:
        return EINVAL;
    }
    if (newsize != size) {
        p9_setattr_t sa = {0};
        sa.valid = P9_SETATTR_SIZE;
        sa.size = newsize;
        rc = setattr_commit(node, &sa);
        if (rc != EOK)
            return rc;
    }
    msg->o = (uint64_t)node->attr.nbytes;
    return _RESMGR_PTR(ctp, &msg->o, sizeof msg->o);
}

static int fs9p_sync_h(resmgr_context_t *ctp, io_sync_t *msg, RESMGR_OCB_T *ocb)
{
    int rc = iofunc_sync_verify(ctp, msg, ocb);
    if (rc != EOK)
        return rc;
    fs9p_node_t *node = fs9p_node(ocb);
    rc = ensure_open(node);
    if (rc != EOK)
        return rc;
    uint32_t datasync = (msg->i.flag & O_SYNC) == 0 && (msg->i.flag & O_DSYNC) != 0;
    return -fs9p_fsync(fs9p_fs.sess, node->fid, datasync);
}

void fs9p_io_init(resmgr_io_funcs_t *io)
{
    io->read = fs9p_read_h;
    io->read64 = fs9p_read_h; /* same message layout; length via _IO_READ_GET_NBYTES */
    io->stat = fs9p_stat_h;
    io->close_ocb = fs9p_close_ocb_h;
    io->write = fs9p_write_h;
    io->write64 = fs9p_write_h;
    io->chmod = fs9p_chmod_h;
    io->chown = fs9p_chown_h;
    io->utime = fs9p_utime_h;
    io->utime64 = fs9p_utime_h;
    io->space = fs9p_space_h;
    io->sync = fs9p_sync_h;
    io->lseek = fs9p_lseek_h;
    io->devctl = fs9p_devctl_h;
}
