/* fs-9p connect handlers: open (and create), readlink; namespace changes. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/iofunc.h>
#include <sys/neutrino.h>
#include <sys/stat.h>

#include "cache.h"
#include "resmgr.h"

static void dbg(const char *op, const char *path, int rc)
{
    if (fs9p_fs.debug)
        fprintf(stderr, "fs-9p: %s \"%s\" -> %s\n", op, path, rc == EOK ? "ok" : strerror(rc));
}

struct link_buf {
    char data[PATH_MAX];
    uint32_t len;
};

static int link_sink(void *arg, const uint8_t *data, uint32_t len)
{
    struct link_buf *lb = arg;
    if (len >= sizeof lb->data)
        return -ENAMETOOLONG;
    memcpy(lb->data, data, len);
    lb->len = len;
    return 0;
}

struct search_ctx {
    resmgr_context_t *ctp;
    const struct _client_info *info;
};

/* Search (x) permission on a directory a walk passes through. With the
 * metadata cache on, its attributes are kept too, so a later lookup below
 * it can check permissions without asking the server. */
static int search_ok(void *arg, const char *dirpath, size_t len, const p9_attr_t *a)
{
    const struct search_ctx *sc = arg;
    if (fs9p_cache_enabled() && len < PATH_MAX) {
        char p[PATH_MAX];
        memcpy(p, dirpath, len);
        p[len] = '\0';
        fs9p_cache_put(p, a);
    }
    iofunc_attr_t dattr;
    iofunc_attr_init(&dattr, S_IFDIR, NULL, NULL);
    dattr.mount = &fs9p_fs.mount;
    fs9p_attr_fill(&dattr, a);
    if (!S_ISDIR(dattr.mode))
        return -ENOTDIR; /* a file used as a directory, not a permission matter */
    return -iofunc_check_access(sc->ctp, &dattr, S_IEXEC, sc->info);
}

/*
 * fs9p_walk() on behalf of a client: a non-root client also needs search
 * permission on every directory the path passes through, as on a local
 * filesystem (QEMU only checks as the host user). That costs a Tgetattr per
 * directory, so root, who may search any directory, keeps the one-request
 * walk. info NULL: no checks (the path was checked already).
 */
static int walk_as(resmgr_context_t *ctp, const struct _client_info *info, const char *path,
                   uint32_t *fid, size_t *link_end)
{
    if (info == NULL || info->cred.euid == 0)
        return -fs9p_walk(fs9p_fs.sess, path, fid, link_end);
    struct search_ctx sc = {ctp, info};
    return -fs9p_walk_check(fs9p_fs.sess, path, fid, link_end, search_ok, &sc);
}

/* Target of the symlink at path[0..len), walked as `info` (see walk_as). */
static int read_link(resmgr_context_t *ctp, const struct _client_info *info, const char *path,
                     size_t len, struct link_buf *lb)
{
    char prefix[PATH_MAX];
    if (len >= sizeof prefix)
        return ENAMETOOLONG;
    memcpy(prefix, path, len);
    prefix[len] = '\0';
    uint32_t fid;
    int rc = walk_as(ctp, info, prefix, &fid, NULL);
    if (rc != EOK)
        return rc;
    lb->len = 0;
    /* Check the type ourselves: with security_model=mapped-xattr QEMU
     * implements Treadlink by reading the file, so a regular file would
     * return its contents and a directory EISDIR (observed). POSIX says
     * EINVAL for anything that isn't a symlink. */
    p9_attr_t a;
    rc = -fs9p_getattr(fs9p_fs.sess, fid, P9_GETATTR_MODE, &a);
    if (rc == EOK && (a.mode & 0170000u) != 0120000u)
        rc = EINVAL;
    if (rc == EOK)
        rc = -fs9p_readlink(fs9p_fs.sess, fid, link_sink, lb);
    (void)fs9p_clunk(fs9p_fs.sess, fid);
    return rc;
}

/*
 * Reply in link-reply form: struct _io_connect_link_reply with nentries 0
 * ("path is a symbolic link") followed by a NUL-terminated path. Found by
 * experiment (see docs/dev/op-mapping.md): the client sizes its reply buffer
 * for this layout
 * (reply_max 1556 = header + 16 entries + PATH_MAX). readlink() wants the
 * byte count as a plain status; a redirect wants _IO_CONNECT_RET_LINK.
 */
