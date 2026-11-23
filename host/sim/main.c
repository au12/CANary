/* Desktop simulator: the real CANary core, the real web UI and a modelled car.
 *
 *   canary-sim [--port 8088] [--speed 1] [--scenario healthy|worn|generic]
 *              [--state DIR] [--profiles DIR] [--web DIR] [--minutes 12]
 *              [--wifi home|always|never] [--fast-forward SECONDS]
 *              [--days N] [--tz MINUTES] [--wear PERCENT_PER_DAY]
 *              [--glovebox URL --vehicle N [--key KEY]]
 *
 * --fast-forward runs that much history flat out before serving, ending at the present moment.
 * --days N does the same for N days of ordinary use (weekday commutes, errands, a motorway trip
 * at the weekend), with the car's faults growing by --wear percent a day.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "host_platform.h"

#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int http_serve(cn_engine *e, const char *web_dir, int port);

static host_platform *g_h;
static cn_engine *g_e;
static const char *g_wifi = "home";

static void *vehicle_thread(void *arg)
{
    (void)arg;
    for (;;) cn_vehicle_step(g_e);
    return NULL;
}

static void *sync_thread(void *arg)
{
    (void)arg;
    for (;;) {
        cn_net_status st;
        memset(&st, 0, sizeof(st));
        pthread_mutex_lock(&g_h->mu);
        bool parked = !simcar_ignition(g_h->car);
        pthread_mutex_unlock(&g_h->mu);
        st.connected = !strcmp(g_wifi, "always") || (!strcmp(g_wifi, "home") && parked);
        cn_strlcpy(st.ssid, "HomeNet", sizeof(st.ssid));
        cn_strlcpy(st.ip, "10.0.10.50", sizeof(st.ip));
        st.rssi = -52;
        cn_net_update(g_e, &st);
        cn_sync_step(g_e);
        if (g_e->format_requested) {
            g_e->format_requested = false;
            char cmd[300];
            snprintf(cmd, sizeof(cmd), "rm -rf '%s/logs'", g_h->sd_dir);
            if (system(cmd) == 0) cn_trips_scan(g_e);
            cn_log(g_e, 'i', "SD card formatted");
        }
        if (g_e->reboot_requested) {
            g_e->reboot_requested = false;
            cn_log(g_e, 'i', "Restart requested (ignored in the simulator)");
        }
        usleep(300000);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN); /* a server that hangs up mid-upload must not end the simulator */
    const char *state = "/tmp/canary-sim", *profiles = "profiles", *web = "firmware/web", *scenario = "worn";
    int port = 8088, minutes = 12, tz_min = 0, vehicle = 0;
    const char *glovebox = NULL, *key = NULL;
    double speed = 1, ff = 0, days = 0, wear = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
        const char *k = argv[i], *v = argv[i + 1];
        if (!strcmp(k, "--port")) port = atoi(v);
        else if (!strcmp(k, "--speed")) speed = atof(v);
        else if (!strcmp(k, "--state")) state = v;
        else if (!strcmp(k, "--profiles")) profiles = v;
        else if (!strcmp(k, "--web")) web = v;
        else if (!strcmp(k, "--scenario")) scenario = v;
        else if (!strcmp(k, "--minutes")) minutes = atoi(v);
        else if (!strcmp(k, "--wifi")) g_wifi = v;
        else if (!strcmp(k, "--fast-forward")) ff = atof(v);
        else if (!strcmp(k, "--days")) days = atof(v);
        else if (!strcmp(k, "--tz")) tz_min = atoi(v);
        else if (!strcmp(k, "--wear")) wear = atof(v);
        else if (!strcmp(k, "--glovebox")) glovebox = v;
        else if (!strcmp(k, "--vehicle")) vehicle = atoi(v);
        else if (!strcmp(k, "--key")) key = v;
    }
    if (days > 0) ff = days * 86400;
    simcar_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (!strcmp(scenario, "worn")) {
        cfg.boost_leak = 0.21;
        cfg.chain_stretch = 4.3;
        cfg.idle_lean = 11;
        cfg.knock_cyl3 = 4.2;
        cfg.pending_dtcs = "P0299";
        cfg.abs_dtcs = "C10B8";
    } else if (!strcmp(scenario, "generic")) {
        cfg.ext_ids = true;
        cfg.no_uds = true;
        cfg.vin = "1HGCM82633A004352";
    }
    cfg.wear_per_day = wear / 100.0;
    cn_mkdirs(state);
    simcar *car = simcar_create(&cfg);
    int64_t start_wall = (int64_t)time(NULL) - (int64_t)ff;
    if (days > 0)
        simcar_script_daily(car, (double)(start_wall + tz_min * 60));
    else
        simcar_script_commute(car, minutes, true);
    g_h = host_platform_create(car, state, ff > 0);
    if (ff > 0) {
        g_h->wall_base = start_wall; /* history ends now */
        g_h->hold_clock = true;      /* a real server would otherwise pull the replayed past into the present */
    }
    g_e = cn_engine_create(&g_h->pf, host_profile_loader, (void *)profiles, "canary-51m0a7", "simulator");
    if (!g_e) return 1;
    if (tz_min) g_e->cfg.tz_offset_min = tz_min;
    if (glovebox) {
        cn_strlcpy(g_e->cfg.gb_url, glovebox, sizeof(g_e->cfg.gb_url));
        cn_strlcpy(g_e->cfg.gb_api_key, key ? key : "", sizeof(g_e->cfg.gb_api_key));
        g_e->cfg.gb_vehicle_id = vehicle;
    }
    if (ff > 0) {
        /* run a stretch of virtual time flat out (to pre-fill trips), then carry on in real time */
        int64_t last = -1000;
        while (g_h->vt_ms < (int64_t)(ff * 1000)) {
            cn_vehicle_step(g_e);
            if (g_h->vt_ms - last >= 1000) {
                last = g_h->vt_ms;
                cn_net_status st;
                memset(&st, 0, sizeof(st));
                st.connected = !strcmp(g_wifi, "always") || (!strcmp(g_wifi, "home") && !simcar_ignition(car));
                cn_strlcpy(st.ssid, "HomeNet", sizeof(st.ssid));
                cn_strlcpy(st.ip, "10.0.10.50", sizeof(st.ip));
                st.rssi = -52;
                cn_net_update(g_e, &st);
                cn_sync_step(g_e);
            }
        }
        /* switch to the wall clock, keeping virtual time continuous */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double back = g_h->vt_ms / speed;
        g_h->t0.tv_sec = now.tv_sec - (time_t)(back / 1000);
        g_h->t0.tv_nsec = now.tv_nsec;
        g_h->hold_clock = false;
        g_h->wall_base = (int64_t)time(NULL) - g_h->vt_ms / 1000;
        g_h->stepped = false;
    }
    g_h->speed = speed;
    pthread_t a, b;
    pthread_create(&a, NULL, vehicle_thread, NULL);
    pthread_create(&b, NULL, sync_thread, NULL);
    return http_serve(g_e, web, port);
}
