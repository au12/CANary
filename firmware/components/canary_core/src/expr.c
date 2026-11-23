/* Tiny expression language used by vehicle profiles (calculated signals and
 * health monitors). The same grammar is implemented in C# in Glovebox; the
 * shared vectors in profiles/tests/expr_vectors.json keep the two in step.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 *   expr    := or
 *   or      := and ( "||" and )*
 *   and     := cmp ( "&&" cmp )*
 *   cmp     := add ( ("=="|"!="|"<"|"<="|">"|">=") add )?
 *   add     := mul ( ("+"|"-") mul )*
 *   mul     := unary ( ("*"|"/"|"%") unary )*
 *   unary   := ("-"|"!") unary | primary
 *   primary := number | name | name "(" args ")" | "(" expr ")"
 *   functions: abs(x) min(a,b) max(a,b) if(c,a,b)
 *
 * A variable that is not available evaluates to NaN and NaN propagates through
 * every operator, so a result is either fully backed by data or NaN.
 */
#include "canary/core.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum {
    OP_NUM, OP_VAR, OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_NEG, OP_NOT, OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT,
    OP_GE, OP_AND, OP_OR, OP_ABS, OP_MIN, OP_MAX, OP_IF
};

typedef struct {
    uint8_t op;
    union {
        double num;
        int var;
    } u;
} ins_t;

struct cn_expr {
    int n;
    ins_t code[];
};

typedef struct {
    const char *s;
    ins_t *code;
    int n, cap;
    cn_expr_resolver resolve;
    void *ctx;
    char *err;
    size_t err_len;
    bool failed;
} parser_t;

static void fail(parser_t *p, const char *msg, const char *detail)
{
    if (p->failed) return;
    p->failed = true;
    if (p->err && p->err_len) snprintf(p->err, p->err_len, "%s%s%s", msg, detail ? ": " : "", detail ? detail : "");
}

static void emit(parser_t *p, uint8_t op, double num, int var)
{
    if (p->failed) return;
    if (p->n == p->cap) {
        int cap = p->cap ? p->cap * 2 : 16;
        ins_t *c = realloc(p->code, sizeof(ins_t) * (size_t)cap);
        if (!c) {
            fail(p, "out of memory", NULL);
            return;
        }
        p->code = c;
        p->cap = cap;
    }
    p->code[p->n].op = op;
    if (op == OP_VAR)
        p->code[p->n].u.var = var;
    else
        p->code[p->n].u.num = num;
    p->n++;
}

static void ws(parser_t *p)
{
    while (*p->s == ' ' || *p->s == '\t' || *p->s == '\n' || *p->s == '\r') p->s++;
}

static bool eat(parser_t *p, const char *tok)
{
    ws(p);
    size_t n = strlen(tok);
    if (strncmp(p->s, tok, n) != 0) return false;
    p->s += n;
    return true;
}

static void parse_or(parser_t *p);

static void parse_primary(parser_t *p)
{
    ws(p);
    if (p->failed) return;
    if (*p->s == '(') {
        p->s++;
        parse_or(p);
        if (!eat(p, ")")) fail(p, "expected )", NULL);
        return;
    }
    if (isdigit((unsigned char)*p->s) || *p->s == '.') {
        char *end;
        double v = strtod(p->s, &end);
        if (end == p->s) {
            fail(p, "bad number", NULL);
            return;
        }
        p->s = end;
        emit(p, OP_NUM, v, 0);
        return;
    }
    if (isalpha((unsigned char)*p->s) || *p->s == '_') {
        char name[48];
        size_t n = 0;
        while (isalnum((unsigned char)*p->s) || *p->s == '_' || *p->s == '.') {
            if (n + 1 < sizeof(name)) name[n++] = *p->s;
            p->s++;
        }
        name[n] = 0;
        ws(p);
        if (*p->s == '(') {
            p->s++;
            int argc = 0;
            ws(p);
            if (*p->s != ')') {
                do {
                    parse_or(p);
                    argc++;
                } while (eat(p, ","));
            }
            if (!eat(p, ")")) {
                fail(p, "expected )", NULL);
                return;
            }
            if (!strcmp(name, "abs") && argc == 1)
                emit(p, OP_ABS, 0, 0);
            else if (!strcmp(name, "min") && argc == 2)
                emit(p, OP_MIN, 0, 0);
            else if (!strcmp(name, "max") && argc == 2)
                emit(p, OP_MAX, 0, 0);
            else if (!strcmp(name, "if") && argc == 3)
                emit(p, OP_IF, 0, 0);
            else
                fail(p, "unknown function or wrong argument count", name);
            return;
        }
        int var = p->resolve ? p->resolve(p->ctx, name) : -1;
        if (var < 0) {
            fail(p, "unknown signal", name);
            return;
        }
        emit(p, OP_VAR, 0, var);
        return;
    }
    fail(p, "unexpected character", NULL);
}

static void parse_unary(parser_t *p)
{
    ws(p);
    if (*p->s == '-') {
        p->s++;
        parse_unary(p);
        emit(p, OP_NEG, 0, 0);
    } else if (*p->s == '!' && p->s[1] != '=') {
        p->s++;
        parse_unary(p);
        emit(p, OP_NOT, 0, 0);
    } else {
        parse_primary(p);
    }
}