static int reply_link(resmgr_context_t *ctp, io_open_t *msg, const char *path, size_t len,
                      long status)
{
    static _Thread_local struct {
        struct _io_connect_link_reply hdr;
        char path[PATH_MAX + 1];
    } rep;
    if (len > PATH_MAX || sizeof rep.hdr + len + 1 > msg->connect.reply_max)
        return ENAMETOOLONG;
    memset(&rep.hdr, 0, sizeof rep.hdr);
    rep.hdr.file_type = msg->connect.file_type;
    rep.hdr.eflag = msg->connect.eflag;
    rep.hdr.path_len = (uint16_t)(len + 1);
    memcpy(rep.path, path, len);
    rep.path[len] = '\0';
    MsgReply(ctp->rcvid, status, &rep, sizeof rep.hdr + len + 1);
    return _RESMGR_NOREPLY;
}

/*
 * The server doesn't cross symlinks during Twalk, so the client library
 * resolves them: we hand back the path with the link at path[0..link_end)
 * replaced by its target, and the client starts the lookup again. A
 * relative target is anchored at the link's directory under our mount
 * point; an absolute one is used as is (as in a Linux guest, it names a
 * path in the guest, not on the host). Loop limits are the client's job.
 */
static int redirect_link(resmgr_context_t *ctp, io_open_t *msg, const char *path, size_t link_end)
{
    static _Thread_local struct link_buf lb;
    /* The walk that found the link already checked the path up to it. */
    int rc = read_link(ctp, NULL, path, link_end, &lb);
    if (rc != EOK)
        return rc;
    lb.data[lb.len] = '\0';

    static _Thread_local char out[PATH_MAX + 1];
    const char *rest = path + link_end; /* "" or "/more/components" */
    size_t dirlen = link_end;
    while (dirlen > 0 && path[dirlen - 1] != '/')
        dirlen--; /* "a/b/link" -> "a/b/" */
    int n;
    if (lb.data[0] == '/')
        n = snprintf(out, sizeof out, "%s%s", lb.data, rest);
    else
        n = snprintf(out, sizeof out, "%s/%.*s%s%s", fs9p_fs.mount_point, (int)dirlen, path,
                     lb.data, rest);
    if (n < 0 || (size_t)n >= sizeof out)
        return ENAMETOOLONG;
    if (fs9p_fs.debug)
        fprintf(stderr, "fs-9p: open \"%s\" -> link to \"%s\"\n", path, out);
    return reply_link(ctp, msg, out, (size_t)n,
                      (long)(_IO_CONNECT_RET_FLAG | _IO_CONNECT_RET_LINK));
}

/* Debug: a connect message's fields, and its extra as text if it has one. */
static void log_connect(const char *op, const struct _io_connect *c, const void *extra)
{
    if (!fs9p_fs.debug)
        return;
    fprintf(stderr,
            "fs-9p: %s \"%s\" subtype %u ioflag 0x%x mode 0%o eflag 0x%x extra_type %u "
            "extra_len %u",
            op, c->path, (unsigned)c->subtype, (unsigned)c->ioflag, (unsigned)c->mode,
            (unsigned)c->eflag, (unsigned)c->extra_type, (unsigned)c->extra_len);
    if (extra != NULL && c->extra_len > 0)
        fprintf(stderr, " extra \"%.*s\"", (int)c->extra_len, (const char *)extra);
    fputc('\n', stderr);
}

/* Tlopen/Tlcreate flags for QNX ioflags (open's oflag + 1). The O_* values
 * differ from Linux's, so each one is mapped. */
static uint32_t dotl_open_flags(uint32_t ioflag)
{
    uint32_t f;
    switch (ioflag & (_IO_FLAG_RD | _IO_FLAG_WR)) {
    case _IO_FLAG_WR:
        f = P9_DOTL_WRONLY;
        break;
    case _IO_FLAG_RD | _IO_FLAG_WR:
        f = P9_DOTL_RDWR;
        break;
    default:
        f = P9_DOTL_RDONLY;
        break;
    }
    if (ioflag & _IO_FLAG_WR) {
        if (ioflag & O_TRUNC)
            f |= P9_DOTL_TRUNC;
        /* The host fd appends, so concurrent appenders on the host and in
         * the guest don't overwrite each other. */
        if (ioflag & O_APPEND)
            f |= P9_DOTL_APPEND;
        if (ioflag & O_SYNC)
            f |= P9_DOTL_SYNC;
        else if (ioflag & O_DSYNC)
            f |= P9_DOTL_DSYNC;
    }
    return f;
}

