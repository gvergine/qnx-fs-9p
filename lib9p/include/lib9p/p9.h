/*
 * lib9p — portable 9P2000.L codec.
 *
 * Freestanding C11: no OS headers, no I/O, no allocation, no errno.
 * Callers supply buffers. Encoders return the encoded message length (> 0)
 * or a negative P9_E* code; decoders return P9_OK (0) or a negative code.
 * Decoded strings and data are views into the caller's message buffer and
 * stay valid only as long as that buffer does.
 *
 * Wire format reference: the Linux kernel 9P client (include/net/9p/9p.h,
 * net/9p/client.c, net/9p/protocol.c), which is the de facto 9P2000.L spec.
 * All integers are little-endian; strings are len[2] + bytes, no NUL.
 */
#ifndef LIB9P_P9_H
#define LIB9P_P9_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define P9_PROTO_VERSION "9P2000.L"

/* --- message types (T = request, R = T + 1) --------------------------------- */

enum {
    P9_TLERROR = 6, /* not sent; Rlerror replaces Rerror in 9P2000.L */
    P9_RLERROR = 7,
    P9_TSTATFS = 8,
    P9_RSTATFS = 9,
    P9_TLOPEN = 12,
    P9_RLOPEN = 13,
    P9_TLCREATE = 14,
    P9_RLCREATE = 15,
    P9_TSYMLINK = 16,
    P9_RSYMLINK = 17,
    P9_TREADLINK = 22,
    P9_RREADLINK = 23,
    P9_TGETATTR = 24,
    P9_RGETATTR = 25,
    P9_TSETATTR = 26,
    P9_RSETATTR = 27,
    P9_TREADDIR = 40,
    P9_RREADDIR = 41,
    P9_TFSYNC = 50,
    P9_RFSYNC = 51,
    P9_TLINK = 70,
    P9_RLINK = 71,
    P9_TMKDIR = 72,
    P9_RMKDIR = 73,
    P9_TRENAMEAT = 74,
    P9_RRENAMEAT = 75,
    P9_TUNLINKAT = 76,
    P9_RUNLINKAT = 77,
    P9_TVERSION = 100,
    P9_RVERSION = 101,
    P9_TATTACH = 104,
    P9_RATTACH = 105,
    P9_TWALK = 110,
    P9_RWALK = 111,
    P9_TREAD = 116,
    P9_RREAD = 117,
    P9_TWRITE = 118,
    P9_RWRITE = 119,
    P9_TCLUNK = 120,
    P9_RCLUNK = 121,
};

/* --- constants ---------------------------------------------------------------- */

#define P9_NOTAG    ((uint16_t)0xFFFFu)
#define P9_NOFID    ((uint32_t)0xFFFFFFFFu)
#define P9_MAXWELEM 16u /* max path elements in one Twalk */

#define P9_HEADER_SIZE 7u  /* size[4] type[1] tag[2] */
#define P9_QID_SIZE    13u /* type[1] version[4] path[8] */
/* Rread / Rreaddir overhead: header + count[4]. Payload per request is at
 * most msize - P9_RREAD_OVERHEAD. */
#define P9_RREAD_OVERHEAD 11u
/* Twrite overhead: header + fid[4] offset[8] count[4]. Payload per request
 * is at most msize - P9_TWRITE_OVERHEAD. */
#define P9_TWRITE_OVERHEAD 23u

/* qid.type bits */
#define P9_QTDIR     0x80u
#define P9_QTAPPEND  0x40u
#define P9_QTEXCL    0x20u
#define P9_QTMOUNT   0x10u
#define P9_QTAUTH    0x08u
#define P9_QTTMP     0x04u
#define P9_QTSYMLINK 0x02u
#define P9_QTLINK    0x01u
#define P9_QTFILE    0x00u

/* Tlopen / Tlcreate flags: Linux generic open(2) values, independent of the
 * guest architecture; the server translates them (include/net/9p/9p.h). */
#define P9_DOTL_RDONLY    0x00000000u
#define P9_DOTL_WRONLY    0x00000001u
#define P9_DOTL_RDWR      0x00000002u
#define P9_DOTL_CREATE    0x00000040u /* 0100 octal */
#define P9_DOTL_EXCL      0x00000080u /* 0200 octal */
#define P9_DOTL_TRUNC     0x00000200u /* 01000 octal */
#define P9_DOTL_APPEND    0x00000400u /* 02000 octal */
#define P9_DOTL_NONBLOCK  0x00000800u /* 04000 octal */
#define P9_DOTL_DSYNC     0x00001000u /* 010000 octal */
#define P9_DOTL_DIRECTORY 0x00010000u /* 0200000 octal */
#define P9_DOTL_NOFOLLOW  0x00020000u /* 0400000 octal */
#define P9_DOTL_CLOEXEC   0x00080000u /* 02000000 octal */
#define P9_DOTL_SYNC      0x00100000u /* 04000000 octal */

