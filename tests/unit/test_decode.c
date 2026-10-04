/* lib9p decoder tests: golden replies, field layouts, truncation, trailing bytes. */
#include <stdlib.h>

#include "lib9p/p9.h"
#include "test.h"

/* Produced by QEMU 10.2.1 in reply to the first virtio test program. */
static const uint8_t g_rversion[] = {0x15, 0x00, 0x00, 0x00, 0x65, 0xff, 0xff,
                                     0x00, 0x20, 0x00, 0x00, 0x08, 0x00, '9',
                                     'P',  '2',  '0',  '0',  '0',  '.',  'L'};
static const uint8_t g_rattach[] = {0x14, 0x00, 0x00, 0x00, 0x69, 0x01, 0x00, 0x80, 0x9d, 0x66,
                                    0xd1, 0x6a, 0x5b, 0x49, 0xa8, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t g_rclunk[] = {0x07, 0x00, 0x00, 0x00, 0x79, 0x02, 0x00};

static void test_golden(void)
{
    uint32_t msize = 0;
    p9_str_t v;
    CHECK_EQ(p9_dec_rversion(g_rversion, sizeof g_rversion, &msize, &v), P9_OK);
    CHECK_EQ(msize, 8192);
    CHECK(v.len == 8 && memcmp(v.ptr, "9P2000.L", 8) == 0);

    p9_hdr_t h;
    CHECK_EQ(p9_dec_hdr(g_rattach, sizeof g_rattach, &h), P9_OK);
    CHECK_EQ(h.size, 20);
    CHECK_EQ(h.type, P9_RATTACH);
    CHECK_EQ(h.tag, 1);
    p9_qid_t q;
    CHECK_EQ(p9_dec_rattach(g_rattach, sizeof g_rattach, &q), P9_OK);
    CHECK_EQ(q.type, P9_QTDIR);
    CHECK_EQ(q.version, 1792108189u); /* "v9fs_attach_return ... version 1792108189" */
    CHECK_EQ(q.path, 11028827u);      /* "... path 11028827" */

    CHECK_EQ(p9_dec_rclunk(g_rclunk, sizeof g_rclunk), P9_OK);
    /* Wrong decoder for the type. */
    CHECK_EQ(p9_dec_rattach(g_rclunk, sizeof g_rclunk, &q), P9_E_TYPE);
}

/* --- hand-built replies ------------------------------------------------------ */

static void build_rlerror(tb_t *t)
{
    tb_begin(t, P9_RLERROR, 9);
    tb_le(t, 2, 4); /* ENOENT */
    tb_end(t);
}

static void build_rwalk(tb_t *t)
{
    tb_begin(t, P9_RWALK, 9);
    tb_le(t, 2, 2);
    tb_qid(t, P9_QTDIR, 1, 0x1111);
    tb_qid(t, P9_QTFILE, 2, 0x2222);
    tb_end(t);
}

static void build_rlopen(tb_t *t)
{
    tb_begin(t, P9_RLOPEN, 9);
    tb_qid(t, P9_QTFILE, 3, 0xABCDEF0123456789ull);
    tb_le(t, 4096, 4);
    tb_end(t);
}

static void build_rread(tb_t *t)
{
    tb_begin(t, P9_RREAD, 9);
    tb_le(t, 5, 4);
    memcpy(t->b + t->n, "hello", 5);
    t->n += 5;
    tb_end(t);
}

static void build_rreaddir(tb_t *t)
{
    tb_begin(t, P9_RREADDIR, 9);
    size_t count_at = t->n;
    tb_le(t, 0, 4);
    size_t start = t->n;
    tb_qid(t, P9_QTDIR, 0, 1);
    tb_le(t, 1, 8);
    tb_u8(t, 4); /* DT_DIR */
    tb_str(t, ".");
    tb_qid(t, P9_QTFILE, 0, 2);
    tb_le(t, 0x7fffffffffffffffull, 8);
    tb_u8(t, 8); /* DT_REG */
    tb_str(t, "file.txt");
    size_t count = t->n - start;
    for (int i = 0; i < 4; i++)
        t->b[count_at + (size_t)i] = (uint8_t)(count >> (8 * i));
    tb_end(t);
}

static void build_rgetattr(tb_t *t)
{
    tb_begin(t, P9_RGETATTR, 9);
    tb_le(t, P9_GETATTR_BASIC, 8);
    tb_qid(t, P9_QTFILE, 7, 0x77);
    tb_le(t, 0100644, 4); /* mode */
    tb_le(t, 1000, 4);    /* uid */
    tb_le(t, 1001, 4);    /* gid */
    for (uint64_t i = 1; i <= 15; i++)
        tb_le(t, i * 0x0101010101ull, 8); /* nlink .. data_version */
    tb_end(t);
}

static void build_rstatfs(tb_t *t)
{
    tb_begin(t, P9_RSTATFS, 9);
    tb_le(t, 0x01021994, 4); /* type */
    tb_le(t, 4096, 4);       /* bsize */
    for (uint64_t i = 1; i <= 6; i++)
        tb_le(t, i * 1000, 8); /* blocks .. fsid */
    tb_le(t, 255, 4);          /* namelen */
    tb_end(t);
}

static void build_rreadlink(tb_t *t)
{
    tb_begin(t, P9_RREADLINK, 9);
    tb_str(t, "../target");
    tb_end(t);
}

static void build_rwrite(tb_t *t)
{
    tb_begin(t, P9_RWRITE, 9);
    tb_le(t, 0x12345, 4);
    tb_end(t);
}

static void build_rlcreate(tb_t *t)
{
    tb_begin(t, P9_RLCREATE, 9);
    tb_qid(t, P9_QTFILE, 5, 0x5555);
    tb_le(t, 8192, 4);
    tb_end(t);
}

static void build_rmkdir(tb_t *t)
{
    tb_begin(t, P9_RMKDIR, 9);
    tb_qid(t, P9_QTDIR, 6, 0x6666);
    tb_end(t);
}

static void build_rsymlink(tb_t *t)
{
    tb_begin(t, P9_RSYMLINK, 9);
    tb_qid(t, P9_QTSYMLINK, 7, 0x7777);
    tb_end(t);
}

static void build_rsetattr(tb_t *t)
{
    tb_begin(t, P9_RSETATTR, 9);
    tb_end(t);
}
static void build_rfsync(tb_t *t)
{
    tb_begin(t, P9_RFSYNC, 9);
    tb_end(t);
}
static void build_rlink(tb_t *t)
{
    tb_begin(t, P9_RLINK, 9);
    tb_end(t);
}
static void build_rrenameat(tb_t *t)
{
    tb_begin(t, P9_RRENAMEAT, 9);
    tb_end(t);
}
static void build_runlinkat(tb_t *t)
{
    tb_begin(t, P9_RUNLINKAT, 9);
    tb_end(t);
}

static void test_layouts_write(void)
{
    tb_t t;
    uint32_t u = 0;
    p9_qid_t q;

    build_rwrite(&t);
    CHECK_EQ(p9_dec_rwrite(t.b, t.n, &u), P9_OK);
    CHECK_EQ(u, 0x12345);

    build_rlcreate(&t);
    CHECK_EQ(p9_dec_rlcreate(t.b, t.n, &q, &u), P9_OK);
    CHECK_EQ(q.path, 0x5555);
    CHECK_EQ(u, 8192);
    /* Same layout as Rlopen, different type. */
    CHECK_EQ(p9_dec_rlopen(t.b, t.n, &q, &u), P9_E_TYPE);

    build_rmkdir(&t);
    CHECK_EQ(p9_dec_rmkdir(t.b, t.n, &q), P9_OK);
    CHECK_EQ(q.type, P9_QTDIR);
    CHECK_EQ(p9_dec_rsymlink(t.b, t.n, &q), P9_E_TYPE);
    build_rsymlink(&t);
    CHECK_EQ(p9_dec_rsymlink(t.b, t.n, &q), P9_OK);
    CHECK_EQ(q.type, P9_QTSYMLINK);
    CHECK_EQ(q.path, 0x7777);

    build_rsetattr(&t);
    CHECK_EQ(t.n, 7);
    CHECK_EQ(p9_dec_rsetattr(t.b, t.n), P9_OK);
    CHECK_EQ(p9_dec_rfsync(t.b, t.n), P9_E_TYPE);
    build_runlinkat(&t);
    CHECK_EQ(p9_dec_runlinkat(t.b, t.n), P9_OK);
    CHECK_EQ(p9_dec_rrenameat(t.b, t.n), P9_E_TYPE);
    /* An empty reply with a stray body byte is malformed. */
    build_rlink(&t);
    t.b[t.n++] = 0;
    tb_end(&t);
    CHECK_EQ(p9_dec_rlink(t.b, t.n), P9_E_PROTO);
}

static void test_layouts(void)
{
    tb_t t;
    uint32_t ecode = 0;
    build_rlerror(&t);
    CHECK_EQ(p9_dec_rlerror(t.b, t.n, &ecode), P9_OK);
    CHECK_EQ(ecode, 2);
    uint32_t msize;
    p9_str_t v;
    CHECK_EQ(p9_dec_rversion(t.b, t.n, &msize, &v), P9_E_TYPE);

    p9_qid_t qids[P9_MAXWELEM];
    uint16_t nq = 0;
    build_rwalk(&t);
    CHECK_EQ(p9_dec_rwalk(t.b, t.n, qids, P9_MAXWELEM, &nq), P9_OK);
    CHECK_EQ(nq, 2);
    CHECK_EQ(qids[0].type, P9_QTDIR);
    CHECK_EQ(qids[1].path, 0x2222);
    CHECK_EQ(p9_dec_rwalk(t.b, t.n, qids, 1, &nq), P9_E_PROTO); /* caller array too small */

    p9_qid_t q;
    uint32_t iounit = 0;
    build_rlopen(&t);
    CHECK_EQ(p9_dec_rlopen(t.b, t.n, &q, &iounit), P9_OK);
    CHECK(q.path == 0xABCDEF0123456789ull);
    CHECK_EQ(q.version, 3);
    CHECK_EQ(iounit, 4096);

    const uint8_t *data = NULL;
    uint32_t count = 0;
    build_rread(&t);
    CHECK_EQ(p9_dec_rread(t.b, t.n, &data, &count), P9_OK);
    CHECK(count == 5 && memcmp(data, "hello", 5) == 0);
    CHECK(data == t.b + 11);

    build_rreaddir(&t);
    CHECK_EQ(p9_dec_rreaddir(t.b, t.n, &data, &count), P9_OK);
    size_t pos = 0;
    p9_dirent_t de;
    CHECK_EQ(p9_dirent_next(data, count, &pos, &de), 1);
    CHECK(de.name.len == 1 && de.name.ptr[0] == '.');
    CHECK_EQ(de.type, 4);
    CHECK_EQ(de.offset, 1);
    CHECK_EQ(p9_dirent_next(data, count, &pos, &de), 1);
    CHECK(de.name.len == 8 && memcmp(de.name.ptr, "file.txt", 8) == 0);
    CHECK(de.offset == 0x7fffffffffffffffull);
    CHECK_EQ(p9_dirent_next(data, count, &pos, &de), 0);
    CHECK_EQ(pos, count);
    size_t bad = count + 1;
    CHECK_EQ(p9_dirent_next(data, count, &bad, &de), P9_E_PROTO);
    /* Every truncation of the entry data is an error, never an overrun. */
    for (size_t cut = 1; cut < count; cut++) {
        uint8_t *d = malloc(cut);
        memcpy(d, data, cut);
        size_t p = 0;
        int rc;
        while ((rc = p9_dirent_next(d, cut, &p, &de)) == 1)
            ;
        CHECK(rc == P9_E_PROTO || (rc == 0 && p == cut));
        free(d);
    }

    p9_attr_t a;
    build_rgetattr(&t);
    CHECK_EQ(t.n, 7 + 153); /* Rgetattr body is 153 bytes */
    CHECK_EQ(p9_dec_rgetattr(t.b, t.n, &a), P9_OK);
    CHECK_EQ(a.valid, P9_GETATTR_BASIC);
    CHECK_EQ(a.mode, 0100644);
    CHECK_EQ(a.uid, 1000);
    CHECK_EQ(a.gid, 1001);
    CHECK(a.nlink == 1 * 0x0101010101ull);
    CHECK(a.size == 3 * 0x0101010101ull);
    CHECK(a.mtime_nsec == 9 * 0x0101010101ull);
    CHECK(a.data_version == 15 * 0x0101010101ull);

    p9_statfs_t st;
    build_rstatfs(&t);
    CHECK_EQ(t.n, 7 + 60); /* Rstatfs body is 60 bytes */
    CHECK_EQ(p9_dec_rstatfs(t.b, t.n, &st), P9_OK);
    CHECK_EQ(st.bsize, 4096);
    CHECK_EQ(st.blocks, 1000);
    CHECK_EQ(st.fsid, 6000);
    CHECK_EQ(st.namelen, 255);

    p9_str_t target;
    build_rreadlink(&t);
    CHECK_EQ(p9_dec_rreadlink(t.b, t.n, &target), P9_OK);
    CHECK(target.len == 9 && memcmp(target.ptr, "../target", 9) == 0);
}

/* --- truncation and framing -------------------------------------------------- */

typedef int (*dec_fn)(const uint8_t *msg, size_t len);

static int d_rlerror(const uint8_t *m, size_t l)
{
    uint32_t e;
    return p9_dec_rlerror(m, l, &e);
}
static int d_rversion(const uint8_t *m, size_t l)
{
    uint32_t ms;
    p9_str_t v;
    return p9_dec_rversion(m, l, &ms, &v);
}
static int d_rattach(const uint8_t *m, size_t l)
{
    p9_qid_t q;
    return p9_dec_rattach(m, l, &q);
}
static int d_rwalk(const uint8_t *m, size_t l)
{
    p9_qid_t q[P9_MAXWELEM];
    uint16_t n;
    return p9_dec_rwalk(m, l, q, P9_MAXWELEM, &n);
}
static int d_rlopen(const uint8_t *m, size_t l)
{
    p9_qid_t q;
    uint32_t io;
    return p9_dec_rlopen(m, l, &q, &io);
}
static int d_rread(const uint8_t *m, size_t l)
{
    const uint8_t *d;
    uint32_t c;
    return p9_dec_rread(m, l, &d, &c);
}
static int d_rreaddir(const uint8_t *m, size_t l)
{
    const uint8_t *d;
    uint32_t c;
    return p9_dec_rreaddir(m, l, &d, &c);
}
static int d_rgetattr(const uint8_t *m, size_t l)
{
    p9_attr_t a;
    return p9_dec_rgetattr(m, l, &a);
}
static int d_rstatfs(const uint8_t *m, size_t l)
{
    p9_statfs_t s;
    return p9_dec_rstatfs(m, l, &s);
}
static int d_rreadlink(const uint8_t *m, size_t l)
{
    p9_str_t s;
    return p9_dec_rreadlink(m, l, &s);
}
static int d_rclunk(const uint8_t *m, size_t l)
{
    return p9_dec_rclunk(m, l);
}
static int d_rwrite(const uint8_t *m, size_t l)
{
    uint32_t c;
    return p9_dec_rwrite(m, l, &c);
}
static int d_rlcreate(const uint8_t *m, size_t l)
{
    p9_qid_t q;
    uint32_t io;
    return p9_dec_rlcreate(m, l, &q, &io);
}
static int d_rmkdir(const uint8_t *m, size_t l)
{
    p9_qid_t q;
    return p9_dec_rmkdir(m, l, &q);
}
static int d_rsymlink(const uint8_t *m, size_t l)
{
    p9_qid_t q;
    return p9_dec_rsymlink(m, l, &q);
}

struct dcase {
    void (*build)(tb_t *t);
    dec_fn dec;
};

static void build_rversion(tb_t *t)
{
    memcpy(t->b, g_rversion, sizeof g_rversion);
    t->n = sizeof g_rversion;
}
static void build_rattach(tb_t *t)
{
    memcpy(t->b, g_rattach, sizeof g_rattach);
    t->n = sizeof g_rattach;
}
static void build_rclunk(tb_t *t)
{
    memcpy(t->b, g_rclunk, sizeof g_rclunk);
    t->n = sizeof g_rclunk;
}

static const struct dcase cases[] = {
    {build_rlerror, d_rlerror},
    {build_rversion, d_rversion},
    {build_rattach, d_rattach},
    {build_rwalk, d_rwalk},
    {build_rlopen, d_rlopen},
    {build_rread, d_rread},
    {build_rreaddir, d_rreaddir},
    {build_rgetattr, d_rgetattr},
    {build_rstatfs, d_rstatfs},
    {build_rreadlink, d_rreadlink},
    {build_rclunk, d_rclunk},
    {build_rwrite, d_rwrite},
    {build_rlcreate, d_rlcreate},
    {build_rmkdir, d_rmkdir},
    {build_rsymlink, d_rsymlink},
    {build_rsetattr, p9_dec_rsetattr},
    {build_rfsync, p9_dec_rfsync},
    {build_rlink, p9_dec_rlink},
    {build_rrenameat, p9_dec_rrenameat},
    {build_runlinkat, p9_dec_runlinkat},
};

static void test_framing(void)
{
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        tb_t t;
        cases[c].build(&t);
        CHECK_EQ(cases[c].dec(t.b, t.n), P9_OK);

        for (size_t cut = 0; cut < t.n; cut++) {
            /* Buffer shorter than the size field claims. */
            uint8_t *m = malloc(cut ? cut : 1);
            memcpy(m, t.b, cut);
            CHECK_EQ(cases[c].dec(m, cut), P9_E_SHORT);
            /* Size field rewritten to match the truncated length. */
            if (cut >= 4) {
                for (int i = 0; i < 4; i++)
                    m[i] = (uint8_t)(cut >> (8 * i));
                int rc = cases[c].dec(m, cut);
                CHECK(rc == P9_E_PROTO || rc == P9_E_SHORT);
            }
            free(m);
        }

        /* Transport delivered more bytes than the message: still valid. */
        tb_t big = t;
        memset(big.b + big.n, 0xAA, 16);
        CHECK_EQ(cases[c].dec(big.b, big.n + 16), P9_OK);

        /* A trailing byte inside the size field is malformed. */
        tb_t trail = t;
        trail.b[trail.n++] = 0;
        tb_end(&trail);
        CHECK_EQ(cases[c].dec(trail.b, trail.n), P9_E_PROTO);
    }

    /* Size below the header size, and a NULL message. */
    uint8_t tiny[7] = {6, 0, 0, 0, P9_RCLUNK, 0, 0};
    CHECK_EQ(p9_dec_rclunk(tiny, sizeof tiny), P9_E_PROTO);
    CHECK_EQ(p9_dec_rclunk(NULL, 0), P9_E_SHORT);
}

