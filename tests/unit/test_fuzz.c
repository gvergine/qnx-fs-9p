/*
 * Fuzz test for the lib9p decoders: mutated valid replies of every type
 * (bit flips, boundary values in length and count fields, truncation,
 * appended garbage, shuffled slices), then random bytes. Deterministic
 * (fixed seed) so failures reproduce; meant to run under -DFS9P_SANITIZE=ON,
 * where any out-of-bounds read aborts. Not coverage-guided: no clang/libFuzzer
 * on the development host.
 */
#include <stdlib.h>

#include "lib9p/p9.h"
#include "test.h"

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;

static uint32_t rnd(void)
{
    /* xorshift64* */
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return (uint32_t)((rng_state * 0x2545F4914F6CDD1Dull) >> 32);
}

static const uint8_t rtypes[] = {P9_RLERROR,  P9_RSTATFS,  P9_RLOPEN,   P9_RREADLINK, P9_RGETATTR,
                                 P9_RREADDIR, P9_RVERSION, P9_RATTACH,  P9_RWALK,     P9_RREAD,
                                 P9_RCLUNK,   P9_RWRITE,   P9_RLCREATE, P9_RMKDIR,    P9_RSYMLINK,
                                 P9_RSETATTR, P9_RFSYNC,   P9_RLINK,    P9_RRENAMEAT, P9_RUNLINKAT};

/* Checks that any view a decoder hands back lies inside the message. */
static bool inside(const uint8_t *msg, size_t len, const void *p, size_t n)
{
    const uint8_t *q = p;
    return q >= msg && n <= len && (size_t)(q - msg) <= len - n;
}

static void decode_all(const uint8_t *m, size_t len)
{
    uint32_t u;
    p9_str_t s;
    p9_qid_t q, qs[P9_MAXWELEM];
    uint16_t nq;
    const uint8_t *d;
    p9_attr_t a;
    p9_statfs_t st;

    (void)p9_dec_rlerror(m, len, &u);
    if (p9_dec_rversion(m, len, &u, &s) == P9_OK)
        CHECK(inside(m, len, s.ptr, s.len));
    (void)p9_dec_rattach(m, len, &q);
    if (p9_dec_rwalk(m, len, qs, P9_MAXWELEM, &nq) == P9_OK)
        CHECK(nq <= P9_MAXWELEM);
    (void)p9_dec_rlopen(m, len, &q, &u);
    if (p9_dec_rread(m, len, &d, &u) == P9_OK)
        CHECK(inside(m, len, d, u));
    if (p9_dec_rreaddir(m, len, &d, &u) == P9_OK) {
        CHECK(inside(m, len, d, u));
        size_t pos = 0;
        p9_dirent_t de;
        int rc;
        while ((rc = p9_dirent_next(d, u, &pos, &de)) == 1)
            CHECK(inside(d, u, de.name.ptr, de.name.len));
        CHECK(rc == 0 || rc == P9_E_PROTO);
    }
    (void)p9_dec_rgetattr(m, len, &a);
    (void)p9_dec_rstatfs(m, len, &st);
    if (p9_dec_rreadlink(m, len, &s) == P9_OK)
        CHECK(inside(m, len, s.ptr, s.len));
    (void)p9_dec_rclunk(m, len);
    (void)p9_dec_rwrite(m, len, &u);
    (void)p9_dec_rlcreate(m, len, &q, &u);
    (void)p9_dec_rmkdir(m, len, &q);
    (void)p9_dec_rsymlink(m, len, &q);
    (void)p9_dec_rsetattr(m, len);
    (void)p9_dec_rfsync(m, len);
    (void)p9_dec_rlink(m, len);
    (void)p9_dec_rrenameat(m, len);
    (void)p9_dec_runlinkat(m, len);
}

/* --- mutation stage ------------------------------------------------------------ */
/* Random bytes rarely get past the header checks; mutated valid replies
 * reach deep into every body, its nested lengths and counts. */

typedef void (*seed_fn)(tb_t *t);

