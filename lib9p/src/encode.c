/* lib9p — T-message encoders. Field order per net/9p/client.c (Linux). */
#include "lib9p/p9.h"
#include "wire.h"

static bool str_ok(p9_str_t s)
{
    return s.len <= UINT16_MAX && (s.ptr != NULL || s.len == 0);
}

int32_t p9_enc_tversion(uint8_t *buf, size_t cap, uint16_t tag, uint32_t msize, p9_str_t version)
{
    if (!str_ok(version))
        return P9_E_INVAL;
    p9_w_t w;
    w_begin(&w, buf, cap, P9_TVERSION, tag);
    w_u32(&w, msize);
    w_str(&w, version);
    return w_finish(&w);
}

/* Tattach fid[4] afid[4] uname[s] aname[s] n_uname[4]; n_uname is the
 * 9P2000.u/.L numeric uid extension. */
int32_t p9_enc_tattach(uint8_t *buf, size_t cap, uint16_t tag, uint32_t fid, uint32_t afid,
                       p9_str_t uname, p9_str_t aname, uint32_t n_uname)
{
    if (!str_ok(uname) || !str_ok(aname))
        return P9_E_INVAL;
    p9_w_t w;
    w_begin(&w, buf, cap, P9_TATTACH, tag);
    w_u32(&w, fid);
    w_u32(&w, afid);
    w_str(&w, uname);
    w_str(&w, aname);
    w_u32(&w, n_uname);
    return w_finish(&w);
}

/* Twalk fid[4] newfid[4] nwname[2] nwname*(wname[s]) */
int32_t p9_enc_twalk(uint8_t *buf, size_t cap, uint16_t tag, uint32_t fid, uint32_t newfid,
                     uint16_t nwname, const p9_str_t *wnames)
{
    if (nwname > P9_MAXWELEM || (nwname > 0 && wnames == NULL))
        return P9_E_INVAL;
    for (uint16_t i = 0; i < nwname; i++)
        if (!str_ok(wnames[i]))
            return P9_E_INVAL;
    p9_w_t w;
    w_begin(&w, buf, cap, P9_TWALK, tag);
    w_u32(&w, fid);
    w_u32(&w, newfid);
    w_u16(&w, nwname);
    for (uint16_t i = 0; i < nwname; i++)
        w_str(&w, wnames[i]);
    return w_finish(&w);
}

int32_t p9_enc_tlopen(uint8_t *buf, size_t cap, uint16_t tag, uint32_t fid, uint32_t flags)
{
    p9_w_t w;
    w_begin(&w, buf, cap, P9_TLOPEN, tag);
    w_u32(&w, fid);
    w_u32(&w, flags);
    return w_finish(&w);
}

/* Tread and Treaddir share fid[4] offset[8] count[4]. */
static int32_t enc_fid_off_count(uint8_t *buf, size_t cap, uint8_t type, uint16_t tag, uint32_t fid,
                                 uint64_t offset, uint32_t count)
{
    p9_w_t w;
    w_begin(&w, buf, cap, type, tag);
    w_u32(&w, fid);
    w_u64(&w, offset);
    w_u32(&w, count);
    return w_finish(&w);
}

int32_t p9_enc_tread(uint8_t *buf, size_t cap, uint16_t tag, uint32_t fid, uint64_t offset,
                     uint32_t count)
{
    return enc_fid_off_count(buf, cap, P9_TREAD, tag, fid, offset, count);
}

int32_t p9_enc_treaddir(uint8_t *buf, size_t cap, uint16_t tag, uint32_t fid, uint64_t offset,
                        uint32_t count)
{
    return enc_fid_off_count(buf, cap, P9_TREADDIR, tag, fid, offset, count);
}

int32_t p9_enc_tgetattr(uint8_t *buf, size_t cap, uint16_t tag, uint32_t fid, uint64_t request_mask)
{
    p9_w_t w;
    w_begin(&w, buf, cap, P9_TGETATTR, tag);
    w_u32(&w, fid);
    w_u64(&w, request_mask);
    return w_finish(&w);
}

/* Tstatfs, Treadlink and Tclunk carry only fid[4]. */
static int32_t enc_fid_only(uint8_t *buf, size_t cap, uint8_t type, uint16_t tag, uint32_t fid)
{
    p9_w_t w;
    w_begin(&w, buf, cap, type, tag);
    w_u32(&w, fid);
    return w_finish(&w);
}

int32_t p9_enc_tstatfs(uint8_t *buf, size_t cap, uint16_t tag, uint32_t fid)
{
    return enc_fid_only(buf, cap, P9_TSTATFS, tag, fid);
}

int32_t p9_enc_treadlink(uint8_t *buf, size_t cap, uint16_t tag, uint32_t fid)
{
    return enc_fid_only(buf, cap, P9_TREADLINK, tag, fid);
}

int32_t p9_enc_tclunk(uint8_t *buf, size_t cap, uint16_t tag, uint32_t fid)
{
    return enc_fid_only(buf, cap, P9_TCLUNK, tag, fid);
}

/* --- write path and namespace operations ---------------------------------------- */

