/* lib9p — R-message decoders. Field order per net/9p/client.c (Linux). */
#include "lib9p/p9.h"
#include "wire.h"

int p9_dec_hdr(const uint8_t *msg, size_t len, p9_hdr_t *hdr)
{
    if (msg == NULL || len < P9_HEADER_SIZE)
        return P9_E_SHORT;
    p9_r_t r = {msg, P9_HEADER_SIZE, 0, false};
    hdr->size = r_u32(&r);
    hdr->type = r_u8(&r);
    hdr->tag = r_u16(&r);
    /* The size field covers the whole message; the transport may deliver
     * more bytes than that (e.g. a used-ring length), never fewer. */
    if (hdr->size < P9_HEADER_SIZE)
        return P9_E_PROTO;
    if (hdr->size > len)
        return P9_E_SHORT;
    return P9_OK;
}

/* Validate the header, require `type`, and set up a reader on the body. */
static int body(const uint8_t *msg, size_t len, uint8_t type, p9_r_t *r)
{
    p9_hdr_t h;
    int rc = p9_dec_hdr(msg, len, &h);
    if (rc != P9_OK)
        return rc;
    if (h.type != type)
        return P9_E_TYPE;
    r->p = msg;
    r->end = h.size;
    r->pos = P9_HEADER_SIZE;
    r->fail = false;
    return P9_OK;
}

/* Every field read and nothing left over. */
static int done(const p9_r_t *r)
{
    return (!r->fail && r->pos == r->end) ? P9_OK : P9_E_PROTO;
}

int p9_dec_rlerror(const uint8_t *msg, size_t len, uint32_t *ecode)
{
    p9_r_t r;
    int rc = body(msg, len, P9_RLERROR, &r);
    if (rc != P9_OK)
        return rc;
    *ecode = r_u32(&r);
    return done(&r);
}

int p9_dec_rversion(const uint8_t *msg, size_t len, uint32_t *msize, p9_str_t *version)
{
    p9_r_t r;
    int rc = body(msg, len, P9_RVERSION, &r);
    if (rc != P9_OK)
        return rc;
    *msize = r_u32(&r);
    *version = r_str(&r);
    return done(&r);
}

int p9_dec_rattach(const uint8_t *msg, size_t len, p9_qid_t *qid)
{
    p9_r_t r;
    int rc = body(msg, len, P9_RATTACH, &r);
    if (rc != P9_OK)
        return rc;
    *qid = r_qid(&r);
    return done(&r);
}

/* Rwalk nwqid[2] nwqid*(qid[13]) */
int p9_dec_rwalk(const uint8_t *msg, size_t len, p9_qid_t *qids, uint16_t max_qids, uint16_t *nwqid)
{
    p9_r_t r;
    int rc = body(msg, len, P9_RWALK, &r);
    if (rc != P9_OK)
        return rc;
    uint16_t n = r_u16(&r);
    if (r.fail || n > P9_MAXWELEM || n > max_qids || (n > 0 && qids == NULL))
        return P9_E_PROTO;
    for (uint16_t i = 0; i < n; i++)
        qids[i] = r_qid(&r);
    *nwqid = n;
    return done(&r);
}

/* Rlopen and Rlcreate: qid[13] iounit[4] */
static int dec_qid_iounit(const uint8_t *msg, size_t len, uint8_t type, p9_qid_t *qid,
                          uint32_t *iounit)
{
    p9_r_t r;
    int rc = body(msg, len, type, &r);
    if (rc != P9_OK)
        return rc;
    *qid = r_qid(&r);
    *iounit = r_u32(&r);
    return done(&r);
}

int p9_dec_rlopen(const uint8_t *msg, size_t len, p9_qid_t *qid, uint32_t *iounit)
{
    return dec_qid_iounit(msg, len, P9_RLOPEN, qid, iounit);
}

int p9_dec_rlcreate(const uint8_t *msg, size_t len, p9_qid_t *qid, uint32_t *iounit)
{
    return dec_qid_iounit(msg, len, P9_RLCREATE, qid, iounit);
}

/* Rread / Rreaddir count[4] data[count] */
static int dec_count_data(const uint8_t *msg, size_t len, uint8_t type, const uint8_t **data,
                          uint32_t *count)
{
    p9_r_t r;
    int rc = body(msg, len, type, &r);
    if (rc != P9_OK)
        return rc;
    uint32_t n = r_u32(&r);
    const uint8_t *d = r_need(&r, n);
    rc = done(&r);
    if (rc != P9_OK)
        return rc;
    *data = d;
    *count = n;
    return P9_OK;
}

int p9_dec_rread(const uint8_t *msg, size_t len, const uint8_t **data, uint32_t *count)
{
    return dec_count_data(msg, len, P9_RREAD, data, count);
}

int p9_dec_rreaddir(const uint8_t *msg, size_t len, const uint8_t **data, uint32_t *count)
{
    return dec_count_data(msg, len, P9_RREADDIR, data, count);
}

/* Rgetattr valid[8] qid[13] mode[4] uid[4] gid[4] nlink[8] rdev[8] size[8]
 * blksize[8] blocks[8] atime_sec[8] atime_nsec[8] mtime_sec[8] mtime_nsec[8]
 * ctime_sec[8] ctime_nsec[8] btime_sec[8] btime_nsec[8] gen[8]
 * data_version[8] */