/* The client's credentials, with supplementary groups for group checks. */
static int client_info(resmgr_context_t *ctp, uint32_t ioflag, struct _client_info **info)
{
    *info = NULL;
    return iofunc_client_info_ext(ctp, (int)ioflag, info, IOFUNC_CLIENTINFO_GETGROUPS);
}

static void client_info_free(struct _client_info **info)
{
    if (*info != NULL)
        (void)iofunc_client_info_ext_free(info);
}

/* Split "a/b/c" into dir "a/b" and name "c" ("c" -> "" and "c"). */
static int split_parent(const char *path, char dir[PATH_MAX], const char **name)
{
    const char *slash = strrchr(path, '/');
    size_t dirlen = slash ? (size_t)(slash - path) : 0;
    *name = slash ? slash + 1 : path;
    if (**name == '\0')
        return EINVAL;
    if (dirlen >= PATH_MAX)
        return ENAMETOOLONG;
    memcpy(dir, path, dirlen);
    dir[dirlen] = '\0';
    return EOK;
}

/* The directory a change happens in, walked and checked. */
typedef struct {
    uint32_t fid;
    iofunc_attr_t attr;
    const char *name; /* last component of the path, inside the caller's string */
    char path[PATH_MAX];
} fs9p_dir_t;

/*
 * Walk the directory holding `path` and check that the client may write and
 * search it. A symlink on the way (within the directory part, or as its
 * last component) can't be crossed by the server: for paths from the
 * connect message the client is redirected, exactly as for open, and
 * _RESMGR_NOREPLY comes back (the reply is sent). Paths in a message's
 * extra (rename's old path, a hard link's source) can't be redirected and
 * get EXDEV, but the C library resolves them through us before sending
 * the request (observed), so that is only a safety net. On success d->fid
 * must be clunked.
 */
static int open_parent(resmgr_context_t *ctp, io_open_t *msg, const char *path, bool redirect,
                       const struct _client_info *info, fs9p_dir_t *d)
{
    int rc = split_parent(path, d->path, &d->name);
    if (rc != EOK)
        return rc;
    size_t link_end = 0;
    rc = walk_as(ctp, info, d->path, &d->fid, &link_end);
    if (rc == ENOTSUP)
        return redirect ? redirect_link(ctp, msg, path, link_end) : EXDEV;
    if (rc != EOK)
        return rc;
    p9_attr_t a;
    rc = -fs9p_getattr(fs9p_fs.sess, d->fid, P9_GETATTR_BASIC, &a);
    if (rc == EOK) {
        iofunc_attr_init(&d->attr, S_IFDIR, NULL, NULL);
        d->attr.mount = &fs9p_fs.mount;
        fs9p_attr_fill(&d->attr, &a);
        if (S_ISLNK(d->attr.mode)) {
            (void)fs9p_clunk(fs9p_fs.sess, d->fid);
            /* The link is the directory part's last component. */
            return redirect ? redirect_link(ctp, msg, path, strlen(d->path)) : EXDEV;
        }
        if (!S_ISDIR(d->attr.mode))
            rc = ENOTDIR;
    }
    /* S_IWRITE | S_IEXEC asks "may write and search" for the client's class
     * (owner, group, other); root passes (verified: owner, root, other). */
    if (rc == EOK)
        rc = iofunc_check_access(ctp, &d->attr, S_IWRITE | S_IEXEC, info);
    if (rc != EOK)
        (void)fs9p_clunk(fs9p_fs.sess, d->fid);
    return rc;
}

/* In a sticky directory (S_ISVTX, like /tmp) only root, the directory's
 * owner and the entry's owner may remove or rename the entry. */
static int sticky_ok(const fs9p_dir_t *d, const char *path, const struct _client_info *info)
{
    uid_t euid = info->cred.euid;
    if (!(d->attr.mode & S_ISVTX) || euid == 0 || euid == d->attr.uid)
        return EOK;
    uint32_t fid;
    int rc = -fs9p_walk(fs9p_fs.sess, path, &fid, NULL);
    if (rc != EOK)
        return rc;
    p9_attr_t a;
    rc = -fs9p_getattr(fs9p_fs.sess, fid, P9_GETATTR_UID, &a);
    (void)fs9p_clunk(fs9p_fs.sess, fid);
    if (rc == EOK && a.uid != (uint32_t)euid)
        rc = EPERM;
    return rc;
}

