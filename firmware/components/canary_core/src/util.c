/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "canary/core.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void cn_buf_init(cn_buf *b) { memset(b, 0, sizeof(*b)); }

void cn_buf_free(cn_buf *b)
{
    free(b->p);
    memset(b, 0, sizeof(*b));
}

static bool buf_reserve(cn_buf *b, size_t extra)
{
    if (b->oom) return false;
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return true;
    size_t cap = b->cap ? b->cap * 2 : 256;
    while (cap < need) cap *= 2;
    char *p = realloc(b->p, cap);
    if (!p) {
        b->oom = true;
        return false;
    }
    b->p = p;
    b->cap = cap;
    return true;
}

void cn_buf_put(cn_buf *b, const char *s, size_t n)
{
    if (!buf_reserve(b, n)) return;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

void cn_buf_puts(cn_buf *b, const char *s) { cn_buf_put(b, s, strlen(s)); }

void cn_buf_printf(cn_buf *b, const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n > 0 && buf_reserve(b, (size_t)n)) {
        vsnprintf(b->p + b->len, (size_t)n + 1, fmt, ap2);
        b->len += (size_t)n;
    }
    va_end(ap2);
}

char *cn_buf_take(cn_buf *b)
{
    if (!b->p && !b->oom) buf_reserve(b, 0);
    char *p = b->p;
    if (p) p[b->len] = 0;
    memset(b, 0, sizeof(*b));
    return p;
}

/* ── JSON writer ── */

void cn_jw_init(cn_jw *w, cn_buf *b)
{
    w->b = b;
    w->depth = 0;
    w->first = 1;
}

static void jw_sep(cn_jw *w)
{
    uint32_t bit = 1u << w->depth;
    if (w->first & bit)
        w->first &= ~bit;
    else
        cn_buf_put(w->b, ",", 1);
}

static void jw_open(cn_jw *w, char c)
{
    jw_sep(w);
    cn_buf_put(w->b, &c, 1);
    if (w->depth < 30) w->depth++;
    w->first |= 1u << w->depth;
}

static void jw_close(cn_jw *w, char c)
{
    if (w->depth > 0) w->depth--;
    cn_buf_put(w->b, &c, 1);
}

void cn_jw_obj(cn_jw *w) { jw_open(w, '{'); }
void cn_jw_end_obj(cn_jw *w) { jw_close(w, '}'); }
void cn_jw_arr(cn_jw *w) { jw_open(w, '['); }
void cn_jw_end_arr(cn_jw *w) { jw_close(w, ']'); }

static void jw_escaped(cn_buf *b, const char *s)
{
    cn_buf_put(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
        case '"': cn_buf_put(b, "\\\"", 2); break;
        case '\\': cn_buf_put(b, "\\\\", 2); break;
        case '\n': cn_buf_put(b, "\\n", 2); break;
        case '\r': cn_buf_put(b, "\\r", 2); break;
        case '\t': cn_buf_put(b, "\\t", 2); break;
        default:
            if (*p < 0x20)
                cn_buf_printf(b, "\\u%04x", *p);
            else
                cn_buf_put(b, (const char *)p, 1);
        }
    }
    cn_buf_put(b, "\"", 1);
}

void cn_jw_key(cn_jw *w, const char *k)
{
    jw_sep(w);
    jw_escaped(w->b, k);
    cn_buf_put(w->b, ":", 1);
    w->first |= 1u << w->depth; /* the value that follows needs no comma */
}

void cn_jw_str(cn_jw *w, const char *s)
{
    jw_sep(w);
    jw_escaped(w->b, s);
}

void cn_jw_num(cn_jw *w, double v, int decimals)
{
    jw_sep(w);
    if (isnan(v) || isinf(v)) {
        cn_buf_put(w->b, "null", 4);
        return;
    }
    if (decimals < 0) {
        cn_buf_printf(w->b, "%.7g", v);
        return;
    }
    char tmp[40];
    snprintf(tmp, sizeof(tmp), "%.*f", decimals > 9 ? 9 : decimals, v);
    if (strcmp(tmp, "-0") == 0 || strncmp(tmp, "-0.", 3) == 0) {
        bool zero = true;
        for (const char *p = tmp + 1; *p; p++)
            if (*p != '0' && *p != '.') zero = false;
        if (zero) memmove(tmp, tmp + 1, strlen(tmp));
    }
    cn_buf_puts(w->b, tmp);
}

void cn_jw_int(cn_jw *w, long long v)
{
    jw_sep(w);
    cn_buf_printf(w->b, "%lld", v);
}

void cn_jw_bool(cn_jw *w, bool v)
{
    jw_sep(w);
    cn_buf_puts(w->b, v ? "true" : "false");
}

void cn_jw_null(cn_jw *w)
{
    jw_sep(w);
    cn_buf_put(w->b, "null", 4);
}

void cn_jw_raw(cn_jw *w, const char *json)
{
    jw_sep(w);
    cn_buf_puts(w->b, json && *json ? json : "null");
}

void cn_jw_kstr(cn_jw *w, const char *k, const char *s)
{
    cn_jw_key(w, k);
    cn_jw_str(w, s);
}
void cn_jw_knum(cn_jw *w, const char *k, double v, int decimals)
{
    cn_jw_key(w, k);
    cn_jw_num(w, v, decimals);
}
void cn_jw_kint(cn_jw *w, const char *k, long long v)
{
    cn_jw_key(w, k);
    cn_jw_int(w, v);
}
void cn_jw_kbool(cn_jw *w, const char *k, bool v)
{
    cn_jw_key(w, k);
    cn_jw_bool(w, v);
}

