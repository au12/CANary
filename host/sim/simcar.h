/* A modelled turbo petrol car on a fake CAN bus, for the desktop simulator and
 * the unit tests. SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "canary/core.h"

typedef struct {
    /* fault injection; all zero = a healthy car */
    double boost_leak;      /* fraction of requested boost that never arrives (0.18 = worn diverter valve) */
    double chain_stretch;   /* degrees of intake cam adaptation */
    double idle_lean;       /* extra fuel trim at idle, percent (vacuum leak) */
    double knock_cyl3;      /* extra knock retard on cylinder 3, degrees */
    double misfire_rate;    /* misfires per minute on cylinder 2 */
    bool thermostat_open;   /* coolant never gets fully warm */
    double charge_voltage;  /* 0 = healthy 14.2 V */
    const char *stored_dtcs;  /* e.g. "P0299 P0171" */
    const char *pending_dtcs;
    const char *abs_dtcs;     /* codes held by the brake module (UDS only) */
    bool mil;
    double ambient_c;       /* default 12 */
    double odometer_km;     /* default 165766 */
    const char *vin;        /* default 3VV2B7AX5JM012345 */
    bool ext_ids;           /* answer on 29-bit identifiers instead (generic car) */
    bool no_uds;            /* behave like a car with standard OBD-II only */
} simcar_cfg;

typedef struct simcar simcar;

simcar *simcar_create(const simcar_cfg *cfg);
void simcar_free(simcar *c);
/* Script: a list of segments. Each holds a target speed (km/h) for a duration; ignition and engine follow. */
typedef struct {
    double seconds;
    double speed; /* target km/h; <0 = ignition off */
    double pedal; /* 0 = automatic; otherwise forces the accelerator (WOT pulls) */
    bool engine_off_ign_on;
} simcar_seg;
void simcar_script(simcar *c, const simcar_seg *segs, int n, bool loop);
/* Built-in scripts */
void simcar_script_commute(simcar *c, int minutes, bool loop);

/* advance the model to absolute time t (seconds) */
void simcar_advance(simcar *c, double t);
/* tester -> car */
void simcar_rx(simcar *c, const cn_can_frame *f);
/* car -> tester: pops one frame, returns false when none is waiting */
bool simcar_tx(simcar *c, cn_can_frame *f);
double simcar_voltage(simcar *c);
double simcar_odometer(simcar *c);
bool simcar_ignition(simcar *c);
unsigned simcar_frames_seen(simcar *c);