/*
 * fs-9p attaches as uid 0, so the server records new files as root's (or,
 * with security_model=none, as the host user's). Give a non-root creator
 * ownership, as a local filesystem would. Best effort: with
 * security_model=none QEMU can't chown and reports it for Tsetattr.
 */
static void set_owner(uint32_t fid, const struct _client_info *info)
{
    if (info->cred.euid == 0)
        return;
    p9_setattr_t sa = {0};
    sa.valid = P9_SETATTR_UID | P9_SETATTR_GID;
    sa.uid = (uint32_t)info->cred.euid;
    sa.gid = (uint32_t)info->cred.egid;
    int rc = fs9p_setattr(fs9p_fs.sess, fid, &sa);
    if (rc != 0 && fs9p_fs.debug)
        fprintf(stderr, "fs-9p: owner of new fid %u: %s\n", fid, strerror(-rc));
}

/* A node for `path`, open on fid (P9_NOFID: not walked yet); its OCB is
 * attached by the caller. */
static fs9p_node_t *node_new(uint32_t fid, const p9_attr_t *a, const char *path)
{
    /* Per-open allocation: the number of open files is unbounded, and each
     * open needs its own fid and attribute. Freed with its last OCB. */
    fs9p_node_t *node = calloc(1, sizeof *node);
    if (node == NULL)
        return NULL;
    node->path = strdup(path);
    if (node->path == NULL) {
        free(node);
        return NULL;
    }
    iofunc_attr_init(&node->attr, S_IFREG, NULL, NULL);
    node->attr.mount = &fs9p_fs.mount;
    node->fid = fid;
    fs9p_attr_fill(&node->attr, a);
    return node;
}

/* Free a node that never got an OCB (its fid is the caller's). */
static void node_discard(fs9p_node_t *node)
{
    if (node != NULL)
        free(node->path);
    free(node);
}

/* open(O_CREAT) of a missing file: Tlcreate in its directory. */
static int create_file(resmgr_context_t *ctp, io_open_t *msg, const char *path,
                       const struct _client_info *info)
{
    uint32_t ioflag = msg->connect.ioflag;
    static _Thread_local fs9p_dir_t d;
    int rc = open_parent(ctp, msg, path, true, info, &d);
    if (rc != EOK)
        return rc; /* including _RESMGR_NOREPLY after a redirect */
    uint32_t fid = d.fid;
    uint32_t iounit = 0;
    uint32_t flags = dotl_open_flags(ioflag) | P9_DOTL_CREATE;
    if (ioflag & O_EXCL)
        flags |= P9_DOTL_EXCL;
    /* On success fid refers to the new, open file. */
    rc = -fs9p_lcreate(fs9p_fs.sess, fid, d.name, flags, msg->connect.mode & 07777u,
                       (uint32_t)info->cred.egid, &iounit);
    if (rc == EOK)
        set_owner(fid, info);
    p9_attr_t a;
    if (rc == EOK)
        rc = -fs9p_getattr(fs9p_fs.sess, fid, P9_GETATTR_BASIC, &a);
    if (rc == EOK) {
        fs9p_cache_put(path, &a); /* replaces a "missing" entry */
        fs9p_cache_forget_parent(path);
    }
    fs9p_node_t *node = NULL;
    if (rc == EOK && (node = node_new(fid, &a, path)) == NULL)
        rc = ENOMEM;
    /* No permission check against the new file's mode: open(O_CREAT|O_WRONLY,
     * 0444) must succeed for the creator. */
    if (rc == EOK) {
        node->opened = true;
        node->iounit = iounit;
        rc = iofunc_ocb_attach(ctp, msg, NULL, &node->attr, NULL);
    }
    if (rc != EOK) {
        (void)fs9p_node_released();
        node_discard(node);
        (void)fs9p_clunk(fs9p_fs.sess, fid);
    }
    return rc;
}

/* Search permission on every directory above path, from cached attributes
 * only: true if all of them are cached and searchable (always for root). */
