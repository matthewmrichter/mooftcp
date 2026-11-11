/* tune_parse.c - see tune_parse.h. */
#include <string.h>
#include <stdio.h>
#include "tune_parse.h"

void tn_defaults(tn_vals *v)
{
    v->rto_min = 1000; v->rto_init = 1000; v->delack = 100; v->stall = 10; v->quickack = 4;
}

int tn_preset(const char *id, tn_vals *v)
{
    tn_vals t;
    tn_defaults(&t);
    if (strcmp(id, "default") == 0) { }
    else if (strcmp(id, "delack0") == 0) t.delack = 0;
    else if (strcmp(id, "delack20") == 0) t.delack = 20;
    else if (strcmp(id, "delack50") == 0) t.delack = 50;
    else if (strcmp(id, "qack16") == 0) t.quickack = 16;
    else if (strcmp(id, "qackall") == 0) t.quickack = 255;
    else if (strcmp(id, "rto500") == 0) { t.rto_min = 500; t.rto_init = 500; }
    else if (strcmp(id, "rto700") == 0) { t.rto_min = 700; t.rto_init = 700; }
    else if (strcmp(id, "stall2") == 0) t.stall = 2;
    else if (strcmp(id, "stall5") == 0) t.stall = 5;
    else return 0;
    *v = t;
    return 1;
}

static void fail(char *err, int cap, const char *what, const char *a)
{
    if (err && cap > 0) snprintf(err, (size_t)cap, "%s %.20s", what, a);
}

int tn_parse_cfg(const char *text, long len, tn_vals *v, char *err, int errcap)
{
    long i = 0;
    int applied = 0;
    if (err && errcap > 0) err[0] = 0;
    while (i < len) {
        char key[16], num[16];
        unsigned val = 0;
        int kn = 0, nn = 0, k;
        unsigned *dst = 0;
        while (i < len && (text[i] == ' ' || text[i] == '\t')) i++;
        if (i >= len) break;
        if (text[i] == '\r' || text[i] == '\n') { i++; continue; }
        if (text[i] == '#') { while (i < len && text[i] != '\r' && text[i] != '\n') i++; continue; }
        while (i < len && text[i] != '=' && text[i] != '\r' && text[i] != '\n') {
            if (text[i] != ' ' && text[i] != '\t' && kn < 15) key[kn++] = text[i];
            i++;
        }
        key[kn] = 0;
        if (i >= len || text[i] != '=') { fail(err, errcap, "no '=' after", key); return -1; }
        i++;
        while (i < len && text[i] != '\r' && text[i] != '\n') {
            if (text[i] != ' ' && text[i] != '\t' && nn < 15) num[nn++] = text[i];
            i++;
        }
        num[nn] = 0;
        if (strcmp(key, "rto_min") == 0) dst = &v->rto_min;
        else if (strcmp(key, "rto_init") == 0) dst = &v->rto_init;
        else if (strcmp(key, "delack") == 0) dst = &v->delack;
        else if (strcmp(key, "stall") == 0) dst = &v->stall;
        else if (strcmp(key, "quickack") == 0) dst = &v->quickack;
        else { fail(err, errcap, "unknown key", key); return -1; }
        if (nn == 0 || nn > 5) { fail(err, errcap, "bad number for", key); return -1; }
        for (k = 0; k < nn; k++) {
            if (num[k] < '0' || num[k] > '9') { fail(err, errcap, "bad number for", key); return -1; }
            val = val * 10u + (unsigned)(num[k] - '0');
        }
        if (val > 65535u) { fail(err, errcap, "bad number for", key); return -1; }
        *dst = val;
        applied++;
    }
    return applied;
}
