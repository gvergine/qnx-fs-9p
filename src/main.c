/*
 * fs-9p — 9P2000.L filesystem client for QNX over virtio-9p (virtio-mmio).
 *
 * Connects to the share named by mount_tag, then serves it at
 * mount_point as a resource manager.
 */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cache.h"
#include "client.h"
#include "resmgr.h"
#include "lib9p/p9.h"
#include "transport.h"

/* 512 KiB: 15-30% faster than 128 KiB for 1 MiB transfers in the
 * benchmark (docs/user/performance.md), for about 1 MiB of DMA memory. */
#define FS9P_DEFAULT_MSIZE   (512u * 1024u)
#define FS9P_DEFAULT_TIMEOUT 30000u /* ms; generous for TCG-emulated guests */
/* Metadata cache size (cache=MS); entries are a few hundred bytes. */
#define FS9P_CACHE_ENTRIES 4096u
/* Requests in flight at once; each slot costs 2 x msize of DMA memory. */
#define FS9P_DEFAULT_REQUESTS 4u

typedef struct {
    const char *mount_tag;
    const char *mount_point;
    uint32_t msize;
    uint32_t cache_ms; /* metadata cache TTL, 0 = off */
    bool ro;
    int debug;
    vio9p_options_t vio;
} fs9p_args_t;

static int parse_u32(const char *s, uint32_t *out)
{
    char *end;
    errno = 0;
    unsigned long v = strtoul(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0' || v > UINT32_MAX)
        return -1;
    *out = (uint32_t)v;
    return 0;
}

/* Parse one comma-separated -o list. Unknown options are an error so typos
 * don't silently change behaviour. */
static int parse_opts(char *list, fs9p_args_t *a)
{
    char *save = NULL;
    for (char *o = strtok_r(list, ",", &save); o != NULL; o = strtok_r(NULL, ",", &save)) {
        char *val = strchr(o, '=');
        if (val != NULL)
            *val++ = '\0';
        uint32_t n;
        if (strcmp(o, "msize") == 0 && val && parse_u32(val, &n) == 0 && n >= 4096) {
            a->msize = n;
        } else if (strcmp(o, "requests") == 0 && val && parse_u32(val, &n) == 0 && n >= 1 &&
                   n <= VIO9P_MAX_SLOTS) {
            a->vio.slots = n;
        } else if (strcmp(o, "timeout") == 0 && val && parse_u32(val, &n) == 0) {
            a->vio.timeout_ms = n;
        } else if (strcmp(o, "irq") == 0 && val && parse_u32(val, &n) == 0 && n <= INT32_MAX) {
            a->vio.irq = (int)n;
        } else if (strcmp(o, "debug") == 0 && !val) {
            a->debug = 1;
            a->vio.verbose = 1;
        } else if (strcmp(o, "cache") == 0 && val && parse_u32(val, &n) == 0 && n <= 3600000u) {
            a->cache_ms = n;
        } else if (strcmp(o, "ro") == 0 && !val) {
            a->ro = true;
        } else if (strcmp(o, "rw") == 0 && !val) {
            a->ro = false;
        } else if (strcmp(o, "poll") == 0 && !val) {
            a->vio.poll = 1;
        } else {
            fprintf(stderr, "fs-9p: bad option '%s%s%s'\n", o, val ? "=" : "", val ? val : "");
            return -1;
        }
    }
    return 0;
}

static int parse_args(int argc, char *argv[], fs9p_args_t *a)
{
    memset(a, 0, sizeof *a);
    a->msize = FS9P_DEFAULT_MSIZE;
    a->vio.timeout_ms = FS9P_DEFAULT_TIMEOUT;
    a->vio.slots = FS9P_DEFAULT_REQUESTS;
    a->vio.irq = -1;
    int c;
    while ((c = getopt(argc, argv, "o:")) != -1) {
        if (c != 'o' || parse_opts(optarg, a) != 0)
            return -1;
    }
    if (argc - optind != 2)
        return -1;
    a->mount_tag = argv[optind];
    /* Absolute, without trailing slashes: symlink redirects are built from it. */
    char *mp = argv[optind + 1];
    size_t n = strlen(mp);
    while (n > 1 && mp[n - 1] == '/')
        mp[--n] = '\0';
    if (mp[0] != '/' || n < 2) {
        fprintf(stderr, "fs-9p: mount_point must be an absolute path other than /\n");
        return -1;
    }
    a->mount_point = mp;
    a->vio.msize = a->msize;
    return 0;
}

int main(int argc, char *argv[])
{
    fs9p_args_t a;
    if (parse_args(argc, argv, &a) != 0) {
        fprintf(stderr, "usage: fs-9p [-o options] mount_tag mount_point (see 'use fs-9p')\n");
        return EXIT_FAILURE;
    }

    /* One thread takes the termination signals (resmgr_core.c) and unmounts
     * cleanly; block them here, before any thread exists, so every thread
     * inherits the mask. */
    sigset_t sigs;
    fs9p_term_signals(&sigs);
    pthread_sigmask(SIG_BLOCK, &sigs, NULL);

    fs9p_transport_t tp;
    int rc = vio9p_open(a.mount_tag, &a.vio, &tp);
    if (rc != 0) {
        fprintf(stderr, "fs-9p: %s: %s\n", a.mount_tag,
                rc == -ENODEV ? "no virtio-9p device with this mount tag" : strerror(-rc));
        return EXIT_FAILURE;
    }

    /* The session lives for the whole process. */
    static fs9p_session_t sess;
    rc = fs9p_session_start(&sess, tp, a.msize, a.debug);
    if (rc != 0) {
        fprintf(stderr, "fs-9p: %s: protocol setup failed: %s\n", a.mount_tag, strerror(-rc));
        tp.ops->close(tp.impl);
        return EXIT_FAILURE;
    }
    if (a.debug)
        fprintf(stderr, "fs-9p: %s: %s, msize %" PRIu32 "\n", a.mount_tag, P9_PROTO_VERSION,
                sess.msize);

    rc = fs9p_cache_init(a.cache_ms, FS9P_CACHE_ENTRIES);
    if (rc != 0) {
        fprintf(stderr, "fs-9p: cache: %s\n", strerror(-rc));
        fs9p_session_stop(&sess);
        return EXIT_FAILURE;
    }

    /* Serves requests until a termination signal unmounts (it exits). */
    rc = fs9p_resmgr_run(&sess, a.mount_tag, a.mount_point, a.ro, a.debug);
    fprintf(stderr, "fs-9p: %s: %s\n", a.mount_point, strerror(-rc));
    fs9p_session_stop(&sess);
    return EXIT_FAILURE;
}