static bool cached_search_ok(resmgr_context_t *ctp, const struct _client_info *info,
                             const char *path)
{
    if (info->cred.euid == 0 || path[0] == '\0')
        return true;
    char dir[PATH_MAX];
    size_t n = 0; /* dir is path[0..n): "" first, then each prefix before a '/' */
    for (;;) {
        if (n >= sizeof dir)
            return false;
        memcpy(dir, path, n);
        dir[n] = '\0';
        bool exists;
        p9_attr_t a;
        if (!fs9p_cache_get(dir, &exists, &a) || !exists)
            return false;
        iofunc_attr_t dattr;
        iofunc_attr_init(&dattr, S_IFDIR, NULL, NULL);
        dattr.mount = &fs9p_fs.mount;
        fs9p_attr_fill(&dattr, &a);
        if (!S_ISDIR(dattr.mode) || iofunc_check_access(ctp, &dattr, S_IEXEC, info) != EOK)
            return false;
        const char *slash = strchr(path + n + (n > 0), '/');
        if (slash == NULL)
            return true;
        n = (size_t)(slash - path);
    }
}

/* A stat()-like open answered from the cache: a node with no fid (walked
 * later only if something needs one) and no request at all. */
static int open_cached(resmgr_context_t *ctp, io_open_t *msg, const struct _client_info *info,
                       const p9_attr_t *a)
{
    const char *path = msg->connect.path;
    fs9p_node_t *node = node_new(P9_NOFID, a, path);
    if (node == NULL)
        return ENOMEM;
    int rc = EOK;
    if (!S_ISDIR(node->attr.mode) &&
        ((msg->connect.eflag & _IO_CONNECT_EFLAG_DIR) || (msg->connect.ioflag & O_DIRECTORY)))
        rc = ENOTDIR;
    if (rc == EOK)
        rc = iofunc_open(ctp, msg, &node->attr, NULL, (struct _client_info *)info);
    if (rc == EOK)
        rc = iofunc_ocb_attach(ctp, msg, NULL, &node->attr, NULL);
    if (rc == EOK) {
        node->fresh = ctp->rcvid;
    } else {
        (void)fs9p_node_released();
        node_discard(node);
    }
    dbg("open (cached)", path, rc);
    return rc;
}

