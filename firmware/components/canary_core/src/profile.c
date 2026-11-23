/* Vehicle profiles: JSON documents that describe ECUs, signals and health
 * monitors. Profiles inherit with "extends"; a child can add signals, put its
 * own data sources in front of the inherited ones, and adjust monitors.
 * See docs/profiles.md. SPDX-License-Identifier: GPL-3.0-or-later */
#include "canary/core.h"

#include "cJSON.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

const int cn_rate_period_ms[CN_RATE__N] = {0, 1000, 10000, 60000, -1};
/* A value older than this is "not available" to monitors and calculated signals. */
const int cn_rate_stale_ms[CN_RATE__N] = {5000, 5000, 30000, 180000, 0};

static const char *const k_rates[] = {"fast", "medium", "slow", "rare", "once"};
static const char *const k_sevs[] = {"ok", "advisory", "warning", "critical"};
static const char *const k_stats[] = {"mean", "min",   "max",   "p50",  "p90",      "p95",     "p99",  "last",
                                      "first", "delta", "range", "time", "fraction", "time_to", "count"};

const char *cn_rate_name(int rate) { return rate >= 0 && rate < CN_RATE__N ? k_rates[rate] : "?"; }
const char *cn_sev_name(int sev) { return sev >= 0 && sev <= CN_SEV_CRITICAL ? k_sevs[sev] : "nodata"; }
const char *cn_stat_name(int stat) { return stat >= 0 && stat < CN_STAT__N ? k_stats[stat] : "?"; }

static int lookup(const char *const *names, int n, const char *s, int dflt)
{
    if (!s) return dflt;
    for (int i = 0; i < n; i++)
        if (!strcmp(names[i], s)) return i;
    return dflt;
}

static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static double jnum(const cJSON *o, const char *k, double dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : dflt;
}

static bool jbool(const cJSON *o, const char *k, bool dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : dflt;
}

static cJSON *find_by_id(cJSON *arr, const char *id)
{
    cJSON *it;
    cJSON_ArrayForEach(it, arr)
    {
        const char *iid = jstr(it, "id");
        if (iid && !strcmp(iid, id)) return it;
    }
    return NULL;
}

static cJSON *ensure(cJSON *obj, const char *key, bool array)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!v) {
        v = array ? cJSON_CreateArray() : cJSON_CreateObject();
        cJSON_AddItemToObject(obj, key, v);
    }
    return v;
}

static void set_key(cJSON *obj, const char *key, const cJSON *value)
{
    cJSON *dup = cJSON_Duplicate(value, true);
    if (cJSON_GetObjectItemCaseSensitive(obj, key))
        cJSON_ReplaceItemInObjectCaseSensitive(obj, key, dup);
    else
        cJSON_AddItemToObject(obj, key, dup);
}

