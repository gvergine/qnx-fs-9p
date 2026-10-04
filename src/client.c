/* fs-9p client session: synchronous 9P2000.L calls over a transport. */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "client.h"
#include "errno_map.h"

/* Each request in flight uses its slot's tag, so tags are unique among
 * outstanding requests. Tversion must use P9_NOTAG. */
#define TAG(slot) ((uint16_t)((slot) + 1u))

static void hexdump(const char *dir, const uint8_t *p, size_t n)
{
    const char *name = n > 4 ? p9_msg_name(p[4]) : NULL;
    fprintf(stderr, "fs-9p: %s %s (%zu bytes)", dir, name ? name : "?", n);
    size_t show = n < 64 ? n : 64;
    for (size_t i = 0; i < show; i++)
        fprintf(stderr, "%s%02x", (i % 16) ? " " : "\n  ", p[i]);
    fprintf(stderr, "%s\n", show < n ? " ..." : "");
}

/*
 * Send the request the caller encoded into the transport buffer and return
 * the reply. Translates transport failures and Rlerror into -errno, and
 * checks that the reply has the expected type and tag. Caller holds the
 * slot.
 */
static int call(fs9p_session_t *s, unsigned slot, int32_t enc, uint8_t rtype, uint16_t tag,
                const uint8_t **resp, size_t *rlen)
{
    if (enc < 0)
        return enc == P9_E_NOSPACE ? -E2BIG : -EINVAL;
    size_t cap;
    const uint8_t *req = s->tp.ops->buf(s->tp.impl, slot, &cap);
    if (s->debug)
        hexdump("->", req, (size_t)enc);

    int rc = s->tp.ops->rpc(s->tp.impl, slot, (size_t)enc, resp, rlen);
    if (rc != 0)
        return rc;

    p9_hdr_t h;
    if (p9_dec_hdr(*resp, *rlen, &h) != P9_OK)
        return -EIO;
    if (s->debug)
        hexdump("<-", *resp, h.size);
    if (h.tag != tag)
        return -EIO;
    if (h.type == P9_RLERROR) {
        uint32_t ecode;
        if (p9_dec_rlerror(*resp, *rlen, &ecode) != P9_OK)
            return -EIO;
        return -fs9p_errno_from_linux(ecode);
    }
    return h.type == rtype ? 0 : -EIO;
}

static uint8_t *req_buf(fs9p_session_t *s, unsigned slot, size_t *cap)
{
    uint8_t *b = s->tp.ops->buf(s->tp.impl, slot, cap);
    /* Never encode more than the negotiated msize. */
    if (s->msize != 0 && *cap > s->msize)
        *cap = s->msize;
    return b;
}

static int clunk_on(fs9p_session_t *s, unsigned sl, uint32_t fid);

static int slot_get(fs9p_session_t *s, unsigned *slot)
{
    int rc = s->tp.ops->get(s->tp.impl, slot);
    if (rc != 0)
        return rc;
    /* Catch up on a clunk that couldn't get a slot earlier. */
    pthread_mutex_lock(&s->fid_lock);
    bool have = s->ndeferred > 0;
    uint32_t fid = have ? s->deferred[--s->ndeferred] : 0;
    pthread_mutex_unlock(&s->fid_lock);
    if (have)
        (void)clunk_on(s, *slot, fid);
    return 0;
}

static void slot_put(fs9p_session_t *s, unsigned slot)
{
    s->tp.ops->put(s->tp.impl, slot);
}

static int fid_alloc(fs9p_session_t *s, uint32_t *fid)
{
    pthread_mutex_lock(&s->fid_lock);
    int rc = p9_idpool_alloc(&s->fids, fid) == P9_OK ? 0 : -ENFILE;
    pthread_mutex_unlock(&s->fid_lock);
    return rc;
}

static void fid_free(fs9p_session_t *s, uint32_t fid)
{
    pthread_mutex_lock(&s->fid_lock);
    (void)p9_idpool_free(&s->fids, fid);
    pthread_mutex_unlock(&s->fid_lock);
}