/* The open itself, for a client whose credentials are `info`. */
static int open_node(resmgr_context_t *ctp, io_open_t *msg, const struct _client_info *info)
{
    const char *path = msg->connect.path;
    uint32_t ioflag = msg->connect.ioflag;
    bool wr = (ioflag & _IO_FLAG_WR) != 0;
    /* Last component a symlink: follow it unless the caller asked not to.
     * lstat() asks with connect.mode == S_IFLNK (observed: lstat sends mode
     * 0120000, stat() and open() send 0); open(O_NOFOLLOW) with ioflag. */
    bool nofollow = (msg->connect.mode & S_IFMT) == S_IFLNK || (ioflag & O_NOFOLLOW);

    /* Metadata cache (cache=MS): a path known to be missing fails at once,
     * and a stat()-like open (a combine message with no read or write
     * access) of a cached path needs no request. */
    bool exists = false;
    p9_attr_t ca;
    if (fs9p_cache_get(path, &exists, &ca) && cached_search_ok(ctp, info, path)) {
        if (!exists && !(ioflag & O_CREAT))
            return ENOENT;
        bool statlike = msg->connect.subtype != _IO_CONNECT_OPEN &&
                        !(ioflag & (_IO_FLAG_RD | _IO_FLAG_WR | O_TRUNC | O_CREAT));
        bool follow = (ca.mode & 0170000u) == 0120000u && !nofollow;
        if (exists && statlike && !follow)
            return open_cached(ctp, msg, info, &ca);
    }

    uint32_t fid;
    size_t link_end = 0;
    int rc = walk_as(ctp, info, path, &fid, &link_end);
    if (rc == ENOTSUP)
        return redirect_link(ctp, msg, path, link_end); /* symlink mid-path */
    if (rc == ENOENT && !(ioflag & O_CREAT))
        fs9p_cache_put_missing(path);
    if (rc == ENOENT && (ioflag & O_CREAT)) {
        rc = fs9p_fs.ro ? EROFS : create_file(ctp, msg, path, info);
        dbg("create", path, rc);
        return rc;
    }
    if (rc != EOK) {
        dbg("open", path, rc);
        return rc;
    }

    p9_attr_t a;
    rc = -fs9p_getattr(fs9p_fs.sess, fid, P9_GETATTR_BASIC, &a);
    if (rc == EOK)
        fs9p_cache_put(path, &a);
    if (rc == EOK && (a.mode & 0170000u) == 0120000u && !nofollow) {
        (void)fs9p_clunk(fs9p_fs.sess, fid);
        return redirect_link(ctp, msg, path, strlen(path));
    }
    if (rc == EOK && (ioflag & O_CREAT) && (ioflag & O_EXCL))
        rc = EEXIST;
    fs9p_node_t *node = NULL;
    if (rc == EOK && (node = node_new(fid, &a, path)) == NULL)
        rc = ENOMEM;
    if (rc == EOK) {
        bool dir = S_ISDIR(node->attr.mode);
        if (!dir && ((msg->connect.eflag & _IO_CONNECT_EFLAG_DIR) || (ioflag & O_DIRECTORY)))
            rc = ENOTDIR;
        else if (dir && wr)
            rc = EISDIR;
    }
    if (rc == EOK)
        rc = iofunc_open(ctp, msg, &node->attr, NULL, (struct _client_info *)info);
    /* A real open() (not the combine message behind stat()) opens the file
     * on the server now, after the permission check: O_TRUNC must take
     * effect at open, and an open file must stay usable after another
     * process removes it, while the server can still reach it through
     * its open fd. stat() and the like stay lazy (Tlopen on first read)
     * and cost no server open. */
    bool now = wr || (msg->connect.subtype == _IO_CONNECT_OPEN && (ioflag & _IO_FLAG_RD));
    if (rc == EOK && now) {
        uint32_t flags = dotl_open_flags(ioflag);
        if (S_ISDIR(node->attr.mode))
            flags |= P9_DOTL_DIRECTORY;
        rc = -fs9p_lopen(fs9p_fs.sess, fid, flags, &node->iounit);
        node->opened = rc == EOK;
        if (rc == EOK && (ioflag & O_TRUNC))
            node->attr.nbytes = 0;
    }
    if (rc == EOK)
        rc = iofunc_ocb_attach(ctp, msg, NULL, &node->attr, NULL);
    /* stat() is an open, a stat and a close in one combine message: the
     * stat can use what this open just fetched (see fs9p_stat_h). */
    if (rc == EOK)
        node->fresh = ctp->rcvid;
    if (rc != EOK) {
        /* No OCB is left on the node. If iofunc allocated one and freed it
         * again, drop its hand-off: the node is freed here, once. */
        (void)fs9p_node_released();
        node_discard(node);
        (void)fs9p_clunk(fs9p_fs.sess, fid);
    }
    dbg("open", path, rc);
    return rc;
}

static int fs9p_open_h(resmgr_context_t *ctp, io_open_t *msg, RESMGR_HANDLE_T *handle, void *extra)
{
    (void)handle;
    (void)extra;
    uint32_t ioflag = msg->connect.ioflag;
    if (fs9p_fs.ro && ((ioflag & _IO_FLAG_WR) || (ioflag & O_TRUNC))) {
        dbg("open", msg->connect.path, EROFS);
        return EROFS;
    }
    log_connect("open", &msg->connect, NULL);
    struct _client_info *info = NULL;
    int rc = client_info(ctp, ioflag, &info);
    if (rc == EOK)
        rc = open_node(ctp, msg, info);
    client_info_free(&info);
    return rc;
}

static int fs9p_readlink_h(resmgr_context_t *ctp, io_readlink_t *msg, RESMGR_HANDLE_T *handle,
                           void *reserved)
{
    (void)handle;
    (void)reserved;
    const char *path = msg->connect.path;
    static _Thread_local struct link_buf lb; /* PATH_MAX: keep it off the stack */
    struct _client_info *info = NULL;
    int rc = client_info(ctp, msg->connect.ioflag, &info);
    if (rc == EOK)
        rc = read_link(ctp, info, path, strlen(path), &lb);
    client_info_free(&info);
    dbg("readlink", path, rc);
    if (rc != EOK)
        return rc;
    /* io_readlink_t and io_open_t are the same union layout. */
    return reply_link(ctp, (io_open_t *)msg, lb.data, lb.len, (long)lb.len);
}

