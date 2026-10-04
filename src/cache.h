/*
 * Metadata cache (-o cache=MS): attributes of recently looked-up
 * paths, and paths found missing, kept for at most MS milliseconds. Keys
 * are paths relative to the mount point, as the connect messages carry
 * them ("" is the root). Off (MS = 0) by default: then every call is a
 * no-op and every lookup asks the server.
 *
 * Consistency model: changes made through fs-9p update or drop the entries
 * they affect at once; changes made on the host show within MS. File data
 * is never cached.
 *
 * Thread-safe (one mutex). Bounded: the oldest entry goes when it is full.
 */
#ifndef FS9P_CACHE_H
#define FS9P_CACHE_H

#include <stdbool.h>
#include <stdint.h>

#include "lib9p/p9.h"

/* ttl_ms 0 disables the cache. Returns 0 or -errno. */
int fs9p_cache_init(uint32_t ttl_ms, unsigned max_entries);
bool fs9p_cache_enabled(void);

/* A fresh entry for path: true and *exists (and *attr when it exists), or
 * false when there is none or it has expired. */
bool fs9p_cache_get(const char *path, bool *exists, p9_attr_t *attr);

void fs9p_cache_put(const char *path, const p9_attr_t *attr);
void fs9p_cache_put_missing(const char *path);

/* Drop the entry for path. */
void fs9p_cache_forget(const char *path);
/* Drop the entry for path and every entry below it (a directory renamed
 * or removed). */
void fs9p_cache_forget_tree(const char *path);
/* Drop the entry for path's directory (its mtime, nlink or listing
 * changed). */
void fs9p_cache_forget_parent(const char *path);

#endif /* FS9P_CACHE_H */