static int do_version(fs9p_session_t *s, unsigned sl, uint32_t want)
{
    size_t cap;
    uint8_t *b = req_buf(s, sl, &cap);
    const uint8_t *r;
    size_t rlen;
    int rc = call(s, sl, p9_enc_tversion(b, cap, P9_NOTAG, want, p9_str(P9_PROTO_VERSION)),
                  P9_RVERSION, P9_NOTAG, &r, &rlen);
    if (rc != 0)
        return rc;
    uint32_t msize;
    p9_str_t ver;
    if (p9_dec_rversion(r, rlen, &msize, &ver) != P9_OK)
        return -EIO;
    /* The server answers "unknown" or another dialect if it can't do .L. */
    if (ver.len != strlen(P9_PROTO_VERSION) || memcmp(ver.ptr, P9_PROTO_VERSION, ver.len) != 0)
        return -EPROTONOSUPPORT;
    /* It may lower msize, never raise it; anything below a useful minimum
     * (header + a few hundred bytes) is unusable. */
    if (msize > want || msize < 512)
        return -EPROTO;
    s->msize = msize;
    return 0;
}

static int do_attach(fs9p_session_t *s, unsigned sl)
{
    uint32_t fid;
    int rc = fid_alloc(s, &fid);
    if (rc != 0)
        return rc;
    size_t cap;
    uint8_t *b = req_buf(s, sl, &cap);
    const uint8_t *r;
    size_t rlen;
    /* uname "" and n_uname 0: with security_model=mapped-xattr / none QEMU
     * ignores the identity; new files get their owner with a Tsetattr. */
    rc = call(s, sl, p9_enc_tattach(b, cap, TAG(sl), fid, P9_NOFID, p9_str(""), p9_str(""), 0),
              P9_RATTACH, TAG(sl), &r, &rlen);
    if (rc == 0 && p9_dec_rattach(r, rlen, &s->root_qid) != P9_OK)
        rc = -EIO;
    if (rc != 0) {
        fid_free(s, fid);
        return rc;
    }
    s->root_fid = fid;
    return 0;
}

int fs9p_session_start(fs9p_session_t *s, fs9p_transport_t tp, uint32_t msize, int debug)
{
    memset(s, 0, sizeof *s);
    s->tp = tp;
    s->debug = debug;
    s->root_fid = P9_NOFID;
    int rc = pthread_mutex_init(&s->fid_lock, NULL);
    if (rc != 0)
        return -rc;
    p9_idpool_init(&s->fids, s->fid_words, FS9P_MAX_FIDS);

    uint32_t tmax = tp.ops->max_msize(tp.impl);
    uint32_t want = msize < tmax ? msize : tmax;
    unsigned sl;
    rc = slot_get(s, &sl);
    if (rc != 0)
        return rc;
    rc = do_version(s, sl, want);
    if (rc == 0)
        rc = do_attach(s, sl);
    slot_put(s, sl);
    return rc;
}

/* Tclunk on a slot the caller holds. The fid is gone on the server even
 * when Tclunk fails (9P semantics), so it goes back to the pool, unless the
 * request timed out: then the server may not have seen it yet, and the fid
 * stays allocated (quarantined) so it is never handed out twice. */
static int clunk_on(fs9p_session_t *s, unsigned sl, uint32_t fid)
{
    size_t cap;
    uint8_t *b = req_buf(s, sl, &cap);
    const uint8_t *r;
    size_t rlen;
    int rc = call(s, sl, p9_enc_tclunk(b, cap, TAG(sl), fid), P9_RCLUNK, TAG(sl), &r, &rlen);
    if (rc == 0 && p9_dec_rclunk(r, rlen) != P9_OK)
        rc = -EIO;
    if (rc != -ETIMEDOUT)
        fid_free(s, fid);
    return rc;
}

int fs9p_clunk(fs9p_session_t *s, uint32_t fid)
{
    unsigned sl;
    int rc = slot_get(s, &sl);
    if (rc == -ETIMEDOUT) {
        /* Every slot is held by a request the device hasn't finished: don't
         * keep the caller (a close) waiting; clunk it later. */
        pthread_mutex_lock(&s->fid_lock);
        if (s->ndeferred < FS9P_MAX_FIDS)
            s->deferred[s->ndeferred++] = fid;
        pthread_mutex_unlock(&s->fid_lock);
        return 0;
    }
    if (rc != 0)
        return rc;
    rc = clunk_on(s, sl, fid);
    slot_put(s, sl);
    return rc;
}