/* New directories and symlinks belong to their creator (see set_owner()). */
static void own_new(const char *path, const struct _client_info *info)
{
    if (info->cred.euid == 0)
        return;
    uint32_t fid;
    if (fs9p_walk(fs9p_fs.sess, path, &fid, NULL) == 0) {
        set_owner(fid, info);
        (void)fs9p_clunk(fs9p_fs.sess, fid);
    }
}

/* Common start of the namespace handlers: refuse what can't apply, and get
 * the client's credentials. `creates`: the operation makes a new name. */
static int ns_begin(resmgr_context_t *ctp, const struct _io_connect *c, bool creates,
                    struct _client_info **info)
{
    *info = NULL;
    if (fs9p_fs.ro)
        return EROFS;
    /* The mount point itself: it exists (mkdir -p relies on EEXIST), and it
     * can't be removed or renamed. */
    if (c->path[0] == '\0')
        return creates ? EEXIST : EBUSY;
    if (c->eflag & (_IO_CONNECT_EFLAG_DOT | _IO_CONNECT_EFLAG_DOTDOT))
        return EINVAL; /* "dir/." or "dir/.." as the last component */
    return client_info(ctp, c->ioflag, info);
}

/* unlink() and rmdir(): rmdir sends mode S_IFDIR, unlink S_IFLNK ("don't
 * follow"), both observed. */
static int fs9p_unlink_h(resmgr_context_t *ctp, io_unlink_t *msg, RESMGR_HANDLE_T *handle,
                         void *reserved)
{
    (void)handle, (void)reserved;
    const char *path = msg->connect.path;
    log_connect("unlink", &msg->connect, NULL);
    struct _client_info *info;
    static _Thread_local fs9p_dir_t d;
    int rc = ns_begin(ctp, &msg->connect, false, &info);
    if (rc == EOK)
        rc = open_parent(ctp, (io_open_t *)msg, path, true, info, &d);
    if (rc == EOK) {
        rc = sticky_ok(&d, path, info);
        bool dir = (msg->connect.mode & S_IFMT) == S_IFDIR;
        if (rc == EOK)
            rc = -fs9p_unlinkat(fs9p_fs.sess, d.fid, d.name, dir ? P9_DOTL_AT_REMOVEDIR : 0);
        (void)fs9p_clunk(fs9p_fs.sess, d.fid);
        if (rc == EOK) {
            fs9p_cache_forget_tree(path);
            fs9p_cache_forget_parent(path);
        }
    }
    client_info_free(&info);
    dbg("unlink", path, rc);
    return rc;
}

/* rename(): the connect path is the new name, the extra the old one, both
 * relative to the mount (observed, also for relative paths in the call). */
static int fs9p_rename_h(resmgr_context_t *ctp, io_rename_t *msg, RESMGR_HANDLE_T *handle,
                         io_rename_extra_t *extra)
{
    (void)handle;
    const char *path = msg->connect.path;
    log_connect("rename", &msg->connect, extra);
    struct _client_info *info;
    static _Thread_local fs9p_dir_t from, to;
    static _Thread_local char old[PATH_MAX];
    int rc = ns_begin(ctp, &msg->connect, false, &info);
    if (rc == EOK && msg->connect.extra_type != _IO_CONNECT_EXTRA_RENAME)
        rc = ENOSYS;
    if (rc == EOK) {
        size_t n = strnlen(extra->path, msg->connect.extra_len);
        if (n == 0 || n >= sizeof old)
            rc = n == 0 ? EINVAL : ENAMETOOLONG;
        else {
            memcpy(old, extra->path, n);
            old[n] = '\0';
        }
    }
    if (rc == EOK)
        rc = open_parent(ctp, (io_open_t *)msg, path, true, info, &to);
    if (rc == EOK) {
        rc = open_parent(ctp, (io_open_t *)msg, old, false, info, &from);
        if (rc == EOK) {
            rc = sticky_ok(&from, old, info);
            if (rc == EOK)
                rc = -fs9p_renameat(fs9p_fs.sess, from.fid, from.name, to.fid, to.name);
            (void)fs9p_clunk(fs9p_fs.sess, from.fid);
            if (rc == EOK) {
                fs9p_cache_forget_tree(old);
                fs9p_cache_forget_tree(path);
                fs9p_cache_forget_parent(old);
                fs9p_cache_forget_parent(path);
            }
        }
        (void)fs9p_clunk(fs9p_fs.sess, to.fid);
    }
    client_info_free(&info);
    dbg("rename", path, rc);
    return rc;
}