/* Tunlinkat flags */
#define P9_DOTL_AT_REMOVEDIR 0x200u

/* Tsetattr valid bits: Linux ATTR_* values (iattr.ia_valid). Not in the
 * installed kernel headers; MODE, UID, GID, SIZE and the time bits were
 * verified against QEMU 10.2.1 (chmod, chown, truncate, touch -d). */
#define P9_SETATTR_MODE      0x001u
#define P9_SETATTR_UID       0x002u
#define P9_SETATTR_GID       0x004u
#define P9_SETATTR_SIZE      0x008u
#define P9_SETATTR_ATIME     0x010u /* change atime: to now, unless ATIME_SET */
#define P9_SETATTR_MTIME     0x020u /* change mtime: to now, unless MTIME_SET */
#define P9_SETATTR_CTIME     0x040u
#define P9_SETATTR_ATIME_SET 0x080u /* use atime_sec/atime_nsec */
#define P9_SETATTR_MTIME_SET 0x100u /* use mtime_sec/mtime_nsec */

/* Tgetattr request_mask / Rgetattr valid bits */
#define P9_GETATTR_MODE         0x00000001ull
#define P9_GETATTR_NLINK        0x00000002ull
#define P9_GETATTR_UID          0x00000004ull
#define P9_GETATTR_GID          0x00000008ull
#define P9_GETATTR_RDEV         0x00000010ull
#define P9_GETATTR_ATIME        0x00000020ull
#define P9_GETATTR_MTIME        0x00000040ull
#define P9_GETATTR_CTIME        0x00000080ull
#define P9_GETATTR_INO          0x00000100ull
#define P9_GETATTR_SIZE         0x00000200ull
#define P9_GETATTR_BLOCKS       0x00000400ull
#define P9_GETATTR_BTIME        0x00000800ull
#define P9_GETATTR_GEN          0x00001000ull
#define P9_GETATTR_DATA_VERSION 0x00002000ull
#define P9_GETATTR_BASIC        0x000007FFull /* MODE .. BLOCKS */
#define P9_GETATTR_ALL          0x00003FFFull

/* --- errors ------------------------------------------------------------------- */

enum {
    P9_OK = 0,
    P9_E_NOSPACE = -1,   /* output buffer too small for the message */
    P9_E_SHORT = -2,     /* input shorter than its header or size field says */
    P9_E_PROTO = -3,     /* malformed body: bad count, overrun, trailing bytes */
    P9_E_TYPE = -4,      /* message type is not the one expected */
    P9_E_INVAL = -5,     /* bad argument from the caller */
    P9_E_EXHAUSTED = -6, /* id pool has no free id */
};

/* Short static description of a P9_E* code; never NULL. */
const char *p9_strerror(int err);

/* Name of a message type ("Tversion", "Rlerror", ...) or NULL if unknown. */
const char *p9_msg_name(uint8_t type);

/* Returns the protocol version string this library speaks. */
const char *p9_proto_version(void);

/* --- types -------------------------------------------------------------------- */

/* Counted string, not NUL-terminated. Encoders reject len > 65535. */
typedef struct {
    const char *ptr;
    size_t len;
} p9_str_t;

/* View of a C string; ptr must not be NULL. */
p9_str_t p9_str(const char *s);

typedef struct {
    uint8_t type;
    uint32_t version;
    uint64_t path;
} p9_qid_t;

typedef struct {
    uint32_t size;
    uint8_t type;
    uint16_t tag;
} p9_hdr_t;

/* Rgetattr body. Only fields whose bit is set in `valid` are meaningful. */
typedef struct {
    uint64_t valid;
    p9_qid_t qid;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint64_t nlink;
    uint64_t rdev;
    uint64_t size;
    uint64_t blksize;
    uint64_t blocks;
    uint64_t atime_sec, atime_nsec;
    uint64_t mtime_sec, mtime_nsec;
    uint64_t ctime_sec, ctime_nsec;
    uint64_t btime_sec, btime_nsec;
    uint64_t gen;
    uint64_t data_version;
} p9_attr_t;

/* Tsetattr body (Linux struct p9_iattr_dotl). Only fields whose bit is set
 * in `valid` are used by the server. */
typedef struct {
    uint32_t valid;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint64_t size;
    uint64_t atime_sec, atime_nsec;
    uint64_t mtime_sec, mtime_nsec;
} p9_setattr_t;