/* Layer one document on top of the accumulated result. */
static void apply_doc(cJSON *acc, const cJSON *doc)
{
    const cJSON *it, *f;
    const cJSON *can = cJSON_GetObjectItemCaseSensitive(doc, "can");
    if (cJSON_IsObject(can)) {
        cJSON *dst = ensure(acc, "can", false);
        cJSON_ArrayForEach(f, can) set_key(dst, f->string, f);
    }
    const cJSON *ecus = cJSON_GetObjectItemCaseSensitive(doc, "ecus");
    if (cJSON_IsObject(ecus)) {
        cJSON *dst = ensure(acc, "ecus", false);
        cJSON_ArrayForEach(it, ecus)
        {
            cJSON *cur = cJSON_GetObjectItemCaseSensitive(dst, it->string);
            if (!cur) {
                cJSON_AddItemToObject(dst, it->string, cJSON_Duplicate(it, true));
            } else {
                cJSON_ArrayForEach(f, it) set_key(cur, f->string, f);
            }
        }
    }
    const cJSON *sigs = cJSON_GetObjectItemCaseSensitive(doc, "signals");
    if (cJSON_IsArray(sigs)) {
        cJSON *dst = ensure(acc, "signals", true);
        cJSON_ArrayForEach(it, sigs)
        {
            const char *id = jstr(it, "id");
            if (!id) continue;
            cJSON *cur = find_by_id(dst, id);
            if (!cur) {
                cJSON_AddItemToArray(dst, cJSON_Duplicate(it, true));
                continue;
            }
            bool replace = jbool(it, "replace_src", false);
            cJSON_ArrayForEach(f, it)
            {
                if (!strcmp(f->string, "replace_src")) continue;
                if (!strcmp(f->string, "src") && !replace && cJSON_IsArray(f)) {
                    /* child sources go in front of the inherited ones */
                    cJSON *old = cJSON_DetachItemFromObjectCaseSensitive(cur, "src");
                    cJSON *merged = cJSON_Duplicate(f, true);
                    const cJSON *o;
                    cJSON_ArrayForEach(o, old) cJSON_AddItemToArray(merged, cJSON_Duplicate(o, true));
                    cJSON_Delete(old);
                    cJSON_AddItemToObject(cur, "src", merged);
                } else {
                    set_key(cur, f->string, f);
                }
            }
        }
    }
    const cJSON *mons = cJSON_GetObjectItemCaseSensitive(doc, "monitors");
    if (cJSON_IsArray(mons)) {
        cJSON *dst = ensure(acc, "monitors", true);
        cJSON_ArrayForEach(it, mons)
        {
            const char *id = jstr(it, "id");
            if (!id) continue;
            cJSON *cur = find_by_id(dst, id);
            if (!cur)
                cJSON_AddItemToArray(dst, cJSON_Duplicate(it, true));
            else
                cJSON_ArrayForEach(f, it) set_key(cur, f->string, f);
        }
    }
    static const char *const removals[][2] = {{"remove_signals", "signals"}, {"remove_monitors", "monitors"}};
    for (int r = 0; r < 2; r++) {
        const cJSON *rm = cJSON_GetObjectItemCaseSensitive(doc, removals[r][0]);
        cJSON *dst = cJSON_GetObjectItemCaseSensitive(acc, removals[r][1]);
        if (!cJSON_IsArray(rm) || !dst) continue;
        cJSON_ArrayForEach(it, rm)
        {
            if (!cJSON_IsString(it)) continue;
            int idx = 0;
            cJSON *e;
            cJSON_ArrayForEach(e, dst)
            {
                const char *iid = jstr(e, "id");
                if (iid && !strcmp(iid, it->valuestring)) {
                    cJSON_DeleteItemFromArray(dst, idx);
                    break;
                }
                idx++;
            }
        }
    }
}

static bool resolve_into(cJSON *acc, const char *id, cn_profile_loader load, void *ctx, int depth, char *err,
                         size_t err_len, cJSON **top_out)
{
    if (depth > 6) {
        snprintf(err, err_len, "profile inheritance too deep at '%s'", id);
        return false;
    }
    char *text = load(ctx, id);
    if (!text) {
        snprintf(err, err_len, "profile '%s' not found", id);
        return false;
    }
    cJSON *doc = cJSON_Parse(text);
    free(text);
    if (!doc) {
        snprintf(err, err_len, "profile '%s' is not valid JSON", id);
        return false;
    }
    const cJSON *ext = cJSON_GetObjectItemCaseSensitive(doc, "extends");
    const cJSON *it;
    bool ok = true;
    cJSON_ArrayForEach(it, ext)
    {
        if (cJSON_IsString(it) && !resolve_into(acc, it->valuestring, load, ctx, depth + 1, err, err_len, NULL)) {
            ok = false;
            break;
        }
    }
    if (ok) apply_doc(acc, doc);
    if (ok && top_out)
        *top_out = doc;
    else
        cJSON_Delete(doc);
    return ok;
}