/* mkdir() arrives as mknod with S_IFDIR. Other special files (FIFOs,
 * device nodes) aren't created: on a host share they would mean nothing
 * to QNX. */
static int fs9p_mknod_h(resmgr_context_t *ctp, io_mknod_t *msg, RESMGR_HANDLE_T *handle,
                        void *reserved)
{
    (void)handle, (void)reserved;
    const char *path = msg->connect.path;
    log_connect("mknod", &msg->connect, NULL);
    struct _client_info *info;
    static _Thread_local fs9p_dir_t d;
    int rc = ns_begin(ctp, &msg->connect, true, &info);
    if (rc == EOK && (msg->connect.mode & S_IFMT) != S_IFDIR)
        rc = ENOTSUP;
    if (rc == EOK)
        rc = open_parent(ctp, (io_open_t *)msg, path, true, info, &d);
    if (rc == EOK) {
        rc = -fs9p_mkdir(fs9p_fs.sess, d.fid, d.name, msg->connect.mode & 07777u,
                         (uint32_t)info->cred.egid);
        (void)fs9p_clunk(fs9p_fs.sess, d.fid);
        if (rc == EOK) {
            fs9p_cache_forget(path);
            fs9p_cache_forget_parent(path);
        }
        if (rc == EOK)
            own_new(path, info);
    }
    client_info_free(&info);
    dbg("mkdir", path, rc);
    return rc;
}

/* symlink() (extra = the target, as written) and link() (extra = the
 * existing file, relative to the mount); observed. */
static int fs9p_link_h(resmgr_context_t *ctp, io_link_t *msg, RESMGR_HANDLE_T *handle,
                       io_link_extra_t *extra)
{
    (void)handle;
    const char *path = msg->connect.path;
    log_connect("link", &msg->connect, extra);
    struct _client_info *info;
    static _Thread_local fs9p_dir_t d;
    static _Thread_local char other[PATH_MAX];
    uint8_t type = msg->connect.extra_type;
    int rc = ns_begin(ctp, &msg->connect, true, &info);
    if (rc == EOK && type != _IO_CONNECT_EXTRA_SYMLINK && type != _IO_CONNECT_EXTRA_LINK)
        rc = ENOSYS;
    if (rc == EOK) {
        size_t n = strnlen(extra->path, msg->connect.extra_len);
        if (n == 0 || n >= sizeof other)
            rc = n == 0 ? ENOENT : ENAMETOOLONG;
        else {
            memcpy(other, extra->path, n);
            other[n] = '\0';
        }
    }
    if (rc == EOK)
        rc = open_parent(ctp, (io_open_t *)msg, path, true, info, &d);
    if (rc == EOK) {
        if (type == _IO_CONNECT_EXTRA_SYMLINK) {
            rc = -fs9p_symlink(fs9p_fs.sess, d.fid, d.name, other, (uint32_t)info->cred.egid);
            if (rc == EOK)
                own_new(path, info);
        } else {
            uint32_t src;
            rc = walk_as(ctp, info, other, &src, NULL);
            if (rc == ENOTSUP)
                rc = EXDEV; /* can't redirect; libc resolves the source first */
            if (rc == EOK) {
                rc = -fs9p_link(fs9p_fs.sess, d.fid, src, d.name);
                (void)fs9p_clunk(fs9p_fs.sess, src);
            }
        }
        (void)fs9p_clunk(fs9p_fs.sess, d.fid);
        if (rc == EOK) {
            fs9p_cache_forget(path);
            fs9p_cache_forget_parent(path);
            if (type == _IO_CONNECT_EXTRA_LINK)
                fs9p_cache_forget(other); /* its link count changed */
        }
    }
    client_info_free(&info);
    dbg(type == _IO_CONNECT_EXTRA_SYMLINK ? "symlink" : "link", path, rc);
    return rc;
}

void fs9p_connect_init(resmgr_connect_funcs_t *c)
{
    c->open = fs9p_open_h;
    c->readlink = fs9p_readlink_h;
    c->unlink = fs9p_unlink_h;
    c->rename = fs9p_rename_h;
    c->mknod = fs9p_mknod_h;
    c->link = fs9p_link_h;
}