/* ── misc ── */

void cn_strlcpy(char *dst, const char *src, size_t n)
{
    if (!n) return;
    if (!src) src = "";
    size_t i = 0;
    for (; i + 1 < n && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int cn_parse_hex(const char *s, uint8_t *out, int cap)
{
    int n = 0;
    while (*s) {
        if (*s == ' ' || *s == ':' || *s == '-') {
            s++;
            continue;
        }
        int hi = hex_nibble(s[0]);
        int lo = hi >= 0 ? hex_nibble(s[1]) : -1;
        if (hi < 0 || lo < 0) return -1;
        if (n >= cap) return -1;
        out[n++] = (uint8_t)((hi << 4) | lo);
        s += 2;
    }
    return n;
}

uint32_t cn_parse_u32(const char *s, int base, bool *ok)
{
    if (ok) *ok = false;
    if (!s || !*s) return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        base = 16;
    }
    char *end = NULL;
    errno = 0;
    unsigned long v = strtoul(s, &end, base);
    if (errno || end == s || *end) return 0;
    if (ok) *ok = true;
    return (uint32_t)v;
}

uint32_t cn_crc32(uint32_t crc, const void *data, size_t len)
{
    static uint32_t table[256];
    static bool init;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    const uint8_t *p = data;
    crc = ~crc;
    while (len--) crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

uint32_t cn_fnv1a(const void *data, size_t len, uint32_t seed)
{
    const uint8_t *p = data;
    uint32_t h = seed ? seed : 2166136261u;
    while (len--) {
        h ^= *p++;
        h *= 16777619u;
    }
    return h;
}

/* Days-from-civil, so this works without a C library timezone database. */
static void civil_from_unix(int64_t t, int *Y, int *M, int *D, int *h, int *m, int *s)
{
    int64_t days = t / 86400;
    int64_t rem = t % 86400;
    if (rem < 0) {
        rem += 86400;
        days--;
    }
    *h = (int)(rem / 3600);
    *m = (int)((rem % 3600) / 60);
    *s = (int)(rem % 60);
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    unsigned doe = (unsigned)(days - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *D = (int)(doy - (153 * mp + 2) / 5 + 1);
    *M = (int)(mp < 10 ? mp + 3 : mp - 9);
    *Y = (int)(y + (*M <= 2));
}

void cn_iso8601(int64_t unix_s, char out[24])
{
    int Y, M, D, h, m, s;
    civil_from_unix(unix_s, &Y, &M, &D, &h, &m, &s);
    snprintf(out, 24, "%04d-%02d-%02dT%02d:%02d:%02dZ", Y, M, D, h, m, s);
}

void cn_compact_time(int64_t unix_s, char out[20])
{
    int Y, M, D, h, m, s;
    civil_from_unix(unix_s, &Y, &M, &D, &h, &m, &s);
    snprintf(out, 20, "%04d%02d%02dT%02d%02d%02dZ", Y, M, D, h, m, s);
}

bool cn_glob(const char *p, const char *s)
{
    while (*p) {
        if (*p == '*') {
            while (*p == '*') p++;
            if (!*p) return true;
            for (; *s; s++)
                if (cn_glob(p, s)) return true;
            return false;
        }
        if (!*s) return false;
        if (*p != '?' && *p != *s) return false;
        p++;
        s++;
    }
    return *s == 0;
}

char *cn_read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    cn_buf b;
    cn_buf_init(&b);
    char tmp[1024];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) cn_buf_put(&b, tmp, n);
    fclose(f);
    if (b.oom) {
        cn_buf_free(&b);
        return NULL;
    }
    if (len) *len = b.len;
    return cn_buf_take(&b);
}

int cn_write_file(const char *path, const char *data, size_t len)
{
    char tmp[200];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    size_t w = fwrite(data, 1, len, f);
    int rc = fclose(f);
    if (w != len || rc != 0) {
        remove(tmp);
        return -1;
    }
    remove(path); /* FAT rename does not replace */
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return -1;
    }
    return 0;
}

int cn_mkdirs(const char *path)
{
    char tmp[200];
    cn_strlcpy(tmp, path, sizeof(tmp));
    size_t n = strlen(tmp);
    for (size_t i = 1; i <= n; i++) {
        if (tmp[i] == '/' || tmp[i] == 0) {
            char c = tmp[i];
            tmp[i] = 0;
            struct stat st;
            if (stat(tmp, &st) != 0) {
                if (mkdir(tmp, 0775) != 0 && errno != EEXIST) return -1;
            }
            tmp[i] = c;
        }
    }
    return 0;
}

bool cn_query_get(const char *query, const char *key, char *out, size_t n)
{
    if (!query || !n) return false;
    size_t kl = strlen(key);
    const char *p = query;
    while (*p) {
        const char *amp = strchr(p, '&');
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        if (seg > kl && strncmp(p, key, kl) == 0 && p[kl] == '=') {
            const char *v = p + kl + 1;
            size_t vl = seg - kl - 1, o = 0;
            for (size_t i = 0; i < vl && o + 1 < n; i++) {
                if (v[i] == '%' && i + 2 < vl + 0 && hex_nibble(v[i + 1]) >= 0 && hex_nibble(v[i + 2]) >= 0) {
                    out[o++] = (char)((hex_nibble(v[i + 1]) << 4) | hex_nibble(v[i + 2]));
                    i += 2;
                } else if (v[i] == '+') {
                    out[o++] = ' ';
                } else {
                    out[o++] = v[i];
                }
            }
            out[o] = 0;
            return true;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return false;
}