static void test_bad_counts(void)
{
    tb_t t;
    p9_qid_t q[P9_MAXWELEM];
    uint16_t n;

    /* nwqid above MAXWELEM. */
    tb_begin(&t, P9_RWALK, 1);
    tb_le(&t, 17, 2);
    for (int i = 0; i < 17; i++)
        tb_qid(&t, 0, 0, (uint64_t)i);
    tb_end(&t);
    CHECK_EQ(p9_dec_rwalk(t.b, t.n, q, P9_MAXWELEM, &n), P9_E_PROTO);

    /* Rread count larger than the message. */
    const uint8_t *d;
    uint32_t c;
    tb_begin(&t, P9_RREAD, 1);
    tb_le(&t, 0xFFFFFFFFu, 4);
    tb_le(&t, 0, 4);
    tb_end(&t);
    CHECK_EQ(p9_dec_rread(t.b, t.n, &d, &c), P9_E_PROTO);

    /* String length larger than the message. */
    p9_str_t s;
    tb_begin(&t, P9_RREADLINK, 1);
    tb_le(&t, 0xFFFF, 2);
    tb_le(&t, 0, 2);
    tb_end(&t);
    CHECK_EQ(p9_dec_rreadlink(t.b, t.n, &s), P9_E_PROTO);
}

int main(void)
{
    test_golden();
    test_layouts();
    test_layouts_write();
    test_framing();
    test_bad_counts();
    return TEST_RESULT();
}