typedef struct {
    cn_profile *p;
} resolver_ctx;

static int resolve_signal(void *ctx, const char *name) { return cn_profile_signal_index(((resolver_ctx *)ctx)->p, name); }

int cn_profile_signal_index(const cn_profile *p, const char *id)
{
    for (int i = 0; i < p->nsignals; i++)
        if (!strcmp(p->signals[i].id, id)) return i;
    return -1;
}

static int ecu_index(const cn_profile *p, const char *id)
{
    for (int i = 0; i < p->necus; i++)
        if (!strcmp(p->ecus[i].id, id)) return i;
    return -1;
}

static int request_index(cn_profile *p, uint8_t ecu, uint8_t mode, uint16_t code, uint8_t rate)
{
    for (int i = 0; i < p->nrequests; i++) {
        cn_request *r = &p->requests[i];
        if (r->ecu == ecu && r->mode == mode && r->code == code) {
            if (rate < r->rate) r->rate = rate;
            return i;
        }
    }
    if (p->nrequests >= CN_MAX_REQUESTS) return -1;
    cn_request *r = &p->requests[p->nrequests];
    memset(r, 0, sizeof(*r));
    r->ecu = ecu;
    r->mode = mode;
    r->code = code;
    r->rate = rate;
    return p->nrequests++;
}

static bool parse_source(cn_profile *p, cn_signal *s, const cJSON *j, cn_source *out, char *err, size_t err_len)
{
    memset(out, 0, sizeof(*out));
    out->mul = jnum(j, "mul", 1.0);
    out->div = jnum(j, "div", 1.0);
    out->add = jnum(j, "add", 0.0);
    out->bit = (uint16_t)jnum(j, "bit", 0);
    out->len = (uint8_t)jnum(j, "len", 8);
    out->sign = jbool(j, "sign", false);
    out->lsb = jbool(j, "lsb", false);
    out->req = -1;
    if (out->div == 0) out->div = 1;
    const char *v;
    bool ok = false;
    if ((v = jstr(j, "obd"))) {
        out->kind = CN_SRC_OBD;
        out->mode = 0x01;
        out->code = (uint16_t)cn_parse_u32(v, 16, &ok);
        const char *ecu = jstr(j, "ecu");
        int ei = ecu_index(p, ecu ? ecu : "obd");
        if (!ok || ei < 0) goto bad;
        out->ecu = (uint8_t)ei;
    } else if ((v = jstr(j, "uds"))) {
        out->kind = CN_SRC_UDS;
        out->mode = 0x22;
        out->code = (uint16_t)cn_parse_u32(v, 16, &ok);
        const char *ecu = jstr(j, "ecu");
        int ei = ecu ? ecu_index(p, ecu) : -1;
        if (!ok || ei < 0) {
            snprintf(err, err_len, "signal '%s': uds source needs a known \"ecu\"", s->id);
            return false;
        }
        out->ecu = (uint8_t)ei;
    } else if ((v = jstr(j, "device"))) {
        out->kind = CN_SRC_DEVICE;
        if (!strcmp(v, "vbat"))
            out->code = CN_DEV_VBAT;
        else if (!strcmp(v, "trip_time"))
            out->code = CN_DEV_TRIP_TIME;
        else
            goto bad;
    } else if (jstr(j, "calc")) {
        out->kind = CN_SRC_CALC; /* compiled once every signal exists */
    } else {
        goto bad;
    }
    if (out->kind == CN_SRC_OBD || out->kind == CN_SRC_UDS) {
        out->req = (int16_t)request_index(p, out->ecu, out->mode, out->code, s->rate);
        if (out->req < 0) {
            snprintf(err, err_len, "too many distinct requests (max %d)", CN_MAX_REQUESTS);
            return false;
        }
    }
    return true;
bad:
    snprintf(err, err_len, "signal '%s': bad source", s->id);
    return false;
}

