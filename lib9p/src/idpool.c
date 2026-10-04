/* lib9p — bitmap id allocator for fids and tags. */
#include "lib9p/p9.h"

#include <string.h>

int p9_idpool_init(p9_idpool_t *pool, uint32_t *words, uint32_t nids)
{
    if (pool == NULL || words == NULL || nids == 0 || nids > 0xFFFFFFFEu)
        return P9_E_INVAL;
    memset(words, 0, (size_t)P9_IDPOOL_WORDS(nids) * sizeof *words);
    pool->words = words;
    pool->nids = nids;
    pool->next = 0;
    pool->used = 0;
    return P9_OK;
}

static bool bit(const p9_idpool_t *pool, uint32_t id)
{
    return (pool->words[id / 32u] >> (id % 32u)) & 1u;
}

int p9_idpool_alloc(p9_idpool_t *pool, uint32_t *id)
{
    if (pool->used == pool->nids)
        return P9_E_EXHAUSTED;
    /* Round-robin from `next`; a free id exists, so this terminates within
     * nids steps. Linear per call is fine for the pool sizes we use. */
    uint32_t i = pool->next;
    while (bit(pool, i))
        i = (i + 1u == pool->nids) ? 0u : i + 1u;
    pool->words[i / 32u] |= 1u << (i % 32u);
    pool->used++;
    pool->next = (i + 1u == pool->nids) ? 0u : i + 1u;
    *id = i;
    return P9_OK;
}

int p9_idpool_free(p9_idpool_t *pool, uint32_t id)
{
    if (id >= pool->nids || !bit(pool, id))
        return P9_E_INVAL;
    pool->words[id / 32u] &= ~(1u << (id % 32u));
    pool->used--;
    return P9_OK;
}

bool p9_idpool_in_use(const p9_idpool_t *pool, uint32_t id)
{
    return id < pool->nids && bit(pool, id);
}

uint32_t p9_idpool_used(const p9_idpool_t *pool)
{
    return pool->used;
}