void fs9p_session_stop(fs9p_session_t *s)
{
    /* Taking every slot waits for the requests in flight; they are never
     * given back (the process is about to exit). A slot still held by a
     * timed-out request the device hasn't finished can't be had (get()
     * gives up after the request timeout): close() then resets the device,
     * and QEMU's reset waits for its requests to finish, so nothing is
     * written into our memory afterwards. */
    unsigned n = s->tp.ops->slots(s->tp.impl), got = 0, sl = 0;
    while (got < n && slot_get(s, &sl) == 0)
        got++;
    if (got > 0 && s->root_fid != P9_NOFID) {
        (void)clunk_on(s, sl, s->root_fid);
        s->root_fid = P9_NOFID;
    }
    s->tp.ops->close(s->tp.impl);
}

uint32_t fs9p_io_max(const fs9p_session_t *s)
{
    return s->msize - P9_RREAD_OVERHEAD;
}

/* Split up to P9_MAXWELEM components of `*path` into names[]; advances *path
 * past them. Components are views into the caller's string. */
static int split_path(const char **path, p9_str_t *names, uint16_t *n)
{
    const char *p = *path;
    *n = 0;
    while (*p != '\0' && *n < P9_MAXWELEM) {
        while (*p == '/')
            p++;
        if (*p == '\0')
            break;
        const char *end = strchr(p, '/');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len > 255)
            return -ENAMETOOLONG;
        names[*n].ptr = p;
        names[*n].len = len;
        (*n)++;
        p += len;
    }
    while (*p == '/')
        p++;
    *path = p;
    return 0;
}

/* Next single component of `*path`, as split_path() with a limit of one. */
static int split_path_one(const char **path, p9_str_t *name, uint16_t *n)
{
    const char *p = *path;
    while (*p == '/')
        p++;
    const char *end = strchr(p, '/');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    *n = 0;
    if (len == 0) {
        *path = p;
        return 0;
    }
    if (len > 255)
        return -ENAMETOOLONG;
    name->ptr = p;
    name->len = len;
    *n = 1;
    p += len;
    while (*p == '/')
        p++;
    *path = p;
    return 0;
}

/*
 * One Twalk of up to P9_MAXWELEM names. 9P semantics (walk(5)): newfid is
 * only created when every name was walked; a partial walk returns the qids
 * walked so far and leaves newfid untouched.
 */
static int walk_once(fs9p_session_t *s, unsigned sl, uint32_t fid, uint32_t newfid, uint16_t n,
                     const p9_str_t *names, uint16_t *walked, p9_qid_t *last)
{
    size_t cap;
    uint8_t *b = req_buf(s, sl, &cap);
    const uint8_t *r;
    size_t rlen;
    int rc = call(s, sl, p9_enc_twalk(b, cap, TAG(sl), fid, newfid, n, names), P9_RWALK, TAG(sl),
                  &r, &rlen);
    if (rc != 0)
        return rc;
    p9_qid_t qids[P9_MAXWELEM];
    if (p9_dec_rwalk(r, rlen, qids, n, walked) != P9_OK)
        return -EIO;
    if (*walked > 0)
        *last = qids[*walked - 1];
    return 0;
}

static int getattr_on(fs9p_session_t *s, unsigned sl, uint32_t fid, uint64_t mask, p9_attr_t *attr)
{
    size_t cap;
    uint8_t *b = req_buf(s, sl, &cap);
    const uint8_t *r;
    size_t rlen;
    int rc =
        call(s, sl, p9_enc_tgetattr(b, cap, TAG(sl), fid, mask), P9_RGETATTR, TAG(sl), &r, &rlen);
    if (rc == 0 && p9_dec_rgetattr(r, rlen, attr) != P9_OK)
        rc = -EIO;
    return rc;
}