static void s_rlerror(tb_t *t)
{
    tb_begin(t, P9_RLERROR, 1);
    tb_le(t, 2, 4);
    tb_end(t);
}
static void s_rversion(tb_t *t)
{
    tb_begin(t, P9_RVERSION, P9_NOTAG);
    tb_le(t, 8192, 4);
    tb_str(t, "9P2000.L");
    tb_end(t);
}
static void s_rwalk(tb_t *t)
{
    tb_begin(t, P9_RWALK, 1);
    tb_le(t, 3, 2);
    for (int i = 0; i < 3; i++)
        tb_qid(t, P9_QTDIR, (uint32_t)i, 0x100u + (uint64_t)i);
    tb_end(t);
}
static void s_rlopen(tb_t *t)
{
    tb_begin(t, P9_RLOPEN, 1);
    tb_qid(t, P9_QTFILE, 1, 2);
    tb_le(t, 4096, 4);
    tb_end(t);
}
static void s_rlcreate(tb_t *t)
{
    tb_begin(t, P9_RLCREATE, 1);
    tb_qid(t, P9_QTFILE, 1, 3);
    tb_le(t, 8192, 4);
    tb_end(t);
}
static void s_rread(tb_t *t)
{
    tb_begin(t, P9_RREAD, 1);
    tb_le(t, 16, 4);
    for (int i = 0; i < 16; i++)
        tb_u8(t, (uint8_t)i);
    tb_end(t);
}
static void s_rreaddir(tb_t *t)
{
    tb_begin(t, P9_RREADDIR, 1);
    size_t at = t->n;
    tb_le(t, 0, 4);
    size_t start = t->n;
    static const char *const names[] = {".", "..", "file.txt"};
    for (int i = 0; i < 3; i++) {
        tb_qid(t, i < 2 ? P9_QTDIR : P9_QTFILE, 0, (uint64_t)i);
        tb_le(t, (uint64_t)i + 1, 8);
        tb_u8(t, i < 2 ? 4 : 8);
        tb_str(t, names[i]);
    }
    size_t count = t->n - start;
    for (int i = 0; i < 4; i++)
        t->b[at + (size_t)i] = (uint8_t)(count >> (8 * i));
    tb_end(t);
}
static void s_rgetattr(tb_t *t)
{
    tb_begin(t, P9_RGETATTR, 1);
    tb_le(t, P9_GETATTR_BASIC, 8);
    tb_qid(t, P9_QTFILE, 1, 7);
    tb_le(t, 0100644, 4);
    tb_le(t, 1000, 4);
    tb_le(t, 1000, 4);
    for (int i = 0; i < 15; i++)
        tb_le(t, (uint64_t)i, 8);
    tb_end(t);
}
static void s_rstatfs(tb_t *t)
{
    tb_begin(t, P9_RSTATFS, 1);
    tb_le(t, 0x01021994, 4);
    tb_le(t, 4096, 4);
    for (int i = 0; i < 6; i++)
        tb_le(t, (uint64_t)i * 1000, 8);
    tb_le(t, 255, 4);
    tb_end(t);
}
static void s_rreadlink(tb_t *t)
{
    tb_begin(t, P9_RREADLINK, 1);
    tb_str(t, "../some/target");
    tb_end(t);
}
static void s_rwrite(tb_t *t)
{
    tb_begin(t, P9_RWRITE, 1);
    tb_le(t, 4096, 4);
    tb_end(t);
}
static void s_rmkdir(tb_t *t)
{
    tb_begin(t, P9_RMKDIR, 1);
    tb_qid(t, P9_QTDIR, 0, 9);
    tb_end(t);
}
static void s_rempty(tb_t *t)
{
    static const uint8_t types[] = {P9_RCLUNK, P9_RSETATTR,  P9_RFSYNC,
                                    P9_RLINK,  P9_RRENAMEAT, P9_RUNLINKAT};
    tb_begin(t, types[rnd() % sizeof types], 1);
    tb_end(t);
}