/* Rstatfs body (field meanings follow Linux struct statfs). */
typedef struct {
    uint32_t type;
    uint32_t bsize;
    uint64_t blocks;
    uint64_t bfree;
    uint64_t bavail;
    uint64_t files;
    uint64_t ffree;
    uint64_t fsid;
    uint32_t namelen;
} p9_statfs_t;

/* One Rreaddir entry. `offset` is the cookie to pass to the next Treaddir
 * to continue after this entry; `type` is a Linux DT_* value. */
typedef struct {
    p9_qid_t qid;
    uint64_t offset;
    uint8_t type;
    p9_str_t name;
} p9_dirent_t;

/* --- encoders (T-messages) -------------------------------------------------- */
/* Each writes a complete message into buf[0..cap) and returns its length,
 * or P9_E_NOSPACE / P9_E_INVAL. Nothing beyond the returned length is
 * touched; on error the buffer contents are unspecified. */

int32_t p9_enc_tversion(uint8_t *buf, size_t cap, uint16_t tag, uint32_t msize, p9_str_t version);
int32_t p9_enc_tattach(uint8_t *buf, size_t cap, uint16_t tag, uint32_t fid, uint32_t afid,
                       p9_str_t uname, p9_str_t aname, uint32_t n_uname);
/* nwname <= P9_MAXWELEM; nwname == 0 clones fid into newfid. */
int32_t p9_enc_twalk(uint8_t *buf, size_t cap, uint16_t tag, uint32_t fid, uint32_t newfid,
                     uint16_t nwname, const p9_str_t *wnames);
int32_t p9_enc_tlopen(uint8_t *buf, size_t cap, uint16_t tag, uint32_t fid, uint32_t flags);
int32_t p9_enc_tread(uint8_t *buf, size_t cap, uint16_t tag, uint32_t fid, uint64_t offset,
                     uint32_t count);
int32_t p9_enc_treaddir(uint8_t *buf, size_t cap, uint16_t tag, uint32_t fid, uint64_t offset,
                        uint32_t count);
int32_t p9_enc_tgetattr(uint8_t *buf, size_t cap, uint16_t tag, uint32_t fid,
                        uint64_t request_mask);
int32_t p9_enc_tstatfs(uint8_t *buf, size_t cap, uint16_t tag, uint32_t fid);
int32_t p9_enc_treadlink(uint8_t *buf, size_t cap, uint16_t tag, uint32_t fid);
int32_t p9_enc_tclunk(uint8_t *buf, size_t cap, uint16_t tag, uint32_t fid);
/* Twrite fid[4] offset[8] count[4] data[count]. Writes the header only and
 * reserves the payload: the caller puts the `count` data bytes at
 * buf + P9_TWRITE_OVERHEAD (before or after this call), so data can go from
 * its source straight into the request. Needs cap >= P9_TWRITE_OVERHEAD +
 * count; the payload bytes themselves are never touched. */
int32_t p9_enc_twrite(uint8_t *buf, size_t cap, uint16_t tag, uint32_t fid, uint64_t offset,
                      uint32_t count);
/* Creates `name` in the directory `fid` and opens it; fid then refers to
 * the new file. flags are P9_DOTL_*; gid is the new file's group. */
int32_t p9_enc_tlcreate(uint8_t *buf, size_t cap, uint16_t tag, uint32_t fid, p9_str_t name,
                        uint32_t flags, uint32_t mode, uint32_t gid);
int32_t p9_enc_tsetattr(uint8_t *buf, size_t cap, uint16_t tag, uint32_t fid,
                        const p9_setattr_t *attr);
int32_t p9_enc_tfsync(uint8_t *buf, size_t cap, uint16_t tag, uint32_t fid, uint32_t datasync);
int32_t p9_enc_tmkdir(uint8_t *buf, size_t cap, uint16_t tag, uint32_t dfid, p9_str_t name,
                      uint32_t mode, uint32_t gid);
int32_t p9_enc_tsymlink(uint8_t *buf, size_t cap, uint16_t tag, uint32_t dfid, p9_str_t name,
                        p9_str_t target, uint32_t gid);
/* Hard link: `name` in directory dfid becomes another name for fid. */
int32_t p9_enc_tlink(uint8_t *buf, size_t cap, uint16_t tag, uint32_t dfid, uint32_t fid,
                     p9_str_t name);
int32_t p9_enc_trenameat(uint8_t *buf, size_t cap, uint16_t tag, uint32_t olddirfid,
                         p9_str_t oldname, uint32_t newdirfid, p9_str_t newname);
