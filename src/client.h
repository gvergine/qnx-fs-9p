/*
 * fs-9p client session: synchronous 9P2000.L calls over a transport.
 *
 * Each call takes one of the transport's slots for its whole encode / rpc /
 * decode sequence (the reply lives in the slot's buffer until then), so
 * calls from different threads run concurrently, up to the number of
 * slots. Calls return 0 or -errno (QNX values; Rlerror codes are mapped
 * through errno_map).
 */
#ifndef FS9P_CLIENT_H
#define FS9P_CLIENT_H

#include <pthread.h>
#include <stdint.h>

#include "lib9p/p9.h"
#include "transport.h"

#define FS9P_MAX_FIDS 1024u

typedef struct {
    fs9p_transport_t tp;
    pthread_mutex_t fid_lock; /* guards `fids` and the deferred clunks */
    uint32_t msize;           /* negotiated */
    uint32_t root_fid;
    p9_qid_t root_qid;
    p9_idpool_t fids;
    uint32_t fid_words[P9_IDPOOL_WORDS(FS9P_MAX_FIDS)];
    /* Fids whose Tclunk couldn't get a slot (all held by timed-out
     * requests): the next call that gets a slot clunks one first. */
    uint32_t deferred[FS9P_MAX_FIDS];
    unsigned ndeferred;
    int debug; /* hex-dump every message to stderr */
} fs9p_session_t;

/* Negotiate the protocol (requesting `msize`, capped by the transport) and
 * attach the export root. On success the session owns `tp`. */
int fs9p_session_start(fs9p_session_t *s, fs9p_transport_t tp, uint32_t msize, int debug);
/* Wait for the requests in flight (at most the request timeout for ones
 * the device doesn't finish), clunk the root fid and close the transport.
 * The slots are kept: any later call blocks until the process exits. */
void fs9p_session_stop(fs9p_session_t *s);

/* Release a fid on the server and return it to the pool. The fid is
 * released locally even if the server reports an error. */
int fs9p_clunk(fs9p_session_t *s, uint32_t fid);

/* Walk `path` ("a/b/c", no leading slash, no "." or ".." components, as
 * QNX delivers path remainders; "" clones the root) from the root fid into
 * a new fid. On success *newfid is a fid the caller must clunk.
 * Errors: -ENOENT (missing component), -ENAMETOOLONG, -ENFILE (no fid), and
 * -ENOTSUP when the walk stopped at a symbolic link before the end of the
 * path: then *link_end (if not NULL) is the offset in `path` just past the
 * link's component, so the caller can resolve it. */
int fs9p_walk(fs9p_session_t *s, const char *path, uint32_t *newfid, size_t *link_end);

/* Called with the attributes of a directory a walk is about to search,
 * while the walk holds its slot. The directory's path is dirpath[0..len)
 * (not NUL-terminated; empty for the root). Return 0 to go on or -errno to
 * stop. */
typedef int (*fs9p_dir_check_t)(void *arg, const char *dirpath, size_t len, const p9_attr_t *dir);

/* fs9p_walk() one component at a time, calling `check` for every directory
 * it searches: the root and each component but the last (a Tgetattr per
 * directory). Same results and errors as fs9p_walk(), plus whatever
 * `check` returns. */
int fs9p_walk_check(fs9p_session_t *s, const char *path, uint32_t *newfid, size_t *link_end,
                    fs9p_dir_check_t check, void *arg);

int fs9p_getattr(fs9p_session_t *s, uint32_t fid, uint64_t mask, p9_attr_t *attr);
int fs9p_lopen(fs9p_session_t *s, uint32_t fid, uint32_t flags, uint32_t *iounit);
/* Tstatfs: statistics of the host filesystem holding fid's file. */
int fs9p_statfs(fs9p_session_t *s, uint32_t fid, p9_statfs_t *st);

/* Receives reply data while the call holds its slot: `data` points into the
 * slot's buffer and is only valid during the call. Return 0 to continue,
 * or -errno to abort the operation with that error. */
typedef int (*fs9p_sink_t)(void *arg, const uint8_t *data, uint32_t len);

/* Tread at `offset` for up to `count` bytes (capped to the negotiated msize).
 * Returns the byte count delivered to `sink` (0 at EOF) or -errno. */
int fs9p_read(fs9p_session_t *s, uint32_t fid, uint64_t offset, uint32_t count, fs9p_sink_t sink,
              void *arg);
/* Treaddir from `cookie`; `sink` gets the packed 9P entries (iterate with
 * p9_dirent_next). Returns the packed length (0 at end) or -errno. */
int fs9p_readdir(fs9p_session_t *s, uint32_t fid, uint64_t cookie, uint32_t count, fs9p_sink_t sink,
                 void *arg);
/* Treadlink; `sink` gets the target bytes (not NUL-terminated). */
int fs9p_readlink(fs9p_session_t *s, uint32_t fid, fs9p_sink_t sink, void *arg);

/* Largest Tread/Treaddir payload that fits the negotiated msize. */
uint32_t fs9p_io_max(const fs9p_session_t *s);

/* --- write path ----------------------------------------------------------------- */

/* Fills `len` bytes of request payload at `dst` (inside the slot's buffer)
 * while the call holds its slot. Returns 0 or -errno. */
typedef int (*fs9p_source_t)(void *arg, uint8_t *dst, uint32_t len);

/* Twrite of up to `count` bytes (capped to fs9p_write_max()) at `offset`;
 * `src` supplies the data straight into the request. Returns the byte count
 * the server wrote (may be short) or -errno. */
int fs9p_write(fs9p_session_t *s, uint32_t fid, uint64_t offset, uint32_t count, fs9p_source_t src,
               void *arg);
/* Largest Twrite payload that fits the negotiated msize. */
uint32_t fs9p_write_max(const fs9p_session_t *s);

/* Tlcreate: create and open `name` in directory `dirfid`. On success dirfid
 * refers to the new, open file (and no longer to the directory). */
int fs9p_lcreate(fs9p_session_t *s, uint32_t dirfid, const char *name, uint32_t flags,
                 uint32_t mode, uint32_t gid, uint32_t *iounit);
int fs9p_setattr(fs9p_session_t *s, uint32_t fid, const p9_setattr_t *attr);
int fs9p_fsync(fs9p_session_t *s, uint32_t fid, uint32_t datasync);

/* --- namespace ---------------------------------------------------------------- */
/* Names are single components inside the directory fid. */

int fs9p_mkdir(fs9p_session_t *s, uint32_t dirfid, const char *name, uint32_t mode, uint32_t gid);
int fs9p_symlink(fs9p_session_t *s, uint32_t dirfid, const char *name, const char *target,
                 uint32_t gid);
/* Hard link: `name` in dirfid becomes another name for fid. */
int fs9p_link(fs9p_session_t *s, uint32_t dirfid, uint32_t fid, const char *name);
int fs9p_renameat(fs9p_session_t *s, uint32_t olddirfid, const char *oldname, uint32_t newdirfid,
                  const char *newname);
/* flags: 0, or P9_DOTL_AT_REMOVEDIR to remove a directory. */
int fs9p_unlinkat(fs9p_session_t *s, uint32_t dirfid, const char *name, uint32_t flags);

#endif /* FS9P_CLIENT_H */
