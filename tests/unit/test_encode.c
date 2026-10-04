/* lib9p encoder tests: golden vectors, exact-capacity behaviour, bad args. */
#include <stdlib.h>

#include "lib9p/p9.h"
#include "test.h"

/* Sent by the first virtio test program and accepted by QEMU 10.2.1 (v9fs trace:
 * "v9fs_version tag 65535 id 100 msize 8192 version 9P2000.L"). */
static const uint8_t g_tversion[] = {0x15, 0x00, 0x00, 0x00, 0x64, 0xff, 0xff,
                                     0x00, 0x20, 0x00, 0x00, 0x08, 0x00, '9',
                                     'P',  '2',  '0',  '0',  '0',  '.',  'L'};
/* "v9fs_attach tag 1 id 104 fid 0 afid -1 uname  aname" */
static const uint8_t g_tattach[] = {0x17, 0x00, 0x00, 0x00, 0x68, 0x01, 0x00, 0x00,
                                    0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x00,
                                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
/* "v9fs_clunk tag 2 id 120 fid 0" */
static const uint8_t g_tclunk[] = {0x0b, 0x00, 0x00, 0x00, 0x78, 0x02,
                                   0x00, 0x00, 0x00, 0x00, 0x00};
/* Tread written from the layout: size[4] type=116 tag=3 fid=1
 * offset=0x0102030405060708 count=0x2000. */
static const uint8_t g_tread[] = {0x17, 0x00, 0x00, 0x00, 0x74, 0x03, 0x00, 0x01,
                                  0x00, 0x00, 0x00, 0x08, 0x07, 0x06, 0x05, 0x04,
                                  0x03, 0x02, 0x01, 0x00, 0x20, 0x00, 0x00};

static void test_golden(void)
{
    uint8_t buf[256];
    int32_t n;

    n = p9_enc_tversion(buf, sizeof buf, P9_NOTAG, 8192, p9_str(P9_PROTO_VERSION));
    CHECK_EQ(n, sizeof g_tversion);
    CHECK_MEM(buf, g_tversion, sizeof g_tversion);

    n = p9_enc_tattach(buf, sizeof buf, 1, 0, P9_NOFID, p9_str(""), p9_str(""), 0);
    CHECK_EQ(n, sizeof g_tattach);
    CHECK_MEM(buf, g_tattach, sizeof g_tattach);

    n = p9_enc_tclunk(buf, sizeof buf, 2, 0);
    CHECK_EQ(n, sizeof g_tclunk);
    CHECK_MEM(buf, g_tclunk, sizeof g_tclunk);

    n = p9_enc_tread(buf, sizeof buf, 3, 1, 0x0102030405060708ull, 0x2000);
    CHECK_EQ(n, sizeof g_tread);
    CHECK_MEM(buf, g_tread, sizeof g_tread);
}

static void test_layouts(void)
{
    uint8_t buf[512];
    tb_t t;

    /* Twalk fid[4] newfid[4] nwname[2] wname[s]... */
    p9_str_t names[2] = {p9_str("usr"), p9_str("bin")};
    int32_t n = p9_enc_twalk(buf, sizeof buf, 7, 1, 2, 2, names);
    tb_begin(&t, P9_TWALK, 7);
    tb_le(&t, 1, 4);
    tb_le(&t, 2, 4);
    tb_le(&t, 2, 2);
    tb_str(&t, "usr");
    tb_str(&t, "bin");
    tb_end(&t);
    CHECK_EQ(n, t.n);
    CHECK_MEM(buf, t.b, t.n);

    /* Twalk with nwname 0 clones the fid. */
    n = p9_enc_twalk(buf, sizeof buf, 7, 1, 2, 0, NULL);
    CHECK_EQ(n, 17);

    n = p9_enc_tlopen(buf, sizeof buf, 4, 9, P9_DOTL_RDONLY | P9_DOTL_DIRECTORY);
    tb_begin(&t, P9_TLOPEN, 4);
    tb_le(&t, 9, 4);
    tb_le(&t, 0200000, 4); /* Linux O_DIRECTORY, generic octal value */
    tb_end(&t);
    CHECK_EQ(n, t.n);
    CHECK_MEM(buf, t.b, t.n);

    n = p9_enc_treaddir(buf, sizeof buf, 5, 9, 42, 4096);
    tb_begin(&t, P9_TREADDIR, 5);
    tb_le(&t, 9, 4);
    tb_le(&t, 42, 8);
    tb_le(&t, 4096, 4);
    tb_end(&t);
    CHECK_EQ(n, t.n);
    CHECK_MEM(buf, t.b, t.n);

    n = p9_enc_tgetattr(buf, sizeof buf, 6, 9, P9_GETATTR_BASIC);
    tb_begin(&t, P9_TGETATTR, 6);
    tb_le(&t, 9, 4);
    tb_le(&t, 0x7ff, 8);
    tb_end(&t);
    CHECK_EQ(n, t.n);
    CHECK_MEM(buf, t.b, t.n);

    n = p9_enc_tstatfs(buf, sizeof buf, 8, 9);
    CHECK_EQ(n, 11);
    CHECK_EQ(buf[4], P9_TSTATFS);
    n = p9_enc_treadlink(buf, sizeof buf, 8, 9);
    CHECK_EQ(n, 11);
    CHECK_EQ(buf[4], P9_TREADLINK);
}

/* Write and namespace messages, written from the Linux client's layouts. */
static void test_layouts_write(void)
{
    uint8_t buf[512];
    tb_t t;
    int32_t n;

    /* Twrite: header only; the payload area is reserved, never written. */
    memset(buf, 0xEE, sizeof buf);
    n = p9_enc_twrite(buf, sizeof buf, 3, 5, 0x1122334455667788ull, 4);
    tb_begin(&t, P9_TWRITE, 3);
    tb_le(&t, 5, 4);
    tb_le(&t, 0x1122334455667788ull, 8);
    tb_le(&t, 4, 4);
    for (int i = 0; i < 4; i++)
        tb_u8(&t, 0xEE);
    tb_end(&t);
    CHECK_EQ(n, P9_TWRITE_OVERHEAD + 4);
    CHECK_EQ(n, t.n);
    CHECK_MEM(buf, t.b, t.n);
    CHECK_EQ(buf[n], 0xEE);
    /* The payload must fit too, not just the header. */
    CHECK_EQ(p9_enc_twrite(buf, P9_TWRITE_OVERHEAD + 3, 3, 5, 0, 4), P9_E_NOSPACE);
    CHECK_EQ(p9_enc_twrite(buf, P9_TWRITE_OVERHEAD, 3, 5, 0, 0), (int32_t)P9_TWRITE_OVERHEAD);

    n = p9_enc_tlcreate(buf, sizeof buf, 4, 6, p9_str("new.txt"),
                        P9_DOTL_WRONLY | P9_DOTL_CREATE | P9_DOTL_TRUNC, 0644, 1000);
    tb_begin(&t, P9_TLCREATE, 4);
    tb_le(&t, 6, 4);
    tb_str(&t, "new.txt");
    tb_le(&t, 01 | 0100 | 01000, 4); /* Linux O_WRONLY|O_CREAT|O_TRUNC, octal */
    tb_le(&t, 0644, 4);
    tb_le(&t, 1000, 4);
    tb_end(&t);
    CHECK_EQ(n, t.n);
    CHECK_MEM(buf, t.b, t.n);

    p9_setattr_t sa = {P9_SETATTR_MODE | P9_SETATTR_SIZE | P9_SETATTR_MTIME_SET,
                       0755,
                       11,
                       12,
                       0x100000000ull,
                       21,
                       22,
                       31,
                       32};
    n = p9_enc_tsetattr(buf, sizeof buf, 5, 7, &sa);
    tb_begin(&t, P9_TSETATTR, 5);
    tb_le(&t, 7, 4);
    tb_le(&t, 0x1 | 0x8 | 0x100, 4);
    tb_le(&t, 0755, 4);
    tb_le(&t, 11, 4);
    tb_le(&t, 12, 4);
    tb_le(&t, 0x100000000ull, 8);
    tb_le(&t, 21, 8);
    tb_le(&t, 22, 8);
    tb_le(&t, 31, 8);
    tb_le(&t, 32, 8);
    tb_end(&t);
    CHECK_EQ(n, t.n);
    CHECK_EQ(n, 7 + 60); /* Tsetattr body is 60 bytes */
    CHECK_MEM(buf, t.b, t.n);
    CHECK_EQ(p9_enc_tsetattr(buf, sizeof buf, 5, 7, NULL), P9_E_INVAL);

    n = p9_enc_tfsync(buf, sizeof buf, 6, 8, 1);
    tb_begin(&t, P9_TFSYNC, 6);
    tb_le(&t, 8, 4);
    tb_le(&t, 1, 4);
    tb_end(&t);
    CHECK_EQ(n, t.n);
    CHECK_MEM(buf, t.b, t.n);

    n = p9_enc_tmkdir(buf, sizeof buf, 7, 9, p9_str("dir"), 0755, 100);
    tb_begin(&t, P9_TMKDIR, 7);
    tb_le(&t, 9, 4);
    tb_str(&t, "dir");
    tb_le(&t, 0755, 4);
    tb_le(&t, 100, 4);
    tb_end(&t);
    CHECK_EQ(n, t.n);
    CHECK_MEM(buf, t.b, t.n);

    n = p9_enc_tsymlink(buf, sizeof buf, 8, 9, p9_str("ln"), p9_str("../target"), 100);
    tb_begin(&t, P9_TSYMLINK, 8);
    tb_le(&t, 9, 4);
    tb_str(&t, "ln");
    tb_str(&t, "../target");
    tb_le(&t, 100, 4);
    tb_end(&t);
    CHECK_EQ(n, t.n);
    CHECK_MEM(buf, t.b, t.n);

    n = p9_enc_tlink(buf, sizeof buf, 9, 9, 10, p9_str("hard"));
    tb_begin(&t, P9_TLINK, 9);
    tb_le(&t, 9, 4);
    tb_le(&t, 10, 4);
    tb_str(&t, "hard");
    tb_end(&t);
    CHECK_EQ(n, t.n);
    CHECK_MEM(buf, t.b, t.n);

    n = p9_enc_trenameat(buf, sizeof buf, 10, 9, p9_str("a"), 11, p9_str("bb"));
    tb_begin(&t, P9_TRENAMEAT, 10);
    tb_le(&t, 9, 4);
    tb_str(&t, "a");
    tb_le(&t, 11, 4);
    tb_str(&t, "bb");
    tb_end(&t);
    CHECK_EQ(n, t.n);
    CHECK_MEM(buf, t.b, t.n);

    n = p9_enc_tunlinkat(buf, sizeof buf, 11, 9, p9_str("sub"), P9_DOTL_AT_REMOVEDIR);
    tb_begin(&t, P9_TUNLINKAT, 11);
    tb_le(&t, 9, 4);
    tb_str(&t, "sub");
    tb_le(&t, 0x200, 4);
    tb_end(&t);
    CHECK_EQ(n, t.n);
    CHECK_MEM(buf, t.b, t.n);
}

/* Each encoder must fail cleanly for every capacity below its length and
 * must not write past the capacity it was given. */
typedef int32_t (*enc_fn)(uint8_t *buf, size_t cap);

static int32_t e_version(uint8_t *b, size_t c)
{
    return p9_enc_tversion(b, c, P9_NOTAG, 8192, p9_str(P9_PROTO_VERSION));
}
static int32_t e_attach(uint8_t *b, size_t c)
{
    return p9_enc_tattach(b, c, 1, 0, P9_NOFID, p9_str("root"), p9_str("/srv"), 0);
}
static int32_t e_walk(uint8_t *b, size_t c)
{
    p9_str_t names[3] = {p9_str("a"), p9_str("bb"), p9_str("ccc")};
    return p9_enc_twalk(b, c, 1, 0, 1, 3, names);
}
static int32_t e_lopen(uint8_t *b, size_t c)
{
    return p9_enc_tlopen(b, c, 1, 0, 0);
}
static int32_t e_read(uint8_t *b, size_t c)
{
    return p9_enc_tread(b, c, 1, 0, 0, 1);
}
static int32_t e_readdir(uint8_t *b, size_t c)
{
    return p9_enc_treaddir(b, c, 1, 0, 0, 1);
}
static int32_t e_getattr(uint8_t *b, size_t c)
{
    return p9_enc_tgetattr(b, c, 1, 0, P9_GETATTR_ALL);
}
static int32_t e_statfs(uint8_t *b, size_t c)
{
    return p9_enc_tstatfs(b, c, 1, 0);
}
static int32_t e_readlink(uint8_t *b, size_t c)
{
    return p9_enc_treadlink(b, c, 1, 0);
}
static int32_t e_clunk(uint8_t *b, size_t c)
{
    return p9_enc_tclunk(b, c, 1, 0);
}

static int32_t e_write(uint8_t *b, size_t c)
{
    return p9_enc_twrite(b, c, 1, 0, 0, 3);
}
static int32_t e_lcreate(uint8_t *b, size_t c)
{
    return p9_enc_tlcreate(b, c, 1, 0, p9_str("f"), 0, 0644, 0);
}
static int32_t e_setattr(uint8_t *b, size_t c)
{
    p9_setattr_t sa = {0};
    return p9_enc_tsetattr(b, c, 1, 0, &sa);
}
static int32_t e_fsync(uint8_t *b, size_t c)
{
    return p9_enc_tfsync(b, c, 1, 0, 0);
}
static int32_t e_mkdir(uint8_t *b, size_t c)
{
    return p9_enc_tmkdir(b, c, 1, 0, p9_str("d"), 0755, 0);
}
static int32_t e_symlink(uint8_t *b, size_t c)
{
    return p9_enc_tsymlink(b, c, 1, 0, p9_str("l"), p9_str("t"), 0);
}
static int32_t e_link(uint8_t *b, size_t c)
{
    return p9_enc_tlink(b, c, 1, 0, 1, p9_str("h"));
}
static int32_t e_renameat(uint8_t *b, size_t c)
{
    return p9_enc_trenameat(b, c, 1, 0, p9_str("a"), 1, p9_str("b"));
}
static int32_t e_unlinkat(uint8_t *b, size_t c)
{
    return p9_enc_tunlinkat(b, c, 1, 0, p9_str("u"), 0);
}

static void test_capacity(void)
{
    static const enc_fn fns[] = {e_version, e_attach,  e_walk,     e_lopen,    e_read,
                                 e_readdir, e_getattr, e_statfs,   e_readlink, e_clunk,
                                 e_write,   e_lcreate, e_setattr,  e_fsync,    e_mkdir,
                                 e_symlink, e_link,    e_renameat, e_unlinkat};
    for (size_t f = 0; f < sizeof fns / sizeof fns[0]; f++) {
        /* Zeroed: Twrite reserves its payload without writing it. */
        uint8_t full[256] = {0};
        int32_t need = fns[f](full, sizeof full);
        CHECK(need > 0);
        for (size_t cap = 0; cap < (size_t)need; cap++) {
            /* Exactly-sized heap buffer so ASan catches any overrun. */
            uint8_t *b = malloc(cap ? cap : 1);
            CHECK_EQ(fns[f](b, cap), P9_E_NOSPACE);
            free(b);
        }
        uint8_t *b = calloc(1, (size_t)need);
        CHECK_EQ(fns[f](b, (size_t)need), need);
        CHECK_MEM(b, full, (size_t)need);
        free(b);
    }
}

static void test_invalid(void)
{
    uint8_t buf[1024];
    p9_str_t names[P9_MAXWELEM + 1];
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        names[i] = p9_str("x");
    CHECK(p9_enc_twalk(buf, sizeof buf, 1, 0, 1, P9_MAXWELEM, names) > 0);
    CHECK_EQ(p9_enc_twalk(buf, sizeof buf, 1, 0, 1, P9_MAXWELEM + 1, names), P9_E_INVAL);
    CHECK_EQ(p9_enc_twalk(buf, sizeof buf, 1, 0, 1, 1, NULL), P9_E_INVAL);

    static char big[70000];
    memset(big, 'a', sizeof big);
    p9_str_t too_long = {big, 65536};
    CHECK_EQ(p9_enc_tversion(buf, sizeof buf, 0, 1, too_long), P9_E_INVAL);
    p9_str_t null_str = {NULL, 1};
    CHECK_EQ(p9_enc_tattach(buf, sizeof buf, 0, 0, 0, null_str, p9_str(""), 0), P9_E_INVAL);

    /* Every name argument of the write and namespace encoders is checked. */
    p9_str_t x = p9_str("x");
    CHECK_EQ(p9_enc_tlcreate(buf, sizeof buf, 0, 0, too_long, 0, 0, 0), P9_E_INVAL);
    CHECK_EQ(p9_enc_tmkdir(buf, sizeof buf, 0, 0, null_str, 0, 0), P9_E_INVAL);
    CHECK_EQ(p9_enc_tsymlink(buf, sizeof buf, 0, 0, x, too_long, 0), P9_E_INVAL);
    CHECK_EQ(p9_enc_tsymlink(buf, sizeof buf, 0, 0, null_str, x, 0), P9_E_INVAL);
    CHECK_EQ(p9_enc_tlink(buf, sizeof buf, 0, 0, 1, too_long), P9_E_INVAL);
    CHECK_EQ(p9_enc_trenameat(buf, sizeof buf, 0, 0, x, 1, null_str), P9_E_INVAL);
    CHECK_EQ(p9_enc_trenameat(buf, sizeof buf, 0, 0, too_long, 1, x), P9_E_INVAL);
    CHECK_EQ(p9_enc_tunlinkat(buf, sizeof buf, 0, 0, null_str, 0), P9_E_INVAL);
}

int main(void)
{
    test_golden();
    test_layouts();
    test_layouts_write();
    test_capacity();
    test_invalid();
    return TEST_RESULT();
}