static void parse_mul(parser_t *p)
{
    parse_unary(p);
    for (;;) {
        ws(p);
        char c = *p->s;
        if (c != '*' && c != '/' && c != '%') break;
        p->s++;
        parse_unary(p);
        emit(p, c == '*' ? OP_MUL : c == '/' ? OP_DIV : OP_MOD, 0, 0);
    }
}

static void parse_add(parser_t *p)
{
    parse_mul(p);
    for (;;) {
        ws(p);
        char c = *p->s;
        if (c != '+' && c != '-') break;
        p->s++;
        parse_mul(p);
        emit(p, c == '+' ? OP_ADD : OP_SUB, 0, 0);
    }
}

static void parse_cmp(parser_t *p)
{
    parse_add(p);
    ws(p);
    uint8_t op;
    if (eat(p, "=="))
        op = OP_EQ;
    else if (eat(p, "!="))
        op = OP_NE;
    else if (eat(p, "<="))
        op = OP_LE;
    else if (eat(p, ">="))
        op = OP_GE;
    else if (eat(p, "<"))
        op = OP_LT;
    else if (eat(p, ">"))
        op = OP_GT;
    else
        return;
    parse_add(p);
    emit(p, op, 0, 0);
}

static void parse_and(parser_t *p)
{
    parse_cmp(p);
    while (eat(p, "&&")) {
        parse_cmp(p);
        emit(p, OP_AND, 0, 0);
    }
}

static void parse_or(parser_t *p)
{
    parse_and(p);
    while (eat(p, "||")) {
        parse_and(p);
        emit(p, OP_OR, 0, 0);
    }
}

cn_expr *cn_expr_compile(const char *src, cn_expr_resolver resolve, void *ctx, char *err, size_t err_len)
{
    parser_t p = {.s = src, .resolve = resolve, .ctx = ctx, .err = err, .err_len = err_len};
    if (err && err_len) err[0] = 0;
    parse_or(&p);
    ws(&p);
    if (!p.failed && *p.s) fail(&p, "unexpected trailing text", p.s);
    if (p.failed || p.n == 0) {
        if (!p.failed) fail(&p, "empty expression", NULL);
        free(p.code);
        return NULL;
    }
    cn_expr *e = malloc(sizeof(cn_expr) + sizeof(ins_t) * (size_t)p.n);
    if (e) {
        e->n = p.n;
        memcpy(e->code, p.code, sizeof(ins_t) * (size_t)p.n);
    }
    free(p.code);
    return e;
}

void cn_expr_free(cn_expr *e) { free(e); }

double cn_expr_eval(const cn_expr *e, const double *vars)
{
    double st[32];
    int sp = 0;
    for (int i = 0; i < e->n; i++) {
        const ins_t *in = &e->code[i];
        if (in->op == OP_NUM || in->op == OP_VAR) {
            if (sp >= 32) return NAN;
            st[sp++] = in->op == OP_NUM ? in->u.num : vars[in->u.var];
            continue;
        }
        if (in->op == OP_NEG || in->op == OP_NOT || in->op == OP_ABS) {
            if (sp < 1) return NAN;
            double a = st[sp - 1];
            if (isnan(a)) continue;
            st[sp - 1] = in->op == OP_NEG ? -a : in->op == OP_NOT ? (a == 0 ? 1.0 : 0.0) : fabs(a);
            continue;
        }
        if (in->op == OP_IF) {
            if (sp < 3) return NAN;
            double c = st[sp - 3], a = st[sp - 2], b = st[sp - 1];
            sp -= 2;
            st[sp - 1] = (isnan(c) || isnan(a) || isnan(b)) ? NAN : (c != 0 ? a : b);
            continue;
        }
        if (sp < 2) return NAN;
        double b = st[--sp], a = st[sp - 1], r;
        if (isnan(a) || isnan(b)) {
            st[sp - 1] = NAN;
            continue;
        }
        switch (in->op) {
        case OP_ADD: r = a + b; break;
        case OP_SUB: r = a - b; break;
        case OP_MUL: r = a * b; break;
        case OP_DIV: r = b == 0 ? NAN : a / b; break;
        case OP_MOD: r = b == 0 ? NAN : fmod(a, b); break;
        case OP_EQ: r = a == b; break;
        case OP_NE: r = a != b; break;
        case OP_LT: r = a < b; break;
        case OP_LE: r = a <= b; break;
        case OP_GT: r = a > b; break;
        case OP_GE: r = a >= b; break;
        case OP_AND: r = (a != 0) && (b != 0); break;
        case OP_OR: r = (a != 0) || (b != 0); break;
        case OP_MIN: r = a < b ? a : b; break;
        case OP_MAX: r = a > b ? a : b; break;
        default: r = NAN;
        }
        st[sp - 1] = r;
    }
    return sp == 1 ? st[0] : NAN;
}

void cn_expr_vars(const cn_expr *e, void (*fn)(void *ctx, int var), void *ctx)
{
    for (int i = 0; i < e->n; i++)
        if (e->code[i].op == OP_VAR) fn(ctx, e->code[i].u.var);
}