static char *dupstr(const char *s) { return s ? strdup(s) : NULL; }

static bool build(cn_profile *p, const cJSON *doc, char *err, size_t err_len)
{
    const cJSON *it;
    const cJSON *can = cJSON_GetObjectItemCaseSensitive(doc, "can");
    p->can_bitrate = (int)jnum(can, "bitrate", 0);
    p->can_id_bits = (int)jnum(can, "id_bits", 0);

    /* ecu 0 is always the OBD-II functional address; its ids are filled in at protocol detection */
    cn_strlcpy(p->ecus[0].id, "obd", sizeof(p->ecus[0].id));
    cn_strlcpy(p->ecus[0].name, "OBD-II", sizeof(p->ecus[0].name));
    p->necus = 1;
    const cJSON *ecus = cJSON_GetObjectItemCaseSensitive(doc, "ecus");
    cJSON_ArrayForEach(it, ecus)
    {
        bool ok1 = true, ok2 = true;
        const char *tx = jstr(it, "tx"), *rx = jstr(it, "rx");
        cn_ecu *e;
        if (!strcmp(it->string, "obd")) {
            e = &p->ecus[0];
        } else {
            if (p->necus >= CN_MAX_ECUS) {
                snprintf(err, err_len, "too many ecus (max %d)", CN_MAX_ECUS);
                return false;
            }
            e = &p->ecus[p->necus++];
            memset(e, 0, sizeof(*e));
            cn_strlcpy(e->id, it->string, sizeof(e->id));
        }
        if (tx) e->tx = cn_parse_u32(tx, 16, &ok1);
        if (rx) e->rx = cn_parse_u32(rx, 16, &ok2);
        if (!ok1 || !ok2) {
            snprintf(err, err_len, "ecu '%s': bad tx/rx", it->string);
            return false;
        }
        e->ext = e->tx > 0x7FF || e->rx > 0x7FF;
        e->dtc = jbool(it, "dtc", false);
        const char *name = jstr(it, "name");
        if (name) cn_strlcpy(e->name, name, sizeof(e->name));
        else if (!e->name[0]) cn_strlcpy(e->name, it->string, sizeof(e->name));
    }

    const cJSON *sigs = cJSON_GetObjectItemCaseSensitive(doc, "signals");
    int nsig = cJSON_GetArraySize(sigs);
    if (nsig > CN_MAX_SIGNALS) {
        snprintf(err, err_len, "too many signals (%d, max %d)", nsig, CN_MAX_SIGNALS);
        return false;
    }
    p->signals = calloc((size_t)(nsig ? nsig : 1), sizeof(cn_signal));
    p->requests = calloc(CN_MAX_REQUESTS, sizeof(cn_request));
    if (!p->signals || !p->requests) {
        snprintf(err, err_len, "out of memory");
        return false;
    }
    cJSON_ArrayForEach(it, sigs)
    {
        const char *id = jstr(it, "id");
        if (!id || !*id) {
            snprintf(err, err_len, "a signal has no id");
            return false;
        }
        cn_signal *s = &p->signals[p->nsignals];
        cn_strlcpy(s->id, id, sizeof(s->id));
        cn_strlcpy(s->name, jstr(it, "name") ? jstr(it, "name") : id, sizeof(s->name));
        cn_strlcpy(s->unit, jstr(it, "unit"), sizeof(s->unit));
        cn_strlcpy(s->group, jstr(it, "group") ? jstr(it, "group") : "other", sizeof(s->group));
        s->rate = (uint8_t)lookup(k_rates, CN_RATE__N, jstr(it, "rate"), CN_RATE_MED);
        s->dec = (uint8_t)jnum(it, "dec", 1);
        s->log = jbool(it, "log", true);
        s->active = -1;
        const cJSON *lo = cJSON_GetObjectItemCaseSensitive(it, "min"), *hi = cJSON_GetObjectItemCaseSensitive(it, "max");
        if (cJSON_IsNumber(lo) || cJSON_IsNumber(hi)) {
            s->has_range = true;
            s->lo = cJSON_IsNumber(lo) ? lo->valuedouble : -INFINITY;
            s->hi = cJSON_IsNumber(hi) ? hi->valuedouble : INFINITY;
        }
        p->nsignals++;
    }
    /* sources need the complete signal list (calc) and ecu list */
    int si = 0;
    cJSON_ArrayForEach(it, sigs)
    {
        cn_signal *s = &p->signals[si++];
        const cJSON *src = cJSON_GetObjectItemCaseSensitive(it, "src"), *j;
        cJSON_ArrayForEach(j, src)
        {
            if (s->nsrc >= CN_MAX_SRC) break;
            cn_source *o = &s->src[s->nsrc];
            if (!parse_source(p, s, j, o, err, err_len)) return false;
            if (o->kind == CN_SRC_CALC) {
                resolver_ctx rc = {p};
                char e2[96];
                o->calc = cn_expr_compile(jstr(j, "calc"), resolve_signal, &rc, e2, sizeof(e2));
                if (!o->calc) {
                    snprintf(err, err_len, "signal '%s': %s", s->id, e2);
                    return false;
                }
            }
            s->nsrc++;
        }
        if (!s->nsrc) {
            snprintf(err, err_len, "signal '%s' has no sources", s->id);
            return false;
        }
    }

    const cJSON *mons = cJSON_GetObjectItemCaseSensitive(doc, "monitors");
    int nmon = cJSON_GetArraySize(mons);
    if (nmon > CN_MAX_MONITORS) nmon = CN_MAX_MONITORS;
    p->monitors = calloc((size_t)(nmon ? nmon : 1), sizeof(cn_monitor));
    if (!p->monitors) {
        snprintf(err, err_len, "out of memory");
        return false;
    }
    cJSON_ArrayForEach(it, mons)
    {
        if (p->nmonitors >= nmon) break;
        const char *id = jstr(it, "id");
        if (!id) continue;
        cn_monitor *m = &p->monitors[p->nmonitors++];
        cn_strlcpy(m->id, id, sizeof(m->id));
        cn_strlcpy(m->title, jstr(it, "title") ? jstr(it, "title") : id, sizeof(m->title));
        cn_strlcpy(m->category, jstr(it, "category") ? jstr(it, "category") : "General", sizeof(m->category));
        cn_strlcpy(m->unit, jstr(it, "unit"), sizeof(m->unit));
        m->stat = (uint8_t)lookup(k_stats, CN_STAT__N, jstr(it, "stat"), CN_STAT_MEAN);
        m->dec = (uint8_t)jnum(it, "dec", 1);
        m->min_time_s = (float)jnum(it, "min_time_s", 10);
        m->settle_s = (float)jnum(it, "settle_s", 0);
        m->summary = dupstr(jstr(it, "summary"));
        m->ok_text = dupstr(jstr(it, "ok"));
        m->detail = dupstr(jstr(it, "detail"));
        m->rng = cn_fnv1a(id, strlen(id), 0) | 1u;
        const cJSON *c;
        cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(it, "causes"))
        {
            if (m->ncauses < CN_MAX_CAUSES && cJSON_IsString(c)) m->causes[m->ncauses++] = strdup(c->valuestring);
        }
        cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(it, "levels"))
        {
            if (m->nlevels >= CN_MAX_LEVELS) break;
            cn_level *l = &m->levels[m->nlevels];
            int sev = lookup(k_sevs, 4, jstr(c, "severity"), -1);
            static const char *const ops[] = {"gt", "ge", "lt", "le"};
            int op = -1;
            for (int o = 0; o < 4; o++)
                if (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(c, ops[o]))) {
                    op = o;
                    l->thr = jnum(c, ops[o], 0);
                }
            if (sev < 1 || op < 0) continue;
            l->sev = (uint8_t)sev;
            l->op = (uint8_t)op;
            m->nlevels++;
        }
        resolver_ctx rc = {p};
        char e2[40] = "";
        const char *when = jstr(it, "when"), *value = jstr(it, "value");
        m->enabled = true;
        if (when && *when) {
            m->when = cn_expr_compile(when, resolve_signal, &rc, e2, sizeof(e2));
            if (!m->when) m->enabled = false;
        }
        if (m->enabled && value && *value) {
            m->value = cn_expr_compile(value, resolve_signal, &rc, e2, sizeof(e2));
            if (!m->value) m->enabled = false;
        } else if (m->enabled) {
            m->enabled = false;
            snprintf(e2, sizeof(e2), "no value expression");
        }
        if (!m->enabled) cn_strlcpy(m->why_disabled, e2, sizeof(m->why_disabled));
        m->status = CN_SEV_NODATA;
    }
    return true;
}