int fs9p_walk_check(fs9p_session_t *s, const char *path, uint32_t *newfid, size_t *link_end,
                    fs9p_dir_check_t check, void *arg)
{
    const char *start = path;
    uint32_t nf;
    unsigned sl;
    int grc = fid_alloc(s, &nf);
    if (grc != 0)
        return grc;
    grc = slot_get(s, &sl);
    if (grc != 0) {
        fid_free(s, nf);
        return grc;
    }
    uint32_t from = s->root_fid;
    bool created = false;
    int rc = 0;
    for (;;) {
        p9_str_t name;
        uint16_t n;
        rc = split_path_one(&path, &name, &n);
        if (rc != 0 || n == 0)
            break;
        /* `from` is the directory about to be searched for `name`. */
        p9_attr_t a;
        rc = getattr_on(s, sl, from, P9_GETATTR_BASIC, &a);
        size_t dlen = (size_t)(name.ptr - start);
        while (dlen > 0 && start[dlen - 1] == '/')
            dlen--;
        if (rc == 0)
            rc = check(arg, start, dlen, &a);
        uint16_t walked = 0;
        p9_qid_t last = {0, 0, 0};
        if (rc == 0)
            rc = walk_once(s, sl, from, nf, 1, &name, &walked, &last);
        if (rc == 0 && walked < 1)
            rc = -ENOENT;
        if (rc != 0)
            break;
        created = true;
        from = nf;
        /* A symlink with more of the path behind it: the server won't
         * cross it, the caller resolves it (as in fs9p_walk). */
        if ((last.type & P9_QTSYMLINK) && *path != '\0') {
            rc = -ENOTSUP;
            if (link_end != NULL)
                *link_end = (size_t)(name.ptr + name.len - start);
            break;
        }
    }
    if (rc == 0 && !created) { /* empty path: clone the root */
        uint16_t walked = 0;
        p9_qid_t last;
        rc = walk_once(s, sl, s->root_fid, nf, 0, NULL, &walked, &last);
        created = rc == 0;
    }
    if (rc == -ETIMEDOUT) {
        /* The server may or may not hold nf now: never hand it out again. */
    } else if (rc != 0) {
        if (created)
            (void)clunk_on(s, sl, nf); /* also returns nf to the pool */
        else
            fid_free(s, nf);
    } else {
        *newfid = nf;
    }
    slot_put(s, sl);
    return rc;
}

int fs9p_walk(fs9p_session_t *s, const char *path, uint32_t *newfid, size_t *link_end)
{
    const char *start = path;
    uint32_t nf;
    unsigned sl;
    int grc = fid_alloc(s, &nf);
    if (grc != 0)
        return grc;
    grc = slot_get(s, &sl);
    if (grc != 0) {
        fid_free(s, nf);
        return grc;
    }
    uint32_t from = s->root_fid;
    bool created = false; /* does nf exist on the server yet? */
    int rc;
    do {
        p9_str_t names[P9_MAXWELEM];
        uint16_t n, walked = 0;
        p9_qid_t last = {0, 0, 0};
        rc = split_path(&path, names, &n);
        if (rc != 0)
            break;
        if (n == 0 && created)
            break;
        rc = walk_once(s, sl, from, nf, n, names, &walked, &last);
        if (rc != 0)
            break;
        if (walked < n) {
            /* Stopped early. A symlink at the stopping point means the rest
             * of the path lies behind a link the server won't cross. */
            if (walked > 0 && (last.type & P9_QTSYMLINK)) {
                rc = -ENOTSUP;
                if (link_end != NULL)
                    *link_end = (size_t)(names[walked - 1].ptr + names[walked - 1].len - start);
            } else if (walked > 0 && !(last.type & P9_QTDIR)) {
                /* A partial Rwalk carries no error code; stopping after a
                 * file means the path used it as a directory. */
                rc = -ENOTDIR;
            } else {
                rc = -ENOENT;
            }
            break;
        }
        created = true;
        from = nf; /* walking a fid onto itself is allowed (walk(5)) */
    } while (*path != '\0');

    if (rc == -ETIMEDOUT) {
        /* The server may or may not hold nf now: never hand it out again. */
    } else if (rc != 0) {
        if (created)
            (void)clunk_on(s, sl, nf); /* also returns nf to the pool */
        else
            fid_free(s, nf);
    } else {
        *newfid = nf;
    }
    slot_put(s, sl);
    return rc;
}