/* flags: 0 or P9_DOTL_AT_REMOVEDIR. */
int32_t p9_enc_tunlinkat(uint8_t *buf, size_t cap, uint16_t tag, uint32_t dirfid, p9_str_t name,
                         uint32_t flags);

/* --- decoders (R-messages) -------------------------------------------------- */
/* `msg` points at the start of a reply and `len` is the number of bytes the
 * transport delivered (may exceed the size field, never be shorter).
 * Every decoder validates the header, requires the expected type, bounds-
 * checks every field, and rejects trailing bytes. A reply of type Rlerror
 * makes any decoder other than p9_dec_rlerror() return P9_E_TYPE: check
 * hdr.type first. */

int p9_dec_hdr(const uint8_t *msg, size_t len, p9_hdr_t *hdr);
int p9_dec_rlerror(const uint8_t *msg, size_t len, uint32_t *ecode);
int p9_dec_rversion(const uint8_t *msg, size_t len, uint32_t *msize, p9_str_t *version);
int p9_dec_rattach(const uint8_t *msg, size_t len, p9_qid_t *qid);
/* Fails with P9_E_PROTO if the reply holds more than max_qids qids. */
int p9_dec_rwalk(const uint8_t *msg, size_t len, p9_qid_t *qids, uint16_t max_qids,
                 uint16_t *nwqid);
int p9_dec_rlopen(const uint8_t *msg, size_t len, p9_qid_t *qid, uint32_t *iounit);
/* *data points into msg; *count bytes are valid. */
int p9_dec_rread(const uint8_t *msg, size_t len, const uint8_t **data, uint32_t *count);
/* Same layout as Rread; iterate the entries with p9_dirent_next(). */
int p9_dec_rreaddir(const uint8_t *msg, size_t len, const uint8_t **data, uint32_t *count);
int p9_dec_rgetattr(const uint8_t *msg, size_t len, p9_attr_t *attr);
int p9_dec_rstatfs(const uint8_t *msg, size_t len, p9_statfs_t *st);
int p9_dec_rreadlink(const uint8_t *msg, size_t len, p9_str_t *target);
int p9_dec_rclunk(const uint8_t *msg, size_t len);
int p9_dec_rwrite(const uint8_t *msg, size_t len, uint32_t *count);
int p9_dec_rlcreate(const uint8_t *msg, size_t len, p9_qid_t *qid, uint32_t *iounit);
int p9_dec_rmkdir(const uint8_t *msg, size_t len, p9_qid_t *qid);
int p9_dec_rsymlink(const uint8_t *msg, size_t len, p9_qid_t *qid);
/* Replies with an empty body. */
int p9_dec_rsetattr(const uint8_t *msg, size_t len);
int p9_dec_rfsync(const uint8_t *msg, size_t len);
int p9_dec_rlink(const uint8_t *msg, size_t len);
int p9_dec_rrenameat(const uint8_t *msg, size_t len);
int p9_dec_runlinkat(const uint8_t *msg, size_t len);

/* Decode the directory entry at data[*pos]. Returns 1 and advances *pos on
 * success, 0 when *pos == len (no more entries), or P9_E_PROTO if the
 * entry is truncated or *pos is out of range. */
int p9_dirent_next(const uint8_t *data, size_t len, size_t *pos, p9_dirent_t *de);

/* --- id pools (fids, tags) ------------------------------------------------------ */
/* Bitmap allocator over caller-supplied storage. Ids are handed out
 * round-robin so that a just-freed id is not immediately reused, which keeps
 * QEMU traces readable and makes use-after-clunk bugs visible. */

typedef struct {
    uint32_t *words; /* (nids + 31) / 32 words, owned by the caller */
    uint32_t nids;   /* valid ids are 0 .. nids-1 */
    uint32_t next;   /* where the next search starts */
    uint32_t used;
} p9_idpool_t;

#define P9_IDPOOL_WORDS(n) (((n) + 31u) / 32u)

/* nids must be in 1 .. 0xFFFFFFFE so P9_NOFID is never handed out; for tag
 * pools pass nids <= 0xFFFF so P9_NOTAG is never handed out. */
int p9_idpool_init(p9_idpool_t *pool, uint32_t *words, uint32_t nids);
int p9_idpool_alloc(p9_idpool_t *pool, uint32_t *id);
/* P9_E_INVAL if id is out of range or not allocated (double free). */
int p9_idpool_free(p9_idpool_t *pool, uint32_t id);
bool p9_idpool_in_use(const p9_idpool_t *pool, uint32_t id);
uint32_t p9_idpool_used(const p9_idpool_t *pool);

#endif /* LIB9P_P9_H */
