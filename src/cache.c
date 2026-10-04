/* fs-9p metadata cache; see cache.h for the consistency model. */
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cache.h"

typedef struct {
    char *path; /* NULL: unused */
    uint32_t hash;
    bool exists;
    p9_attr_t attr;
    uint64_t born_ms; /* CLOCK_MONOTONIC */
    int next;         /* next entry in the same bucket, -1 at the end */
} entry_t;

/* The one cache of the process (one mount per process). */
static struct {
    pthread_mutex_t lock;
    uint32_t ttl_ms; /* 0: disabled */
    unsigned cap;
    unsigned used;
    entry_t *e;
    int *bucket; /* nbuckets heads, -1 when empty */
    unsigned nbuckets;
} cache = {PTHREAD_MUTEX_INITIALIZER, 0, 0, 0, NULL, NULL, 0};

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* FNV-1a */
static uint32_t hash_path(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s != '\0') {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}

int fs9p_cache_init(uint32_t ttl_ms, unsigned max_entries)
{
    if (ttl_ms == 0 || max_entries == 0)
        return 0;
    unsigned nb = 1;
    while (nb < 2u * max_entries)
        nb <<= 1;
    /* Allocated once at startup and bounded; entry paths are allocated per
     * entry, at most max_entries of them. */
    cache.e = calloc(max_entries, sizeof *cache.e);
    cache.bucket = malloc(nb * sizeof *cache.bucket);
    if (cache.e == NULL || cache.bucket == NULL) {
        free(cache.e);
        free(cache.bucket);
        cache.e = NULL;
        cache.bucket = NULL;
        return -ENOMEM;
    }
    for (unsigned i = 0; i < nb; i++)
        cache.bucket[i] = -1;
    cache.nbuckets = nb;
    cache.cap = max_entries;
    cache.ttl_ms = ttl_ms;
    return 0;
}

bool fs9p_cache_enabled(void)
{
    return cache.ttl_ms != 0;
}

/* --- with the lock held -------------------------------------------------------- */

static int find(const char *path, uint32_t h)
{
    for (int i = cache.bucket[h & (cache.nbuckets - 1)]; i >= 0; i = cache.e[i].next)
        if (cache.e[i].hash == h && strcmp(cache.e[i].path, path) == 0)
            return i;
    return -1;
}

static void drop(int i)
{
    entry_t *x = &cache.e[i];
    int *link = &cache.bucket[x->hash & (cache.nbuckets - 1)];
    while (*link != i)
        link = &cache.e[*link].next;
    *link = x->next;
    free(x->path);
    x->path = NULL;
    cache.used--;
}

/* A free entry, evicting the oldest one when the cache is full. */
static int take_free(void)
{
    int oldest = -1;
    for (unsigned i = 0; i < cache.cap; i++) {
        if (cache.e[i].path == NULL)
            return (int)i;
        if (oldest < 0 || cache.e[i].born_ms < cache.e[oldest].born_ms)
            oldest = (int)i;
    }
    drop(oldest);
    return oldest;
}

static void store(const char *path, bool exists, const p9_attr_t *attr)
{
    uint32_t h = hash_path(path);
    int i = find(path, h);
    if (i < 0) {
        char *copy = strdup(path);
        if (copy == NULL)
            return; /* not caching is always correct */
        i = take_free();
        entry_t *x = &cache.e[i];
        x->path = copy;
        x->hash = h;
        x->next = cache.bucket[h & (cache.nbuckets - 1)];
        cache.bucket[h & (cache.nbuckets - 1)] = i;
        cache.used++;
    }
    entry_t *x = &cache.e[i];
    x->exists = exists;
    if (attr != NULL)
        x->attr = *attr;
    x->born_ms = now_ms();
}

/* --- public -------------------------------------------------------------------- */

bool fs9p_cache_get(const char *path, bool *exists, p9_attr_t *attr)
{
    if (!fs9p_cache_enabled())
        return false;
    bool hit = false;
    pthread_mutex_lock(&cache.lock);
    int i = find(path, hash_path(path));
    if (i >= 0 && now_ms() - cache.e[i].born_ms > cache.ttl_ms) {
        drop(i);
        i = -1;
    }
    if (i >= 0) {
        *exists = cache.e[i].exists;
        if (cache.e[i].exists && attr != NULL)
            *attr = cache.e[i].attr;
        hit = true;
    }
    pthread_mutex_unlock(&cache.lock);
    return hit;
}

void fs9p_cache_put(const char *path, const p9_attr_t *attr)
{
    if (!fs9p_cache_enabled())
        return;
    pthread_mutex_lock(&cache.lock);
    store(path, true, attr);
    pthread_mutex_unlock(&cache.lock);
}

void fs9p_cache_put_missing(const char *path)
{
    if (!fs9p_cache_enabled())
        return;
    pthread_mutex_lock(&cache.lock);
    store(path, false, NULL);
    pthread_mutex_unlock(&cache.lock);
}

void fs9p_cache_forget(const char *path)
{
    if (!fs9p_cache_enabled())
        return;
    pthread_mutex_lock(&cache.lock);
    int i = find(path, hash_path(path));
    if (i >= 0)
        drop(i);
    pthread_mutex_unlock(&cache.lock);
}

void fs9p_cache_forget_tree(const char *path)
{
    if (!fs9p_cache_enabled())
        return;
    size_t n = strlen(path);
    pthread_mutex_lock(&cache.lock);
    for (unsigned i = 0; i < cache.cap; i++) {
        const char *p = cache.e[i].path;
        if (p != NULL && (n == 0 || (strncmp(p, path, n) == 0 && (p[n] == '\0' || p[n] == '/'))))
            drop((int)i);
    }
    pthread_mutex_unlock(&cache.lock);
}

void fs9p_cache_forget_parent(const char *path)
{
    if (!fs9p_cache_enabled())
        return;
    const char *slash = strrchr(path, '/');
    char parent[PATH_MAX];
    size_t n = slash ? (size_t)(slash - path) : 0;
    if (n >= sizeof parent) {
        fs9p_cache_forget_tree(""); /* can't name it: drop everything */
        return;
    }
    memcpy(parent, path, n);
    parent[n] = '\0';
    fs9p_cache_forget(parent);
}