int fs9p_getattr(fs9p_session_t *s, uint32_t fid, uint64_t mask, p9_attr_t *attr)
{
    unsigned sl;
    int grc = slot_get(s, &sl);
    if (grc != 0)
        return grc;
    int rc = getattr_on(s, sl, fid, mask, attr);
    slot_put(s, sl);
    return rc;
}

int fs9p_lopen(fs9p_session_t *s, uint32_t fid, uint32_t flags, uint32_t *iounit)
{
    unsigned sl;
    int grc = slot_get(s, &sl);
    if (grc != 0)
        return grc;
    size_t cap;
    uint8_t *b = req_buf(s, sl, &cap);
    const uint8_t *r;
    size_t rlen;
    p9_qid_t qid;
    int rc = call(s, sl, p9_enc_tlopen(b, cap, TAG(sl), fid, flags), P9_RLOPEN, TAG(sl), &r, &rlen);
    if (rc == 0 && p9_dec_rlopen(r, rlen, &qid, iounit) != P9_OK)
        rc = -EIO;
    slot_put(s, sl);
    return rc;
}

int fs9p_statfs(fs9p_session_t *s, uint32_t fid, p9_statfs_t *st)
{
    unsigned sl;
    int grc = slot_get(s, &sl);
    if (grc != 0)
        return grc;
    size_t cap;
    uint8_t *b = req_buf(s, sl, &cap);
    const uint8_t *r;
    size_t rlen;
    int rc = call(s, sl, p9_enc_tstatfs(b, cap, TAG(sl), fid), P9_RSTATFS, TAG(sl), &r, &rlen);
    if (rc == 0 && p9_dec_rstatfs(r, rlen, st) != P9_OK)
        rc = -EIO;
    slot_put(s, sl);
    return rc;
}

/* Shared body of Tread and Treaddir: same request and reply layout. */
static int read_common(fs9p_session_t *s, bool dir, uint32_t fid, uint64_t offset, uint32_t count,
                       fs9p_sink_t sink, void *arg)
{
    if (count > fs9p_io_max(s))
        count = fs9p_io_max(s);
    unsigned sl;
    int grc = slot_get(s, &sl);
    if (grc != 0)
        return grc;
    size_t cap;
    uint8_t *b = req_buf(s, sl, &cap);
    const uint8_t *r;
    size_t rlen;
    int32_t enc = dir ? p9_enc_treaddir(b, cap, TAG(sl), fid, offset, count)
                      : p9_enc_tread(b, cap, TAG(sl), fid, offset, count);
    int rc = call(s, sl, enc, dir ? P9_RREADDIR : P9_RREAD, TAG(sl), &r, &rlen);
    const uint8_t *data;
    uint32_t n = 0;
    if (rc == 0) {
        int drc = dir ? p9_dec_rreaddir(r, rlen, &data, &n) : p9_dec_rread(r, rlen, &data, &n);
        /* The server must not return more than was asked for. */
        if (drc != P9_OK || n > count)
            rc = -EIO;
    }
    if (rc == 0 && n > 0)
        rc = sink(arg, data, n);
    slot_put(s, sl);
    return rc == 0 ? (int)n : rc;
}

int fs9p_read(fs9p_session_t *s, uint32_t fid, uint64_t offset, uint32_t count, fs9p_sink_t sink,
              void *arg)
{
    return read_common(s, false, fid, offset, count, sink, arg);
}

int fs9p_readdir(fs9p_session_t *s, uint32_t fid, uint64_t cookie, uint32_t count, fs9p_sink_t sink,
                 void *arg)
{
    return read_common(s, true, fid, cookie, count, sink, arg);
}

