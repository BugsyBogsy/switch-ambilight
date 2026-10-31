/*
 * v0.7.14: which Switch Frame Tap version is newer - for the updaters (the
 * Switch app, the PC viewers). C and C++.
 *
 * "0.7.14", "v0.7.14-beta2", "0.7.13.2": the numbers compare first (a
 * missing one is 0); with the same numbers a release is newer than its
 * -test / -beta / -rc builds, and those compare by kind, then number
 * (0.7.14-beta2 > 0.7.14-beta1 > 0.7.14-test9).
 */
#ifndef SFT_VERSION_H
#define SFT_VERSION_H

#include <ctype.h>
#include <string.h>

typedef struct { long n[4]; int kind; long sub; } sft_version_t;   /* kind: 3 release, 2 rc, 1 beta, 0 anything else */

static inline sft_version_t sft_version_parse(const char *s)
{
    sft_version_t v = { { 0, 0, 0, 0 }, 3, 0 };
    if (s == NULL) return v;
    if (*s == 'v' || *s == 'V') s++;
    for (int i = 0; i < 4 && isdigit((unsigned char)*s); i++) {
        while (isdigit((unsigned char)*s)) v.n[i] = v.n[i] * 10 + (*s++ - '0');
        if (*s == '.') s++;
    }
    if (*s == '-') {
        s++;
        v.kind = !strncmp(s, "rc", 2) ? 2 : !strncmp(s, "beta", 4) ? 1 : 0;
        while (*s && !isdigit((unsigned char)*s)) s++;
        while (isdigit((unsigned char)*s)) v.sub = v.sub * 10 + (*s++ - '0');
    }
    return v;
}

/* < 0: a is older than b; 0: the same; > 0: a is newer */
static inline int sft_version_cmp(const char *a, const char *b)
{
    const sft_version_t x = sft_version_parse(a), y = sft_version_parse(b);
    for (int i = 0; i < 4; i++) if (x.n[i] != y.n[i]) return x.n[i] < y.n[i] ? -1 : 1;
    if (x.kind != y.kind) return x.kind < y.kind ? -1 : 1;
    if (x.sub != y.sub) return x.sub < y.sub ? -1 : 1;
    return 0;
}

#endif