cn_profile *cn_profile_load(const char *id, cn_profile_loader load, void *ctx, char *err, size_t err_len)
{
    char e2[128] = "";
    if (!err) {
        err = e2;
        err_len = sizeof(e2);
    }
    cJSON *acc = cJSON_CreateObject();
    cJSON *top = NULL;
    if (!resolve_into(acc, id, load, ctx, 0, err, err_len, &top)) {
        cJSON_Delete(acc);
        return NULL;
    }
    cn_profile *p = calloc(1, sizeof(*p));
    if (!p) {
        cJSON_Delete(acc);
        cJSON_Delete(top);
        return NULL;
    }
    cn_strlcpy(p->id, id, sizeof(p->id));
    cn_strlcpy(p->name, jstr(top, "name") ? jstr(top, "name") : id, sizeof(p->name));
    cJSON_AddStringToObject(acc, "id", p->id);
    cJSON_AddStringToObject(acc, "name", p->name);
    cJSON_Delete(top);
    bool ok = build(p, acc, err, err_len);
    if (ok) {
        p->resolved_json = cJSON_PrintUnformatted(acc);
        if (p->resolved_json)
            snprintf(p->hash, sizeof(p->hash), "%08x",
                     (unsigned)cn_fnv1a(p->resolved_json, strlen(p->resolved_json), 0));
    }
    cJSON_Delete(acc);
    if (!ok || !p->resolved_json) {
        cn_profile_free(p);
        return NULL;
    }
    return p;
}

