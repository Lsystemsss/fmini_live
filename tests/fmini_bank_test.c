/* SPDX-License-Identifier: GPL-3.0-only
 * FMini-1 preset bank (1.14.0, firmware/src/fmini_bank.c): every slot's factory record is well formed and fits its
 * slot; slots 0..99 are the factory presets, 100..255 INIT (a sawtooth on oscillator 1, no arpeggiator, LFO, effects,
 * modulation, no filter envelope); the RAM name list holds every name. Host only:
 * cc -O1 -w -Ibuild/gen -Ifirmware/src -Itests -o OUT tests/fmini_bank_test.c -lm */
#define UI_TEST_NO_MAIN 1
#define FELUCCA_FMINI_ONLY 1
#include "ui_test.c"
#include "../firmware/src/fmini_app.c"
#include "../firmware/src/fmini_rec.c"
#include "../firmware/src/fmini_bank.c"
#include <stdio.h>

static int bad;
static void fbchk(int ok, const char *what) { printf("fmini_bank: %s %s\n", what, ok ? "ok" : "FAIL"); if (!ok) bad++; }

int main(void)
{
    uint8_t rec[FB_SLOT];
    const uint8_t *d = 0;
    uint32_t l = 0, k, n, i, fails = 0, overlong = 0;

    /* every slot's record: well formed, in its slot */
    for (k = 0; k < FB_NSLOT; k++) {
        n = fb_factory_rec(k, rec);
        if (n > FB_SLOT || !fr_valid(rec, n))
            overlong++;
        if (!fr_find(rec, n, FR_NAME, &d, &l) || l == 0 || l > 12)
            fails++;
    }
    fbchk(overlong == 0, "every slot's record is well formed and fits the slot");
    fbchk(fails == 0, "every slot has a name of 1..12 characters");

    /* factory slot 0: name, patch and extras as the bank has them */
    n = fb_factory_rec(0, rec);
    fbchk(fr_find(rec, n, FR_NAME, &d, &l) && l == 9 && !memcmp(d, "MINI LEAD", 9), "slot 0 is MINI LEAD");
    fbchk(fr_find(rec, n, FR_PATCH, &d, &l) && l == FG_SIZE && !memcmp(d, FMINI_FACTORY[0], FG_SIZE),
          "slot 0's patch is the factory patch");
    fbchk(fr_find(rec, n, FR_EXTRA, &d, &l) && l == sizeof(fmini_x_t) && !memcmp(d, &FMINI_X[0], sizeof(fmini_x_t)),
          "slot 0's extras are the factory extras");

    /* INIT (past the last factory slot: no slot has it now, 1.14.1): sawtooth on oscillator 1 alone, nothing else */
    n = fb_factory_rec(FMINI_NFACTORY, rec);
    fbchk(fr_find(rec, n, FR_NAME, &d, &l) && l == 4 && !memcmp(d, "INIT", 4), "past the factory slots is INIT");
    fbchk(fr_find(rec, n, FR_PATCH, &d, &l) && l == FG_SIZE, "INIT has a patch");
    if (fr_find(rec, n, FR_PATCH, &d, &l)) {
        const uint8_t *p = d;
        fbchk(p[FG_W1] == 2 && p[FG_L1] == 127 && p[FG_SW1] == FS_O1 && p[FG_L2] == 0 && p[FG_L3] == 0 && p[FG_LN] == 0 &&
              p[FG_F2] == 64 && p[FG_F3] == 64, "INIT: a saw on oscillator 1 at full, the others off, no detune");
    }
    if (fr_find(rec, n, FR_EXTRA, &d, &l)) {
        const fmini_x_t *x = (const fmini_x_t *)d;
        int off = 1;
        for (i = 0; i < 7; i++) off &= x->arp[i] == 0;
        for (i = 0; i < 4; i++) off &= x->lfo[i] == 0;
        for (i = 0; i < 12; i++) off &= x->fx[i] == 0;
        for (i = 0; i < 3; i++) off &= x->mod[i] == 0;
        fbchk(off && x->oct == 0 && x->level == FMINI_X[0].level, "INIT: no arpeggiator, LFO, effects or modulation");
    }
    if (fr_find(rec, n, FR_PARAM, &d, &l)) {
        fbchk(d[0] == 127 && d[1] == 0 && d[8 + 2] == 127 && d[12] == 0 && d[14] == 1 && d[17] == 1,
              "INIT: cutoff open, sustain full, no filter envelope, no sends");
    }

    /* the RAM names: every slot named by its factory patch (1.14.1: 200 factory patches, no INIT slot) */
    fb_names_init();
    fbchk(!strcmp(fb_name[0], "MINI LEAD") && !strcmp(fb_name[100], FMINI_PRESETS[100].name) && !strcmp(fb_name[FB_NSLOT - 1], FMINI_PRESETS[FB_NSLOT - 1].name),
          "the RAM name list: slot 0 and the last slots");

    /* the load waits: a SLOT loads once it has been still for 50 ms; each turn starts the count again */
    {
        uint32_t t0 = 1000u;
        uint8_t want[FB_SLOT];
        (void)want;
        trk[0].eng_req = ENGI_FMINI;
        fmini_slot[0] = 0xFFFFu;
        fm1_ms = t0;
        trk[0].p[P_E7] = 5;
        fmini_poll();
        fbchk(fmini_slot[0] != 5u, "a turned slot is not loaded at once");
        fm1_ms = t0 + 30u;
        trk[0].p[P_E7] = 6;                        /* turned again: the count restarts */
        fmini_poll();
        fm1_ms = t0 + 60u;                         /* 30 ms since the last turn */
        fmini_poll();
        fbchk(fmini_slot[0] != 6u, "still turning (30 ms since the last change): not loaded");
        fm1_ms = t0 + 80u;                         /* 50 ms since the last turn */
        fmini_poll();
        fbchk(fmini_slot[0] == 6u, "still for 50 ms: the slot loads");
    }

    printf(bad ? "FMINI BANK TEST FAILED\n" : "fmini_bank test passed\n");
    return bad != 0;
}
