/* Minimal assert harness for host unit tests. Not part of lib9p. */
#ifndef FS9P_TEST_H
#define FS9P_TEST_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int test_failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);               \
            test_failures++;                                                                       \
        }                                                                                          \
    } while (0)

#define CHECK_EQ(a, b)                                                                             \
    do {                                                                                           \
        long long a_ = (long long)(a), b_ = (long long)(b);                                        \
        if (a_ != b_) {                                                                            \
            fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s (%lld != %lld)\n", __FILE__,         \
                    __LINE__, #a, #b, a_, b_);                                                     \
            test_failures++;                                                                       \
        }                                                                                          \
    } while (0)

#define CHECK_MEM(a, b, n) CHECK(memcmp((a), (b), (n)) == 0)

#define TEST_RESULT() (test_failures == 0 ? 0 : 1)

/* Test-side message builder: independent of lib9p's own wire code, so the
 * decoders are not tested against themselves. */
typedef struct {
    uint8_t b[1024];
    size_t n;
} tb_t;

static inline void tb_u8(tb_t *t, uint8_t v)
{
    t->b[t->n++] = v;
}

static inline void tb_le(tb_t *t, uint64_t v, int bytes)
{
    for (int i = 0; i < bytes; i++)
        t->b[t->n++] = (uint8_t)(v >> (8 * i));
}

static inline void tb_str(tb_t *t, const char *s)
{
    size_t n = strlen(s);
    tb_le(t, n, 2);
    memcpy(t->b + t->n, s, n);
    t->n += n;
}

static inline void tb_qid(tb_t *t, uint8_t type, uint32_t version, uint64_t path)
{
    tb_u8(t, type);
    tb_le(t, version, 4);
    tb_le(t, path, 8);
}

static inline void tb_begin(tb_t *t, uint8_t type, uint16_t tag)
{
    t->n = 0;
    tb_le(t, 0, 4);
    tb_u8(t, type);
    tb_le(t, tag, 2);
}

static inline void tb_end(tb_t *t)
{
    for (int i = 0; i < 4; i++)
        t->b[i] = (uint8_t)(t->n >> (8 * i));
}

#endif /* FS9P_TEST_H */
