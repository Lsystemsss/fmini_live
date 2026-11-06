/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (FMini-1 fork) */
#ifndef FMINI_BANK_C
#define FMINI_BANK_C
/* FMini-1 preset bank (1.14.1): 200 slots, all of them factory patches (000..199). INIT (one sawtooth
 * on oscillator 1, no filter envelope, no effects). A slot that was saved holds its own record (fmini_rec.c); a slot
 * never saved reads its factory record. The records live in 13 storage objects (OBJ_PRESET0..12), 16 slots each; each
 * object starts with a stamp, and a new firmware with another factory bank clears them at boot (install = factory).
 * The name and number of every slot are the only copy in RAM (fb_name); the record is read when a slot is chosen. */

#define FB_NSLOT 200u
#define FB_NOBJ ((FB_NSLOT + FB_PER_OBJ - 1u) / FB_PER_OBJ)   /* 13 objects: the last one holds 8 slots */
#define FB_PER_OBJ 16u
#define FB_SLOT 128u                          /* one record: name 14 + patch 34 + extra 30 + preset 20 bytes, room to grow */
#define FB_PAYLOAD (4u + FB_PER_OBJ * FB_SLOT)
#define FB_INIT_SLOT FMINI_NFACTORY           /* the first INIT slot */

static char fb_name[FB_NSLOT][13];            /* RAM: the list's names (upper case, 0-terminated) */
static uint8_t fb_ready;                      /* the RAM lists are filled (fb_names_init or fb_boot) */
static uint8_t fb_obj_buf[FB_PAYLOAD] __attribute__((aligned(4), section(".pool")));   /* (main loop only) */

/* the INIT patch: a sawtooth on oscillator 1 alone (FG_ bytes, eng_fmini.c) */
static void fb_init_patch(uint8_t *p)
{
    uint32_t i;
    for (i = 0; i < FG_SIZE; i++)
        p[i] = 0;
    p[FG_R1] = 3; p[FG_R2] = 3; p[FG_R3] = 3;   /* 8' */
    p[FG_W1] = 2; p[FG_W2] = 2; p[FG_W3] = 2;   /* SAW */
    p[FG_F2] = 64; p[FG_F3] = 64;               /* 0 detune */
    p[FG_L1] = 127;                             /* oscillator 1 at full, 2 and 3 and the noise off */
    p[FG_SW1] = FS_O1;
    p[FG_TUNE] = 64;
}

/* the record of slot k as the factory has it (k < FMINI_NFACTORY: the bank; else INIT), into rec (FB_SLOT bytes); its length */
static uint32_t fb_factory_rec(uint32_t k, uint8_t *rec)
{
    uint8_t patch[FG_SIZE], par[18];
    fmini_x_t x;
    const char *name = k < FMINI_NFACTORY ? FMINI_PRESETS[k].name : "INIT";
    uint32_t n = 0, i, nl = 0;
    while (nl < 12u && name[nl])
        nl++;
    if (k < FMINI_NFACTORY) {
        memcpy(patch, FMINI_FACTORY[k], FG_SIZE);
        x = FMINI_X[k];
    } else {                                    /* INIT: no arpeggiator, LFO, effects, modulation; the bank's level and octave 0 */
        fb_init_patch(patch);
        memset(&x, 0, sizeof x);
        x.level = FMINI_X[0].level;
    }
    /* PARAM: e[8] (signed), env[4] (ATK DEC SUS REL), fenv, mono, fx[4] (stored +1: 1 = no send) */
    for (i = 0; i < 8; i++)
        par[i] = (uint8_t)(k < FMINI_NFACTORY ? FMINI_PRESETS[k].e[i] : (i == 0 ? 127 : 0));
    for (i = 0; i < 4; i++)
        par[8 + i] = (uint8_t)(k < FMINI_NFACTORY ? FMINI_PRESETS[k].env[i] : (i == 2 ? 127 : 0));
    par[12] = (uint8_t)(k < FMINI_NFACTORY ? FMINI_PRESETS[k].fenv : 0);
    par[13] = (uint8_t)(k < FMINI_NFACTORY ? FMINI_PRESETS[k].mono : 0);
    for (i = 0; i < 4; i++)
        par[14 + i] = (uint8_t)(k < FMINI_NFACTORY ? FMINI_PRESETS[k].fx[i] : 1);
    n = fr_put(rec, FB_SLOT, n, FR_NAME, (const uint8_t *)name, nl);
    n = fr_put(rec, FB_SLOT, n, FR_PATCH, patch, FG_SIZE);
    n = fr_put(rec, FB_SLOT, n, FR_EXTRA, (const uint8_t *)&x, sizeof x);
    n = fr_put(rec, FB_SLOT, n, FR_PARAM, par, sizeof par);
    {   static const uint8_t PS[2] = {0, 0};             /* the factory's PITCH BEND (0 = +-2) and MOD WHEEL (0) */
        n = fr_put(rec, FB_SLOT, n, FR_PSET, PS, sizeof PS);
    }
    return n;
}