int fs9p_readlink(fs9p_session_t *s, uint32_t fid, fs9p_sink_t sink, void *arg)
{
    unsigned sl;
    int grc = slot_get(s, &sl);
    if (grc != 0)
        return grc;
    size_t cap;
    uint8_t *b = req_buf(s, sl, &cap);
    const uint8_t *r;
    size_t rlen;
    p9_str_t target;
    int rc = call(s, sl, p9_enc_treadlink(b, cap, TAG(sl), fid), P9_RREADLINK, TAG(sl), &r, &rlen);
    if (rc == 0 && p9_dec_rreadlink(r, rlen, &target) != P9_OK)
        rc = -EIO;
    if (rc == 0)
        rc = sink(arg, (const uint8_t *)target.ptr, (uint32_t)target.len);
    slot_put(s, sl);
    return rc;
}

/* --- write path ----------------------------------------------------------------- */

uint32_t fs9p_write_max(const fs9p_session_t *s)
{
    return s->msize - P9_TWRITE_OVERHEAD;
}

int fs9p_write(fs9p_session_t *s, uint32_t fid, uint64_t offset, uint32_t count, fs9p_source_t src,
               void *arg)
{
    if (count > fs9p_write_max(s))
        count = fs9p_write_max(s);
    unsigned sl;
    int grc = slot_get(s, &sl);
    if (grc != 0)
        return grc;
    size_t cap;
    uint8_t *b = req_buf(s, sl, &cap);
    const uint8_t *r;
    size_t rlen;
    uint32_t done = 0;
    /* Payload first, straight into its place after the header. */
    int rc = src(arg, b + P9_TWRITE_OVERHEAD, count);
    if (rc == 0)
        rc = call(s, sl, p9_enc_twrite(b, cap, TAG(sl), fid, offset, count), P9_RWRITE, TAG(sl), &r,
                  &rlen);
    if (rc == 0 && (p9_dec_rwrite(r, rlen, &done) != P9_OK || done > count))
        rc = -EIO;
    slot_put(s, sl);
    return rc == 0 ? (int)done : rc;
}

int fs9p_lcreate(fs9p_session_t *s, uint32_t dirfid, const char *name, uint32_t flags,
                 uint32_t mode, uint32_t gid, uint32_t *iounit)
{
    unsigned sl;
    int grc = slot_get(s, &sl);
    if (grc != 0)
        return grc;
    size_t cap;
    uint8_t *b = req_buf(s, sl, &cap);
    const uint8_t *r;
    size_t rlen;
    p9_qid_t qid;
    int rc = call(s, sl, p9_enc_tlcreate(b, cap, TAG(sl), dirfid, p9_str(name), flags, mode, gid),
                  P9_RLCREATE, TAG(sl), &r, &rlen);
    if (rc == 0 && p9_dec_rlcreate(r, rlen, &qid, iounit) != P9_OK)
        rc = -EIO;
    slot_put(s, sl);
    return rc;
}

int fs9p_setattr(fs9p_session_t *s, uint32_t fid, const p9_setattr_t *attr)
{
    unsigned sl;
    int grc = slot_get(s, &sl);
    if (grc != 0)
        return grc;
    size_t cap;
    uint8_t *b = req_buf(s, sl, &cap);
    const uint8_t *r;
    size_t rlen;
    int rc =
        call(s, sl, p9_enc_tsetattr(b, cap, TAG(sl), fid, attr), P9_RSETATTR, TAG(sl), &r, &rlen);
    if (rc == 0 && p9_dec_rsetattr(r, rlen) != P9_OK)
        rc = -EIO;
    slot_put(s, sl);
    return rc;
}

int fs9p_fsync(fs9p_session_t *s, uint32_t fid, uint32_t datasync)
{
    unsigned sl;
    int grc = slot_get(s, &sl);
    if (grc != 0)
        return grc;
    size_t cap;
    uint8_t *b = req_buf(s, sl, &cap);
    const uint8_t *r;
    size_t rlen;
    int rc =
        call(s, sl, p9_enc_tfsync(b, cap, TAG(sl), fid, datasync), P9_RFSYNC, TAG(sl), &r, &rlen);
    if (rc == 0 && p9_dec_rfsync(r, rlen) != P9_OK)
        rc = -EIO;
    slot_put(s, sl);
    return rc;
}

