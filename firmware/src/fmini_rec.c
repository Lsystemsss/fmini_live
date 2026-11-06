/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (FMini-1 fork) */
#ifndef FMINI_REC_C
#define FMINI_REC_C
/* FMini-1 preset records (1.14.0): a record is a run of blocks, each (tag, len, data), len 0..255. A reader skips the
 * tags it does not know, so a parameter added later is one new tag and an older firmware still reads the rest. The
 * tags are stored: a number is never reused for another meaning. No sequencer notes (SEQ) are stored here.
 * Pure functions (no flash, no RAM beyond the caller's buffers): the host test tests/fmini_rec_test.c. */

enum {
    FR_NAME = 1,     /* 1..12 printable ASCII, not 0-terminated */
    FR_PATCH = 2,    /* FG_SIZE bytes: the track's patch (eng_fmini.c) */
    FR_EXTRA = 3,    /* the fmini_x_t bytes: arpeggiator, LFO, FX, level, modulation, octave */
    FR_PARAM = 4,    /* the preset's parameter values: 2 bytes each, little endian, in order */
    FR_PSET = 5,     /* the preset's own settings: PITCH BEND range (0..3), MOD WHEEL position (0..127) */
};

/* append one block; 0 when it does not fit in cap */
static uint32_t fr_put(uint8_t *buf, uint32_t cap, uint32_t n, uint8_t tag, const uint8_t *data, uint32_t len)
{
    if (len > 255u || n + 2u + len > cap)
        return 0u;
    buf[n++] = tag;
    buf[n++] = (uint8_t)len;
    while (len--)
        buf[n++] = *data++;
    return n;
}

/* the block with tag 'want' in buf[0..n): its data (*data, *len) and 1; 0 when there is none or the record is torn */
static int fr_find(const uint8_t *buf, uint32_t n, uint8_t want, const uint8_t **data, uint32_t *len)
{
    uint32_t i = 0;
    while (i + 2u <= n) {
        uint32_t l = buf[i + 1u];
        if (i + 2u + l > n)
            return 0;                          /* a block that runs past the end: the record is damaged */
        if (buf[i] == want) {
            *data = buf + i + 2u;
            *len = l;
            return 1;
        }
        i += 2u + l;                           /* another tag (known or not): skip it whole */
    }
    return 0;
}

/* a record is well formed: every block ends inside it (the unknown tags are fine) */
static int fr_valid(const uint8_t *buf, uint32_t n)
{
    uint32_t i = 0;
    while (i < n) {
        if (i + 2u > n || i + 2u + buf[i + 1u] > n)
            return 0;
        i += 2u + buf[i + 1u];
    }
    return 1;
}

#endif /* FMINI_REC_C */