/* the name of slot k into fb_name (the list's copy) */
static void fb_set_name(uint32_t k, const uint8_t *name, uint32_t len)
{
    uint32_t i;
    for (i = 0; i < 12u && i < len; i++)
        fb_name[k][i] = (char)name[i];
    fb_name[k][i < 12u ? i : 12u] = 0;
}

/* the record's length: its blocks up to the first empty tag (the slot is zero beyond it) */
static uint32_t fb_len(const uint8_t *rec)
{
    uint32_t i = 0;
    while (i + 2u <= FB_SLOT && rec[i] != 0 && i + 2u + rec[i + 1u] <= FB_SLOT)
        i += 2u + rec[i + 1u];
    return i;
}

/* slot j's record from fb_obj_buf (an object read): its length, 0 when the slot was never saved or is damaged */
static uint32_t fb_slot_from_buf(uint32_t j, uint8_t *rec)
{
    const uint8_t *src = fb_obj_buf + 4u + j * FB_SLOT;
    uint32_t n;
    if (src[0] == 0)
        return 0;
    n = fb_len(src);
    if (n == 0 || !fr_valid(src, n))
        return 0;
    memcpy(rec, src, n);
    return n;
}

/* slot k's preset as the generic code reads it (fmini_pre, eng_fmini.c) from its record n bytes: the name is the RAM copy */
static void fb_fill_pre(uint32_t k, const uint8_t *rec, uint32_t n)
{
    const uint8_t *d = 0;
    uint32_t l = 0, i;
    preset_t *p = &fmini_pre[k];
    p->name = fb_name[k];
    p->pat = 0;                                 /* no sequencer pattern in a preset (the steps are not stored) */
    if (fr_find(rec, n, FR_PARAM, &d, &l) && l == 18u) {
        for (i = 0; i < 8; i++)
            p->e[i] = (int8_t)d[i];
        for (i = 0; i < 4; i++)
            p->env[i] = d[8 + i];
        p->fenv = (int8_t)d[12];
        p->mono = d[13];
        for (i = 0; i < 4; i++)
            p->fx[i] = d[14 + i];
    }
}

/* slot k's whole state into the RAM lists: its name and its preset (its record: saved, or factory) */
static void fb_slot_init(uint32_t k, const uint8_t *rec, uint32_t n)
{
    const uint8_t *d = 0;
    uint32_t l = 0;
    if (fr_find(rec, n, FR_NAME, &d, &l))
        fb_set_name(k, d, l);
    fb_fill_pre(k, rec, n);
}

#if FELUCCA_FLASH
static int fb_obj_read(uint32_t o);
static int fb_obj_clear(uint32_t o);
#endif
/* slot k's record into rec: the saved one, else its factory record; its length */
#if !FELUCCA_FLASH
static uint8_t fb_host[FB_NSLOT][FB_SLOT];    /* host build (tests): the saved records in RAM instead of flash */
static uint8_t fb_host_n[FB_NSLOT];
#endif
static uint32_t fb_read(uint32_t k, uint8_t *rec)
{
#if FELUCCA_FLASH
    if (k < FB_NSLOT && fb_obj_read(k / FB_PER_OBJ)) {
        uint32_t n = fb_slot_from_buf(k % FB_PER_OBJ, rec);
        if (n)
            return n;
    }
#else
    if (k < FB_NSLOT && fb_host_n[k]) {
        memcpy(rec, fb_host[k], fb_host_n[k]);
        return fb_host_n[k];
    }
#endif
    return fb_factory_rec(k, rec);
}

/* slot k's arpeggiator, LFO, level, FX and octave (the FMINI_X block of its record); zeros if the record is damaged */
static void fb_extra(uint32_t k, fmini_x_t *x)
{
    uint8_t rec[FB_SLOT];
    uint32_t n = fb_read(k, rec), l = 0;
    const uint8_t *d = 0;
    if (fr_find(rec, n, FR_EXTRA, &d, &l) && l == sizeof *x)
        memcpy(x, d, sizeof *x);
    else
        memset(x, 0, sizeof *x);
}