/* --- namespace ---------------------------------------------------------------- */

/* Shared tail of the namespace calls: send what the caller encoded, then
 * decode a reply that is either empty or a lone qid. */
static int call_simple(fs9p_session_t *s, unsigned sl, int32_t enc, uint8_t rtype)
{
    const uint8_t *r;
    size_t rlen;
    int rc = call(s, sl, enc, rtype, TAG(sl), &r, &rlen);
    if (rc != 0)
        return rc;
    p9_qid_t qid;
    switch (rtype) {
    case P9_RMKDIR:
        return p9_dec_rmkdir(r, rlen, &qid) == P9_OK ? 0 : -EIO;
    case P9_RSYMLINK:
        return p9_dec_rsymlink(r, rlen, &qid) == P9_OK ? 0 : -EIO;
    case P9_RLINK:
        return p9_dec_rlink(r, rlen) == P9_OK ? 0 : -EIO;
    case P9_RRENAMEAT:
        return p9_dec_rrenameat(r, rlen) == P9_OK ? 0 : -EIO;
    case P9_RUNLINKAT:
        return p9_dec_runlinkat(r, rlen) == P9_OK ? 0 : -EIO;
    default:
        return -EIO;
    }
}

int fs9p_mkdir(fs9p_session_t *s, uint32_t dirfid, const char *name, uint32_t mode, uint32_t gid)
{
    unsigned sl;
    int grc = slot_get(s, &sl);
    if (grc != 0)
        return grc;
    size_t cap;
    uint8_t *b = req_buf(s, sl, &cap);
    int rc = call_simple(s, sl, p9_enc_tmkdir(b, cap, TAG(sl), dirfid, p9_str(name), mode, gid),
                         P9_RMKDIR);
    slot_put(s, sl);
    return rc;
}

int fs9p_symlink(fs9p_session_t *s, uint32_t dirfid, const char *name, const char *target,
                 uint32_t gid)
{
    unsigned sl;
    int grc = slot_get(s, &sl);
    if (grc != 0)
        return grc;
    size_t cap;
    uint8_t *b = req_buf(s, sl, &cap);
    int rc = call_simple(
        s, sl, p9_enc_tsymlink(b, cap, TAG(sl), dirfid, p9_str(name), p9_str(target), gid),
        P9_RSYMLINK);
    slot_put(s, sl);
    return rc;
}

int fs9p_link(fs9p_session_t *s, uint32_t dirfid, uint32_t fid, const char *name)
{
    unsigned sl;
    int grc = slot_get(s, &sl);
    if (grc != 0)
        return grc;
    size_t cap;
    uint8_t *b = req_buf(s, sl, &cap);
    int rc = call_simple(s, sl, p9_enc_tlink(b, cap, TAG(sl), dirfid, fid, p9_str(name)), P9_RLINK);
    slot_put(s, sl);
    return rc;
}

int fs9p_renameat(fs9p_session_t *s, uint32_t olddirfid, const char *oldname, uint32_t newdirfid,
                  const char *newname)
{
    unsigned sl;
    int grc = slot_get(s, &sl);
    if (grc != 0)
        return grc;
    size_t cap;
    uint8_t *b = req_buf(s, sl, &cap);
    int rc = call_simple(
        s, sl,
        p9_enc_trenameat(b, cap, TAG(sl), olddirfid, p9_str(oldname), newdirfid, p9_str(newname)),
        P9_RRENAMEAT);
    slot_put(s, sl);
    return rc;
}

int fs9p_unlinkat(fs9p_session_t *s, uint32_t dirfid, const char *name, uint32_t flags)
{
    unsigned sl;
    int grc = slot_get(s, &sl);
    if (grc != 0)
        return grc;
    size_t cap;
    uint8_t *b = req_buf(s, sl, &cap);
    int rc = call_simple(s, sl, p9_enc_tunlinkat(b, cap, TAG(sl), dirfid, p9_str(name), flags),
                         P9_RUNLINKAT);
    slot_put(s, sl);
    return rc;
}
