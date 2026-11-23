/* POSIX implementation of cn_platform for the simulator and tests.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "canary/core.h"
#include "simcar.h"

#include <pthread.h>
#include <time.h>

typedef int (*host_http_hook)(void *ctx, const cn_http_req *req, cn_http_resp *resp);

typedef struct host_platform {
    cn_platform pf;
    simcar *car;
    bool stepped;        /* virtual clock: time only passes when the core waits */
    double speed;        /* real-time factor when not stepped */
    int64_t vt_ms;
    struct timespec t0;
    int64_t wall_base;   /* unix seconds at start; 0 = clock not set */
    char state_dir[200];
    char sd_dir[220];
    bool has_storage;
    uint64_t sd_total, sd_free;
    pthread_mutex_t mu;
    host_http_hook http_hook;
    void *http_ctx;
    uint32_t rng;
    bool no_vbat;
    unsigned tx_frames;
} host_platform;

host_platform *host_platform_create(simcar *car, const char *state_dir, bool stepped);
void host_platform_free(host_platform *h);
/* Profiles from a directory of <id>.json files. ctx = directory path. */
char *host_profile_loader(void *ctx, const char *id);
/* Plain HTTP/1.1 client over a socket (no TLS). */
int host_http_request(void *ctx, const cn_http_req *req, cn_http_resp *resp);