static const seed_fn seeds[] = {s_rlerror, s_rversion, s_rwalk,    s_rlopen,  s_rlcreate,
                                s_rread,   s_rreaddir, s_rgetattr, s_rstatfs, s_rreadlink,
                                s_rwrite,  s_rmkdir,   s_rempty};

/* Values that tend to break length arithmetic. */
static uint32_t boundary(size_t len)
{
    static const uint32_t v[] = {0, 1, 2, 6, 7, 0x7f, 0x80, 0xff, 0xffff, 0x7fffffff, 0xffffffff};
    if ((rnd() & 3) == 0)
        return (uint32_t)len + (rnd() % 5) - 2; /* around the real length */
    return v[rnd() % (sizeof v / sizeof v[0])];
}

static void mutate(tb_t *t)
{
    size_t len = t->n;
    switch (rnd() % 6) {
    case 0: /* flip a bit */
        if (len)
            t->b[rnd() % len] ^= (uint8_t)(1u << (rnd() % 8));
        break;
    case 1: /* a boundary byte */
        if (len)
            t->b[rnd() % len] = (uint8_t)boundary(len);
        break;
    case 2: /* a 2- or 4-byte field: counts and lengths */
        if (len >= 4) {
            size_t at = rnd() % (len - 3);
            uint32_t v = boundary(len);
            int w = (rnd() & 1) ? 2 : 4;
            for (int i = 0; i < w; i++)
                t->b[at + (size_t)i] = (uint8_t)(v >> (8 * i));
        }
        break;
    case 3: /* truncate, sometimes keeping the size field consistent */
        t->n = len ? rnd() % len : 0;
        if (t->n >= 4 && (rnd() & 1))
            tb_end(t);
        break;
    case 4: /* append garbage, sometimes covered by the size field */
        for (unsigned i = rnd() % 16; i > 0 && t->n < sizeof t->b; i--)
            t->b[t->n++] = (uint8_t)rnd();
        if (rnd() & 1)
            tb_end(t);
        break;
    default: /* duplicate a slice in place */
        if (len >= 2) {
            size_t a = rnd() % len, b = rnd() % len, n = rnd() % (len - (a > b ? a : b));
            memmove(t->b + b, t->b + a, n);
        }
        break;
    }
}

static void mutation_stage(void)
{
    for (int iter = 0; iter < 300000; iter++) {
        tb_t t;
        seeds[rnd() % (sizeof seeds / sizeof seeds[0])](&t);
        for (unsigned m = 1 + rnd() % 4; m > 0; m--)
            mutate(&t);
        uint8_t *msg = malloc(t.n ? t.n : 1); /* exact size: ASan sees overreads */
        memcpy(msg, t.b, t.n);
        decode_all(msg, t.n);
        free(msg);
    }
}

int main(void)
{
    mutation_stage();
    for (int iter = 0; iter < 200000; iter++) {
        size_t len = rnd() % 200;
        /* Exactly-sized heap buffer so ASan catches any overread. */
        uint8_t *m = malloc(len ? len : 1);
        for (size_t i = 0; i < len; i++)
            m[i] = (uint8_t)rnd();
        /* Mostly plausible headers, so bodies get exercised too. */
        if (len >= 7 && (rnd() & 3) != 0) {
            size_t size = (rnd() & 1) ? len : len - (rnd() % (len - 6));
            for (int i = 0; i < 4; i++)
                m[i] = (uint8_t)(size >> (8 * i));
            m[4] = rtypes[rnd() % sizeof rtypes];
            /* Small counts make nested length fields line up more often. */
            if (len >= 11 && (rnd() & 1))
                m[8] = m[9] = m[10] = 0;
        }
        decode_all(m, len);

        size_t pos = rnd() % (len + 2);
        p9_dirent_t de;
        int rc;
        while ((rc = p9_dirent_next(m, len, &pos, &de)) == 1)
            CHECK(inside(m, len, de.name.ptr, de.name.len));
        CHECK(rc == 0 || rc == P9_E_PROTO);
        free(m);
    }
    return TEST_RESULT();
}