int p9_dec_rgetattr(const uint8_t *msg, size_t len, p9_attr_t *a)
{
    p9_r_t r;
    int rc = body(msg, len, P9_RGETATTR, &r);
    if (rc != P9_OK)
        return rc;
    a->valid = r_u64(&r);
    a->qid = r_qid(&r);
    a->mode = r_u32(&r);
    a->uid = r_u32(&r);
    a->gid = r_u32(&r);
    a->nlink = r_u64(&r);
    a->rdev = r_u64(&r);
    a->size = r_u64(&r);
    a->blksize = r_u64(&r);
    a->blocks = r_u64(&r);
    a->atime_sec = r_u64(&r);
    a->atime_nsec = r_u64(&r);
    a->mtime_sec = r_u64(&r);
    a->mtime_nsec = r_u64(&r);
    a->ctime_sec = r_u64(&r);
    a->ctime_nsec = r_u64(&r);
    a->btime_sec = r_u64(&r);
    a->btime_nsec = r_u64(&r);
    a->gen = r_u64(&r);
    a->data_version = r_u64(&r);
    return done(&r);
}

/* Rstatfs type[4] bsize[4] blocks[8] bfree[8] bavail[8] files[8] ffree[8]
 * fsid[8] namelen[4] */
int p9_dec_rstatfs(const uint8_t *msg, size_t len, p9_statfs_t *st)
{
    p9_r_t r;
    int rc = body(msg, len, P9_RSTATFS, &r);
    if (rc != P9_OK)
        return rc;
    st->type = r_u32(&r);
    st->bsize = r_u32(&r);
    st->blocks = r_u64(&r);
    st->bfree = r_u64(&r);
    st->bavail = r_u64(&r);
    st->files = r_u64(&r);
    st->ffree = r_u64(&r);
    st->fsid = r_u64(&r);
    st->namelen = r_u32(&r);
    return done(&r);
}

int p9_dec_rreadlink(const uint8_t *msg, size_t len, p9_str_t *target)
{
    p9_r_t r;
    int rc = body(msg, len, P9_RREADLINK, &r);
    if (rc != P9_OK)
        return rc;
    *target = r_str(&r);
    return done(&r);
}

/* Replies with no body. */
static int dec_empty(const uint8_t *msg, size_t len, uint8_t type)
{
    p9_r_t r;
    int rc = body(msg, len, type, &r);
    if (rc != P9_OK)
        return rc;
    return done(&r);
}

int p9_dec_rclunk(const uint8_t *msg, size_t len)
{
    return dec_empty(msg, len, P9_RCLUNK);
}

int p9_dec_rsetattr(const uint8_t *msg, size_t len)
{
    return dec_empty(msg, len, P9_RSETATTR);
}

int p9_dec_rfsync(const uint8_t *msg, size_t len)
{
    return dec_empty(msg, len, P9_RFSYNC);
}

int p9_dec_rlink(const uint8_t *msg, size_t len)
{
    return dec_empty(msg, len, P9_RLINK);
}

int p9_dec_rrenameat(const uint8_t *msg, size_t len)
{
    return dec_empty(msg, len, P9_RRENAMEAT);
}

int p9_dec_runlinkat(const uint8_t *msg, size_t len)
{
    return dec_empty(msg, len, P9_RUNLINKAT);
}

/* Rwrite count[4] */
int p9_dec_rwrite(const uint8_t *msg, size_t len, uint32_t *count)
{
    p9_r_t r;
    int rc = body(msg, len, P9_RWRITE, &r);
    if (rc != P9_OK)
        return rc;
    *count = r_u32(&r);
    return done(&r);
}

/* Rmkdir and Rsymlink: qid[13] */
static int dec_qid(const uint8_t *msg, size_t len, uint8_t type, p9_qid_t *qid)
{
    p9_r_t r;
    int rc = body(msg, len, type, &r);
    if (rc != P9_OK)
        return rc;
    *qid = r_qid(&r);
    return done(&r);
}

int p9_dec_rmkdir(const uint8_t *msg, size_t len, p9_qid_t *qid)
{
    return dec_qid(msg, len, P9_RMKDIR, qid);
}

int p9_dec_rsymlink(const uint8_t *msg, size_t len, p9_qid_t *qid)
{
    return dec_qid(msg, len, P9_RSYMLINK, qid);
}

/* Rreaddir entry: qid[13] offset[8] type[1] name[s] */
int p9_dirent_next(const uint8_t *data, size_t len, size_t *pos, p9_dirent_t *de)
{
    if (*pos == len)
        return 0;
    if (data == NULL || *pos > len)
        return P9_E_PROTO;
    p9_r_t r = {data, len, *pos, false};
    p9_dirent_t e;
    e.qid = r_qid(&r);
    e.offset = r_u64(&r);
    e.type = r_u8(&r);
    e.name = r_str(&r);
    if (r.fail)
        return P9_E_PROTO;
    *de = e;
    *pos = r.pos;
    return 1;
}
