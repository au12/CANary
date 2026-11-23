/* Health monitors: declarative checks from the vehicle profile, evaluated over
 * a trip on the device and over a week in Glovebox (same definitions, same
 * 0.5 s evaluation tick). SPDX-License-Identifier: GPL-3.0-or-later */
#include "canary/core.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TICK_S 0.5

void cn_monitors_reset(cn_profile *p)
{
    for (int i = 0; i < p->nmonitors; i++) {
        cn_monitor *m = &p->monitors[i];
        m->n = m->n_true = m->edges = m->gate_run = 0;
        m->prev_true = m->reached = m->skip = false;
        m->side = 0;
        m->sum = m->first = m->last = 0;
        m->vmin = INFINITY;
        m->vmax = -INFINITY;
        m->time_to_s = 0;
        m->res_seen = 0;
        m->rng = cn_fnv1a(m->id, strlen(m->id), 0) | 1u;
        m->status = CN_SEV_NODATA;
        m->has_result = false;
        m->result = NAN;
    }
}

void cn_monitors_tick(cn_profile *p, const double *vars, double trip_time_s, double dt_s)
{
    (void)dt_s;
    for (int i = 0; i < p->nmonitors; i++) {
        cn_monitor *m = &p->monitors[i];
        if (!m->enabled) continue;
        double gate = m->when ? cn_expr_eval(m->when, vars) : 1.0;
        if (isnan(gate) || gate == 0) {
            m->prev_true = false;
            m->gate_run = 0;
            continue;
        }
        if (++m->gate_run * TICK_S <= m->settle_s) continue; /* still settling */
        double v = cn_expr_eval(m->value, vars);
        if (isnan(v)) continue;
        if (m->stat == CN_STAT_TIME_TO) {
            /* "How long until X" has no answer when X already holds as the trip begins, such as a warm
             * engine being restarted: the check then sits this trip out. */
            if (!m->n && v != 0) m->skip = true;
            if (m->skip) continue;
        }
        if (!m->n) m->first = v;
        m->last = v;
        m->n++;
        m->sum += v;
        if (v < m->vmin) m->vmin = v;
        if (v > m->vmax) m->vmax = v;
        bool t = v != 0;
        if (t) {
            m->n_true++;
            if (!m->prev_true) m->edges++;
            if (!m->reached) {
                m->reached = true;
                m->time_to_s = (float)trip_time_s;
            }
        }
        m->prev_true = t;
        /* reservoir sample for percentiles */
        m->res_seen++;
        if (m->res_seen <= CN_RESERVOIR) {
            m->res[m->res_seen - 1] = (float)v;
        } else {
            m->rng = m->rng * 1664525u + 1013904223u;
            uint32_t j = (m->rng >> 8) % m->res_seen;
            if (j < CN_RESERVOIR) m->res[j] = (float)v;
        }
    }
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

static double percentile(const cn_monitor *m, double q)
{
    int k = m->res_seen < CN_RESERVOIR ? (int)m->res_seen : CN_RESERVOIR;
    if (k <= 0) return NAN;
    float tmp[CN_RESERVOIR];
    memcpy(tmp, m->res, sizeof(float) * (size_t)k);
    qsort(tmp, (size_t)k, sizeof(float), cmp_float);
    double pos = q * (k - 1);
    int lo = (int)floor(pos);
    int hi = lo + 1 < k ? lo + 1 : lo;
    return tmp[lo] + (pos - lo) * (tmp[hi] - tmp[lo]);
}

double cn_monitor_stat(const cn_monitor *m, double trip_time_s, bool *ok)
{
    *ok = m->n > 0 && m->n * TICK_S >= m->min_time_s;
    if (!m->n) return NAN;
    switch (m->stat) {
    case CN_STAT_MEAN: return m->sum / m->n;
    case CN_STAT_MIN: return m->vmin;
    case CN_STAT_MAX: return m->vmax;
    case CN_STAT_P50: return percentile(m, 0.50);
    case CN_STAT_P90: return percentile(m, 0.90);
    case CN_STAT_P95: return percentile(m, 0.95);
    case CN_STAT_P99: return percentile(m, 0.99);
    case CN_STAT_LAST: return m->last;
    case CN_STAT_FIRST: return m->first;
    case CN_STAT_DELTA: return m->last - m->first;
    case CN_STAT_RANGE: return m->vmax - m->vmin;
    case CN_STAT_TIME: return m->n_true * TICK_S;
    case CN_STAT_FRACTION: return 100.0 * m->n_true / m->n;
    /* Never reached: the elapsed time is a lower bound, which is what the thresholds test. */
    case CN_STAT_TIME_TO: return m->reached ? m->time_to_s : trip_time_s;
    case CN_STAT_COUNT: return m->edges;
    default: return NAN;
    }
}

static bool level_hit(const cn_level *l, double v)
{
    switch (l->op) {
    case CN_OP_GT: return v > l->thr;
    case CN_OP_GE: return v >= l->thr;
    case CN_OP_LT: return v < l->thr;
    default: return v <= l->thr;
    }
}

void cn_monitors_evaluate(cn_profile *p, double trip_time_s)
{
    for (int i = 0; i < p->nmonitors; i++) {
        cn_monitor *m = &p->monitors[i];
        if (!m->enabled) continue;
        bool ok;
        double v = cn_monitor_stat(m, trip_time_s, &ok);
        m->result = v;
        m->has_result = ok && !isnan(v);
        if (!m->has_result) {
            m->status = CN_SEV_NODATA;
            continue;
        }
        uint8_t sev = CN_SEV_OK, side = 0;
        for (int l = 0; l < m->nlevels; l++)
            if (level_hit(&m->levels[l], v) && m->levels[l].sev > sev) {
                sev = m->levels[l].sev;
                side = m->two_sided && m->levels[l].op >= CN_OP_LT;
            }
        m->status = sev;
        m->side = side;
    }
}

static const cn_mon_text *says(const cn_monitor *m) { return &m->says[m->status != CN_SEV_NODATA && m->status > CN_SEV_OK ? m->side : 0]; }

const char *cn_monitor_headline(const cn_monitor *m)
{
    if (m->status == CN_SEV_NODATA || m->status == CN_SEV_OK) return m->title;
    const char *a = says(m)->alert ? says(m)->alert : m->says[0].alert;
    return a ? a : m->title;
}

const char *cn_monitor_template(const cn_monitor *m)
{
    if (m->status == CN_SEV_NODATA) return NULL;
    const char *sum = says(m)->summary ? says(m)->summary : m->says[0].summary;
    if (m->status > CN_SEV_OK) return sum ? sum : "Outside the expected range ({value}).";
    return m->ok_text ? m->ok_text : "Within the expected range ({value}).";
}

const char *cn_monitor_detail(const cn_monitor *m) { return says(m)->detail ? says(m)->detail : m->says[0].detail; }

int cn_monitor_causes(const cn_monitor *m, char *const **list)
{
    const cn_mon_text *t = says(m)->ncauses ? says(m) : &m->says[0];
    *list = t->causes;
    return t->ncauses;
}

void cn_monitor_text(const cn_monitor *m, char *out, size_t n)
{
    const char *tpl = NULL;
    if (m->status == CN_SEV_NODATA) {
        if (m->enabled && m->unavailable)
            snprintf(out, n, "This car does not report: %s.", m->missing);
        else
            cn_strlcpy(out, m->enabled ? "Not enough data yet." : "Not available on this vehicle.", n);
        return;
    }
    tpl = cn_monitor_template(m);
    /* "{value}" carries its unit: "25.0 kPa", "4.6°", "18%". Web UI and Glovebox re-render it in the user's units. */
    char num[48];
    const char *unit = m->unit;
    if (!strncmp(unit, "\xCE\x94", 2)) unit += 2; /* Δ marks a difference; it is not shown */
    bool tight = !unit[0] || !strcmp(unit, "%") || !strcmp(unit, "\xC2\xB0");
    snprintf(num, sizeof(num), "%.*f%s%s", m->dec, m->result, tight ? "" : " ", unit);
    size_t o = 0;
    for (const char *s = tpl; *s && o + 1 < n;) {
        if (!strncmp(s, "{value}", 7)) {
            for (const char *q = num; *q && o + 1 < n; q++) out[o++] = *q;
            s += 7;
        } else {
            out[o++] = *s++;
        }
    }
    out[o] = 0;
}