void cn_profile_free(cn_profile *p)
{
    if (!p) return;
    for (int i = 0; i < p->nsignals; i++)
        for (int k = 0; k < p->signals[i].nsrc; k++) cn_expr_free(p->signals[i].src[k].calc);
    for (int i = 0; i < p->nmonitors; i++) {
        cn_monitor *m = &p->monitors[i];
        cn_expr_free(m->when);
        cn_expr_free(m->value);
        free(m->summary);
        free(m->ok_text);
        free(m->detail);
        for (int c = 0; c < m->ncauses; c++) free(m->causes[c]);
    }
    free(p->signals);
    free(p->requests);
    free(p->monitors);
    free(p->resolved_json);
    free(p);
}

int cn_profile_match_vin(const char *profile_json, const char *vin)
{
    if (!vin || strlen(vin) < 11) return 0;
    cJSON *doc = cJSON_Parse(profile_json);
    if (!doc) return 0;
    int score = 0;
    const cJSON *match = cJSON_GetObjectItemCaseSensitive(doc, "match");
    const cJSON *pat;
    cJSON_ArrayForEach(pat, cJSON_GetObjectItemCaseSensitive(match, "vin"))
    {
        if (cJSON_IsString(pat) && cn_glob(pat->valuestring, vin)) {
            score = (int)jnum(match, "priority", 10);
            if (score < 1) score = 1;
            break;
        }
    }
    cJSON_Delete(doc);
    return score;
}