int32_t p9_enc_twrite(uint8_t *buf, size_t cap, uint16_t tag, uint32_t fid, uint64_t offset,
                      uint32_t count)
{
    p9_w_t w;
    w_begin(&w, buf, cap, P9_TWRITE, tag);
    w_u32(&w, fid);
    w_u64(&w, offset);
    w_u32(&w, count);
    (void)w_need(&w, count); /* reserve the caller's payload; not written */
    return w_finish(&w);
}

/* Tlcreate fid[4] name[s] flags[4] mode[4] gid[4] */
int32_t p9_enc_tlcreate(uint8_t *buf, size_t cap, uint16_t tag, uint32_t fid, p9_str_t name,
                        uint32_t flags, uint32_t mode, uint32_t gid)
{
    if (!str_ok(name))
        return P9_E_INVAL;
    p9_w_t w;
    w_begin(&w, buf, cap, P9_TLCREATE, tag);
    w_u32(&w, fid);
    w_str(&w, name);
    w_u32(&w, flags);
    w_u32(&w, mode);
    w_u32(&w, gid);
    return w_finish(&w);
}

/* Tsetattr fid[4] valid[4] mode[4] uid[4] gid[4] size[8] atime_sec[8]
 * atime_nsec[8] mtime_sec[8] mtime_nsec[8] */
int32_t p9_enc_tsetattr(uint8_t *buf, size_t cap, uint16_t tag, uint32_t fid, const p9_setattr_t *a)
{
    if (a == NULL)
        return P9_E_INVAL;
    p9_w_t w;
    w_begin(&w, buf, cap, P9_TSETATTR, tag);
    w_u32(&w, fid);
    w_u32(&w, a->valid);
    w_u32(&w, a->mode);
    w_u32(&w, a->uid);
    w_u32(&w, a->gid);
    w_u64(&w, a->size);
    w_u64(&w, a->atime_sec);
    w_u64(&w, a->atime_nsec);
    w_u64(&w, a->mtime_sec);
    w_u64(&w, a->mtime_nsec);
    return w_finish(&w);
}

/* Tfsync fid[4] datasync[4] */
int32_t p9_enc_tfsync(uint8_t *buf, size_t cap, uint16_t tag, uint32_t fid, uint32_t datasync)
{
    p9_w_t w;
    w_begin(&w, buf, cap, P9_TFSYNC, tag);
    w_u32(&w, fid);
    w_u32(&w, datasync);
    return w_finish(&w);
}

/* Tmkdir dfid[4] name[s] mode[4] gid[4] */
int32_t p9_enc_tmkdir(uint8_t *buf, size_t cap, uint16_t tag, uint32_t dfid, p9_str_t name,
                      uint32_t mode, uint32_t gid)
{
    if (!str_ok(name))
        return P9_E_INVAL;
    p9_w_t w;
    w_begin(&w, buf, cap, P9_TMKDIR, tag);
    w_u32(&w, dfid);
    w_str(&w, name);
    w_u32(&w, mode);
    w_u32(&w, gid);
    return w_finish(&w);
}

/* Tsymlink fid[4] name[s] symtgt[s] gid[4] */
int32_t p9_enc_tsymlink(uint8_t *buf, size_t cap, uint16_t tag, uint32_t dfid, p9_str_t name,
                        p9_str_t target, uint32_t gid)
{
    if (!str_ok(name) || !str_ok(target))
        return P9_E_INVAL;
    p9_w_t w;
    w_begin(&w, buf, cap, P9_TSYMLINK, tag);
    w_u32(&w, dfid);
    w_str(&w, name);
    w_str(&w, target);
    w_u32(&w, gid);
    return w_finish(&w);
}

/* Tlink dfid[4] fid[4] name[s] */
int32_t p9_enc_tlink(uint8_t *buf, size_t cap, uint16_t tag, uint32_t dfid, uint32_t fid,
                     p9_str_t name)
{
    if (!str_ok(name))
        return P9_E_INVAL;
    p9_w_t w;
    w_begin(&w, buf, cap, P9_TLINK, tag);
    w_u32(&w, dfid);
    w_u32(&w, fid);
    w_str(&w, name);
    return w_finish(&w);
}

/* Trenameat olddirfid[4] oldname[s] newdirfid[4] newname[s] */
int32_t p9_enc_trenameat(uint8_t *buf, size_t cap, uint16_t tag, uint32_t olddirfid,
                         p9_str_t oldname, uint32_t newdirfid, p9_str_t newname)
{
    if (!str_ok(oldname) || !str_ok(newname))
        return P9_E_INVAL;
    p9_w_t w;
    w_begin(&w, buf, cap, P9_TRENAMEAT, tag);
    w_u32(&w, olddirfid);
    w_str(&w, oldname);
    w_u32(&w, newdirfid);
    w_str(&w, newname);
    return w_finish(&w);
}

/* Tunlinkat dirfd[4] name[s] flags[4] */
int32_t p9_enc_tunlinkat(uint8_t *buf, size_t cap, uint16_t tag, uint32_t dirfid, p9_str_t name,
                         uint32_t flags)
{
    if (!str_ok(name))
        return P9_E_INVAL;
    p9_w_t w;
    w_begin(&w, buf, cap, P9_TUNLINKAT, tag);
    w_u32(&w, dirfid);
    w_str(&w, name);
    w_u32(&w, flags);
    return w_finish(&w);
}
