/* common/sft_version.h: which version is newer (the updaters) */
#include <stdio.h>
#include "../sft_version.h"

static int fails;
static void expect(const char *a, const char *b, int want)
{
    int got = sft_version_cmp(a, b);
    got = got < 0 ? -1 : got > 0 ? 1 : 0;
    if (got != want) { printf("FAIL %s vs %s: %d, want %d\n", a, b, got, want); fails++; }
}

int main(void)
{
    expect("0.7.14", "0.7.13.2", 1);
    expect("v0.7.14", "0.7.14", 0);
    expect("0.7.14-beta2", "0.7.14-beta1", 1);
    expect("0.7.14", "0.7.14-beta2", 1);
    expect("0.7.14-rc1", "0.7.14-beta9", 1);
    expect("0.7.14-beta1", "0.7.14-test9", 1);
    expect("0.7.13.2", "0.7.14-beta1", -1);
    expect("0.7.14.1", "0.7.14", 1);
    expect("0.8", "0.7.99", 1);
    expect("0.7.14-beta10", "0.7.14-beta9", 1);
    expect("dev", "0.7.14", -1);
    printf("%s (%d failure%s)\n", fails ? "FAILED" : "OK", fails, fails == 1 ? "" : "s");
    return fails != 0;
}