/* slot k's PITCH BEND range and MOD WHEEL position (the FR_PSET block; 0 and 0 when it is not there, as the factory's are) */
static void fb_pset(uint32_t k, uint8_t *bend, uint8_t *wheel)
{
    uint8_t rec[FB_SLOT];
    uint32_t n = fb_read(k, rec), l = 0;
    const uint8_t *d = 0;
    *bend = 0;
    *wheel = 0;
    if (fr_find(rec, n, FR_PSET, &d, &l) && l == 2u) {
        *bend = (uint8_t)(d[0] & 3u);
        *wheel = (uint8_t)(d[1] & 127u);
    }
}

#if FELUCCA_FLASH
/* save slot k (its record rec, n bytes) into its object: the other slots of the object are kept. 0 = written */
static int fb_write(uint32_t k, const uint8_t *rec, uint32_t n)
{
    uint32_t o = k / FB_PER_OBJ, j = k % FB_PER_OBJ, i;
    uint8_t *dst;
    if (k >= FB_NSLOT || n == 0u || n > FB_SLOT || !fr_valid(rec, n))
        return -1;
    if (!fb_obj_read(o))
        fb_obj_clear(o);                        /* no bank here yet: a bank of stamp and empty slots */
    dst = fb_obj_buf + 4u + j * FB_SLOT;
    for (i = 0; i < FB_SLOT; i++)
        dst[i] = i < n ? rec[i] : 0;
    if (st_save(OBJ_PRESET0 + o, fb_obj_buf, FB_PAYLOAD) != 0)
        return -1;
    fb_slot_init(k, rec, n);                    /* the list and the preset now show the saved slot */
    return 0;
}
#else
/* host build: the same save, into the RAM copy (no flash) */
static int fb_write(uint32_t k, const uint8_t *rec, uint32_t n)
{
    if (k >= FB_NSLOT || n == 0u || n > FB_SLOT || !fr_valid(rec, n))
        return -1;
    memcpy(fb_host[k], rec, n);
    fb_host_n[k] = (uint8_t)n;
    fb_slot_init(k, rec, n);
    return 0;
}
#endif

/* the stamp of this build's factory bank: a hash of the 200 patches and their names (1.14.9), so a firmware whose bank
 * differs clears the saved slots at boot (the comment at the top says so; a fixed stamp never changed) */
static uint32_t fb_stamp_v;
static uint32_t fb_stamp(void)
{
    uint32_t h = 2166136261u, i, k;
    if (fb_stamp_v)
        return fb_stamp_v;
    for (i = 0; i < FMINI_NFACTORY * FG_SIZE; i++)
        h = (h ^ ((const uint8_t *)FMINI_FACTORY)[i]) * 16777619u;
    for (k = 0; k < FMINI_NFACTORY; k++)
        for (i = 0; FMINI_PRESETS[k].name[i]; i++)
            h = (h ^ (uint8_t)FMINI_PRESETS[k].name[i]) * 16777619u;
    fb_stamp_v = h | 1u;
    return fb_stamp_v;
}

#if FELUCCA_FLASH
/* object o into fb_obj_buf; 1 when it was read and its stamp is this bank's (an empty bank is a stamp only) */
static int fb_obj_read(uint32_t o)
{
    uint32_t stamp;
    if (!flash_ok || st_load(OBJ_PRESET0 + o, fb_obj_buf, FB_PAYLOAD) != (int)FB_PAYLOAD)
        return 0;
    stamp = (uint32_t)fb_obj_buf[0] | (uint32_t)fb_obj_buf[1] << 8 | (uint32_t)fb_obj_buf[2] << 16 | (uint32_t)fb_obj_buf[3] << 24;
    return stamp == fb_stamp();
}

/* a new object: the stamp and no saved slots (every slot is its factory record); its save result: 0 = written */
static int fb_obj_clear(uint32_t o)
{
    uint32_t i;
    for (i = 0; i < FB_PAYLOAD; i++)
        fb_obj_buf[i] = 0;
    fb_obj_buf[0] = (uint8_t)fb_stamp();
    fb_obj_buf[1] = (uint8_t)(fb_stamp() >> 8);
    fb_obj_buf[2] = (uint8_t)(fb_stamp() >> 16);
    fb_obj_buf[3] = (uint8_t)(fb_stamp() >> 24);
    return flash_ok ? st_save(OBJ_PRESET0 + o, fb_obj_buf, FB_PAYLOAD) : -1;
}

/* the bank at boot (after persist_boot: flash_ok is known): an object without this bank's stamp (another firmware's
 * bank, or none) is cleared, so every slot reads its factory record. Once per boot: 16 object reads, and the names. */
