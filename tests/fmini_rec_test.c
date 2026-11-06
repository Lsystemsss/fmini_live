/* SPDX-License-Identifier: GPL-3.0-only
 * FMini-1 preset records (firmware/src/fmini_rec.c): blocks round trip, an unknown tag is skipped, a torn record is
 * refused, the size of a typical preset. Host only: cc -O1 -w -Ifirmware/src -o OUT tests/fmini_rec_test.c */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "fmini_rec.c"

static int bad;
static void check(int ok, const char *what) { printf("fmini_rec: %s %s\n", what, ok ? "ok" : "FAIL"); if (!ok) bad++; }

int main(void)
{
    uint8_t rec[256], patch[32], extra[28], name[12], out[32];
    uint32_t n = 0, len = 0;
    const uint8_t *d = 0;
    uint32_t i;
    for (i = 0; i < 32; i++) patch[i] = (uint8_t)(i * 3);
    for (i = 0; i < 28; i++) extra[i] = (uint8_t)(i + 1);
    memcpy(name, "MINI LEAD", 9);

    n = fr_put(rec, sizeof rec, n, FR_NAME, name, 9);
    n = fr_put(rec, sizeof rec, n, FR_PATCH, patch, 32);
    n = fr_put(rec, sizeof rec, n, FR_EXTRA, extra, 28);
    check(n == 2 + 9 + 2 + 32 + 2 + 28, "a preset's blocks: the record size");
    check(fr_valid(rec, n), "a written record is well formed");

    check(fr_find(rec, n, FR_PATCH, &d, &len) && len == 32 && !memcmp(d, patch, 32), "PATCH found and equal");
    check(fr_find(rec, n, FR_EXTRA, &d, &len) && len == 28 && !memcmp(d, extra, 28), "EXTRA found and equal");
    check(fr_find(rec, n, FR_NAME, &d, &len) && len == 9 && !memcmp(d, name, 9), "NAME found and equal");
    check(!fr_find(rec, n, FR_PARAM, &d, &len), "a block the record does not hold: not found");

    /* a newer firmware's record: an unknown tag 9 in the middle; the old reader still finds the rest */
    {
        uint8_t fut[64], unk[3] = {7, 8, 9};
        uint32_t m = 0;
        m = fr_put(fut, sizeof fut, m, FR_NAME, name, 9);
        m = fr_put(fut, sizeof fut, m, 9, unk, 3);
        m = fr_put(fut, sizeof fut, m, FR_PATCH, patch, 32);
        check(fr_valid(fut, m), "a record with an unknown tag is well formed");
        check(fr_find(fut, m, FR_PATCH, &d, &len) && len == 32 && !memcmp(d, patch, 32),
              "the known block after an unknown tag is found");
    }

    /* torn: the last block runs past the end (a cut write) */
    check(!fr_valid(rec, n - 1) && !fr_find(rec, n - 1, FR_EXTRA, &d, &len), "a torn record is refused");
    /* a block longer than the room is not written */
    check(fr_put(rec, 10, 0, FR_PATCH, patch, 32) == 0, "a block that does not fit is refused");
    check(fr_put(rec, sizeof rec, 0, FR_PATCH, patch, 256) == 0, "a block longer than 255 is refused");
    (void)out;

    /* the size of a whole preset without the sequencer: name 12 + patch 32 + extra 28 + 8 parameters (16 bytes) */
    {
        uint32_t size = 0;
        uint8_t par[16];
        memset(par, 0, sizeof par);
        size = fr_put(rec, sizeof rec, 0, FR_NAME, name, 12);
        size = fr_put(rec, sizeof rec, size, FR_PATCH, patch, 32);
        size = fr_put(rec, sizeof rec, size, FR_EXTRA, extra, 28);
        size = fr_put(rec, sizeof rec, size, FR_PARAM, par, 16);
        check(size <= 96u, "a typical preset is at most 96 bytes");
        printf("fmini_rec: a typical preset is %u bytes\n", (unsigned)size);
    }
    printf(bad ? "FMINI REC TEST FAILED\n" : "fmini_rec test passed\n");
    return bad != 0;
}
