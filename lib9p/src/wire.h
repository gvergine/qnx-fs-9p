/*
 * Internal byte cursors for lib9p. Explicit little-endian, byte by byte;
 * never casts structs onto buffers. Both cursors have a sticky failure flag:
 * once an access would cross the end, nothing more is read or written and
 * the caller checks the flag once at the end.
 */
#ifndef LIB9P_WIRE_H
#define LIB9P_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lib9p/p9.h"

/* --- writer ---------------------------------------------------------------- */

typedef struct {
    uint8_t *p;
    size_t cap;
    size_t pos;
    bool fail;
} p9_w_t;

static inline uint8_t *w_need(p9_w_t *w, size_t n)
{
    if (w->fail || w->cap - w->pos < n) {
        w->fail = true;
        return NULL;
    }
    uint8_t *q = w->p + w->pos;
    w->pos += n;
    return q;
}

static inline void w_u8(p9_w_t *w, uint8_t v)
{
    uint8_t *q = w_need(w, 1);
    if (q)
        q[0] = v;
}

static inline void w_u16(p9_w_t *w, uint16_t v)
{
    uint8_t *q = w_need(w, 2);
    if (q) {
        q[0] = (uint8_t)v;
        q[1] = (uint8_t)(v >> 8);
    }
}

static inline void w_u32(p9_w_t *w, uint32_t v)
{
    uint8_t *q = w_need(w, 4);
    if (q)
        for (int i = 0; i < 4; i++)
            q[i] = (uint8_t)(v >> (8 * i));
}

static inline void w_u64(p9_w_t *w, uint64_t v)
{
    uint8_t *q = w_need(w, 8);
    if (q)
        for (int i = 0; i < 8; i++)
            q[i] = (uint8_t)(v >> (8 * i));
}

/* Caller must have checked s.len <= UINT16_MAX. */
static inline void w_str(p9_w_t *w, p9_str_t s)
{
    w_u16(w, (uint16_t)s.len);
    uint8_t *q = w_need(w, s.len);
    if (q && s.len)
        memcpy(q, s.ptr, s.len);
}

/* Start a message: reserve size[4], then type[1] tag[2]. */
static inline void w_begin(p9_w_t *w, uint8_t *buf, size_t cap, uint8_t type, uint16_t tag)
{
    w->p = buf;
    w->cap = cap;
    w->pos = 0;
    w->fail = false;
    w_u32(w, 0);
    w_u8(w, type);
    w_u16(w, tag);
}

/* Patch size[4] and return the message length, or P9_E_NOSPACE. */
static inline int32_t w_finish(p9_w_t *w)
{
    if (w->fail)
        return P9_E_NOSPACE;
    if (w->pos > INT32_MAX)
        return P9_E_INVAL;
    uint32_t n = (uint32_t)w->pos;
    for (int i = 0; i < 4; i++)
        w->p[i] = (uint8_t)(n >> (8 * i));
    return (int32_t)n;
}

/* --- reader ---------------------------------------------------------------- */

typedef struct {
    const uint8_t *p;
    size_t end; /* one past the last readable byte */
    size_t pos;
    bool fail;
} p9_r_t;

static inline const uint8_t *r_need(p9_r_t *r, size_t n)
{
    if (r->fail || r->end - r->pos < n) {
        r->fail = true;
        return NULL;
    }
    const uint8_t *q = r->p + r->pos;
    r->pos += n;
    return q;
}

static inline uint8_t r_u8(p9_r_t *r)
{
    const uint8_t *q = r_need(r, 1);
    return q ? q[0] : 0;
}

static inline uint16_t r_u16(p9_r_t *r)
{
    const uint8_t *q = r_need(r, 2);
    return q ? (uint16_t)(q[0] | (q[1] << 8)) : 0;
}

static inline uint32_t r_u32(p9_r_t *r)
{
    const uint8_t *q = r_need(r, 4);
    if (!q)
        return 0;
    return (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
}

static inline uint64_t r_u64(p9_r_t *r)
{
    const uint8_t *q = r_need(r, 8);
    if (!q)
        return 0;
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | q[i];
    return v;
}

/* String view into the buffer; the length field is bounds-checked. */
static inline p9_str_t r_str(p9_r_t *r)
{
    p9_str_t s = {"", 0};
    uint16_t n = r_u16(r);
    const uint8_t *q = r_need(r, n);
    if (q) {
        s.ptr = (const char *)q;
        s.len = n;
    }
    return s;
}

static inline p9_qid_t r_qid(p9_r_t *r)
{
    p9_qid_t q;
    q.type = r_u8(r);
    q.version = r_u32(r);
    q.path = r_u64(r);
    return q;
}

#endif /* LIB9P_WIRE_H */