static void fb_boot(void)
{
    uint32_t o, j, k;
    uint8_t rec[FB_SLOT];
    fb_ready = 1;
    for (o = 0; o < FB_NOBJ; o++) {
        if (!fb_obj_read(o))
            fb_obj_clear(o);                    /* (a failed write leaves the factory reads: nothing saved) */
        for (j = 0; j < FB_PER_OBJ; j++) {
            uint32_t n;
            k = o * FB_PER_OBJ + j;
            if (k >= FB_NSLOT)
                break;
            n = fb_slot_from_buf(j, rec);
            if (!n)
                n = fb_factory_rec(k, rec);     /* never saved, or damaged: the factory's record */
            fb_slot_init(k, rec, n);
        }
    }
}
#endif

/* the patch of slot s (any of the 256) into dst: its record's, the factory's if the record is damaged */
static void fmini_slot_patch(uint32_t s, uint8_t *dst)
{
    uint8_t rec[FB_SLOT];
    uint32_t n = fb_read(s, rec), l = 0;
    const uint8_t *d = 0;
    if (fr_find(rec, n, FR_PATCH, &d, &l) && l == FG_SIZE)
        memcpy(dst, d, FG_SIZE);
    else
        memcpy(dst, FMINI_FACTORY[s < FMINI_NFACTORY ? s : 0], FG_SIZE);
}

/* the RAM list at boot: every slot's name, from its record (saved, or the factory's) */
static void fb_names_init(void)
{
    uint32_t k;
    uint8_t rec[FB_SLOT];
    if (fb_ready)                               /* the flash read (fb_boot) has the saved slots: keep them */
        return;
    fb_ready = 1;
    for (k = 0; k < FB_NSLOT; k++) {
        uint32_t n = fb_factory_rec(k, rec);    /* (host path: no flash, so the factory records) */
        fb_slot_init(k, rec, n);
    }
}

/* RESTORE (1.14.1-protocol 4): the editor writes all 200 records only after the user has answered on the device (the
 * question, NO by default: fmini_app.c). ASK: the question is up; OK: accepted, records may be written; NO: refused.
 * Without a word from the editor for 60 s the question is NO and an accepted restore ends (IDLE). */
enum { FB_RS_IDLE = 0, FB_RS_ASK, FB_RS_OK, FB_RS_NO };
#define FB_RS_TIMEOUT 60000u
static uint8_t fb_rs;                         /* the state (the device's answer is in it) */
/* what the question is for (1.14.1): a slot (< FB_NSLOT: its factory preset, done on the device at YES), FB_NSLOT (a backup:
 * the editor writes the presets after OK) or FB_RS_ALL (every factory preset, done at YES) */
#define FB_RS_ALL (FB_NSLOT + 1u)
static uint32_t fb_rs_k = FB_NSLOT;
static uint8_t fb_rs_yes;                     /* the question's answer as ALGORITHM sets it: 1 = YES (NO by default) */
static uint32_t fb_rs_t;                      /* the last word from the editor or the user */
static uint8_t fb_rs_state(void)
{
    if ((fb_rs == FB_RS_ASK || fb_rs == FB_RS_OK) && (uint32_t)(fm1_ms - fb_rs_t) >= FB_RS_TIMEOUT)
        fb_rs = fb_rs == FB_RS_ASK ? FB_RS_NO : FB_RS_IDLE;
    return fb_rs;
}
static void fb_rs_touch(void) { fb_rs_t = fm1_ms; }

/* FACTORY RESET (protocol 4): the slot's saved record forgotten, so it reads its factory record (a factory patch for every slot now) from
 * now on, and a later firmware's factory bank shows. 0 = done */
static int fb_forget(uint32_t k)
{
    uint8_t rec[FB_SLOT];
    uint32_t n;
    if (k >= FB_NSLOT)
        return -1;
#if FELUCCA_FLASH
    {
        uint32_t o = k / FB_PER_OBJ, j = k % FB_PER_OBJ, i;
        if (!fb_obj_read(o))
            fb_obj_clear(o);
        for (i = 0; i < FB_SLOT; i++)
            fb_obj_buf[4u + j * FB_SLOT + i] = 0;
        if (st_save(OBJ_PRESET0 + o, fb_obj_buf, FB_PAYLOAD) != 0)
            return -1;
    }
#else
    fb_host_n[k] = 0;
#endif
    n = fb_factory_rec(k, rec);
    fb_slot_init(k, rec, n);
    return 0;
}

/* the question's YES for a reset (fb_rs_k): one slot's or all the factory presets' (fb_forget). 0 = done */
static int fb_rs_apply(void)
{
    uint32_t k;
    if (fb_rs_k == FB_RS_ALL) {
        for (k = 0; k < FB_NSLOT; k++)
            if (fb_forget(k) != 0)
                return -1;
        return 0;
    }
    return fb_rs_k < FB_NSLOT ? fb_forget(fb_rs_k) : 0;
}

#endif /* FMINI_BANK_C */
