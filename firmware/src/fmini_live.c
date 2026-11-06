/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef FMINI_LIVE_C
#define FMINI_LIVE_C
#if FELUCCA_FMINI_ONLY
/* FMini-1 LIVE mode: a library of 16 slots. Each slot has a MIDI channel (1..16, or OFF), a sound (a preset 000..199) and,
 * if you want, a second sound, the layer, that plays together with the first.
 *
 * HOME held 0.5 s opens it (and closes it: the tap on HOME still goes to the first page as before). In LIVE the screen is the
 * list; SELECT picks a slot, K1 its CHANNEL, K2 (or PRESETS) its SOUND, K3 its LAYER (a turn left of 000 is NONE), K4 the
 * VOLUME of the sound (A) and ALGORITHM the VOLUME of the layer (B): a trim in dB on the preset's own level (-24..+7 dB, 1 dB
 * a step; the level curve is 0.5 dB a step, so it is exact), heard at once on the slot that is loaded. The slot
 * the list is on is loaded after 0.12 s, so the keys play it while you edit. A note that comes on a slot's channel loads that
 * slot first: its sound goes into track 1 and its layer into track 2, and the keys, MIDI, the arpeggiator and the sequencer
 * of track 1 play track 2 too (voice.c trk_note_on). One slot is loaded at a time: a note on another slot's channel replaces
 * it. The notes, pitch bend and CCs of a channel no slot is set to are dropped, and so is Program Change (the slot is the
 * sound). CCs and the mod wheel move the first sound only.
 *
 * Who does what: the audio ISR (seq.c events_block, lv_route) only looks up the slot a channel plays (lv_map) and, for a note
 * on a slot that is not loaded, asks for it (lv_req) and waits with that message at the head of the queue; the main loop
 * (lv_poll, from fa_input) loads the slot, since a sound load reads the flash. The slots are kept in the settings
 * record, favorites.factory rows 4 and 13 (the favourites of SAMPLE and SLICE, retired in 1.13: no screen or editor of this
 * firmware sets them), packed at 31 bits a slot behind a marker and a checksum: anything else in those bytes reads as the
 * defaults (slot n: channel n, preset n, no layer, 0 dB).
 * Included by fmini_app.c before fa_scene. */

#define LV_N 16u                                          /* the slots */
#define LV_OFF 16u                                        /* a slot's channel: OFF, no MIDI channel plays it */
#define LV_NOB 0xFFu                                      /* a slot without a layer */
#define LV_HOLD_MS 500u                                   /* HOME held this long: LIVE on / off */
#define LV_LOAD_MS 120u                                   /* the list stays on a slot this long: it loads (the keys play it) */
#define LV_SAVE_MS 2000u                                  /* an edit is written to the settings this long after the last one */
#define LV_MAGIC 0xF2u                                    /* (the marker is the version: 0xF1, the first record, had no volumes) */
#define LV_BYTES 64u                                      /* the settings bytes taken: marker, 62 of slots, checksum */
#define LV_BITS 31u                                       /* a slot: channel 5, sound 8, layer 8, volume of A 5, of B 5 */
#define LV_TMIN (-24)                                     /* a volume: dB on the preset's own level, -24..+7 (5 bits) */
#define LV_TMAX 7

static uint8_t lv_ch[LV_N];                               /* the slot's MIDI channel 0..15, LV_OFF */
static uint8_t lv_a[LV_N];                                /* its sound: a preset 0..FMINI_NSLOT - 1 */
static uint8_t lv_b[LV_N];                                /* its layer: a preset, LV_NOB none */
static int8_t lv_ta[LV_N], lv_tb[LV_N];                   /* the volume of A and of B, dB */
static int16_t lv_base[2];                                /* the levels the presets of the loaded slot brought (the volume is added to these) */
static uint8_t lv_row;                                    /* the slot the list is on */
static uint8_t lv_want = 0xFF;                            /* the slot to load when the list has stayed on it (0xFF none) */
static uint32_t lv_want_ms;
static uint8_t lv_dirty;                                  /* the slots changed, not yet written to the settings */
static uint32_t lv_dirty_ms;
static int16_t lv_prev = -1;                              /* the preset track 1 had when LIVE opened: it comes back on exit */
static uint8_t lv_hd;                                     /* HOME is down and may be held */
static uint32_t lv_hd_t0;

/* ------------------------------------------------------------ slots --- */
static void lv_defaults(void)
{
    uint32_t s;
    for (s = 0; s < LV_N; s++) {
        lv_ch[s] = (uint8_t)s;                            /* slot n plays on channel n + 1 */
        lv_a[s] = (uint8_t)s;                             /* with preset n */
        lv_b[s] = LV_NOB;
        lv_ta[s] = lv_tb[s] = 0;
    }
}

/* the channel -> slot table the ISR reads: the first slot set to each channel, + 1 */
static void lv_map_build(void)
{
    uint8_t m[16];
    uint32_t s;
    for (s = 0; s < 16u; s++)
        m[s] = 0;
    for (s = 0; s < LV_N; s++)
        if (lv_ch[s] < 16u && !m[lv_ch[s]])
            m[lv_ch[s]] = (uint8_t)(s + 1u);
    fm1_irq_off();                                        /* (the ISR never sees half a table) */
    for (s = 0; s < 16u; s++)
        lv_map[s] = m[s];
    fm1_irq_on();
}

/* ---------------------------------------------------------- settings --- */
static uint8_t *lv_cfg_byte(uint32_t i) { return &favorites.factory[i < 32u ? 4u : 13u][i & 31u]; }
static uint8_t lv_cfg_sum(void)
{
    uint32_t i, h = 0xA5u;
    for (i = 1; i < LV_BYTES - 1u; i++)
        h = (h * 31u + *lv_cfg_byte(i)) & 0xFFu;
    return (uint8_t)h;
}
/* n (<= 8) bits at bit p of the 62 bytes of slots (cfg bytes 1..62), least significant first */
static uint32_t lv_bits_get(uint32_t p, uint32_t n)
{
    uint32_t k, v = 0;
    for (k = 0; k < n; k++)
        v |= ((uint32_t)(*lv_cfg_byte(1u + (p + k) / 8u) >> ((p + k) % 8u)) & 1u) << k;
    return v;
}
static void lv_bits_set(uint32_t p, uint32_t n, uint32_t v)
{
    uint32_t k;
    for (k = 0; k < n; k++) {
        uint8_t *c = lv_cfg_byte(1u + (p + k) / 8u);
        uint8_t m = (uint8_t)(1u << ((p + k) % 8u));
        *c = (uint8_t)((v >> k) & 1u ? *c | m : *c & ~m);
    }
}
static void lv_cfg_load(void)
{
    uint32_t s, ok = *lv_cfg_byte(0) == LV_MAGIC && *lv_cfg_byte(LV_BYTES - 1u) == lv_cfg_sum();
    for (s = 0; ok && s < LV_N; s++) {                    /* a value out of range: the whole record is not ours */
        uint32_t p = s * LV_BITS, c = lv_bits_get(p, 5), a = lv_bits_get(p + 5u, 8), b = lv_bits_get(p + 13u, 8);
        if (c > LV_OFF || a >= FMINI_NSLOT || (b != LV_NOB && b >= FMINI_NSLOT))
            ok = 0;
    }
    if (!ok) {
        lv_defaults();
    } else {
        for (s = 0; s < LV_N; s++) {
            uint32_t p = s * LV_BITS;
            lv_ch[s] = (uint8_t)lv_bits_get(p, 5);
            lv_a[s] = (uint8_t)lv_bits_get(p + 5u, 8);
            lv_b[s] = (uint8_t)lv_bits_get(p + 13u, 8);
            lv_ta[s] = (int8_t)((int32_t)lv_bits_get(p + 21u, 5) + LV_TMIN);
            lv_tb[s] = (int8_t)((int32_t)lv_bits_get(p + 26u, 5) + LV_TMIN);
        }
    }
    lv_map_build();
}
/* the slots into the settings' bytes; the flash copy is settings_save's (deferred while anything plays) */
static void lv_cfg_save(void)
{
    uint32_t s;
    lv_dirty = 0;
    *lv_cfg_byte(0) = LV_MAGIC;
    for (s = 0; s < LV_N; s++) {
        uint32_t p = s * LV_BITS;
        lv_bits_set(p, 5, lv_ch[s]);
        lv_bits_set(p + 5u, 8, lv_a[s]);
        lv_bits_set(p + 13u, 8, lv_b[s]);
        lv_bits_set(p + 21u, 5, (uint32_t)((int32_t)lv_ta[s] - LV_TMIN));
        lv_bits_set(p + 26u, 5, (uint32_t)((int32_t)lv_tb[s] - LV_TMIN));
    }
    *lv_cfg_byte(LV_BYTES - 1u) = lv_cfg_sum();
    settings_save();
}

/* ------------------------------------------------------------- sound --- */
/* slot s into the tracks: its sound into track 1, its layer into track 2 (unmuted), or track 2 muted without one. What
 * sounds on the two tracks lets go first (panic_req); the layer is off while track 2 changes (the ISR) */
/* a preset's level (P_LEVEL, 0.5 dB a step, 0 = off) with the slot's volume (dB) on it */
static int16_t lv_level(int32_t base, int32_t db)
{
    return (int16_t)(base <= 0 ? 0 : clamp(base + 2 * db, 1, 127));
}
static void lv_apply(uint32_t s)
{
    track_t *t = &trk[0], *u = &trk[1];
    uint32_t a = lv_a[s], b = lv_b[s];
    lv_layer = 0;
    panic_req |= 3u;
    t->user = 0;
    apply_preset_to(t, a);
    t->p[P_E7] = (int16_t)a;
    fa_factory_extras(t, a);                              /* its arpeggiator, LFO, FX settings, octave */
    lv_base[0] = t->p[P_LEVEL];
    t->p[P_LEVEL] = lv_level(lv_base[0], lv_ta[s]);
    if (b != LV_NOB) {
        apply_preset_to(u, b);
        u->p[P_E7] = (int16_t)b;
        fa_factory_extras(u, b);
        lv_base[1] = u->p[P_LEVEL];
        u->p[P_LEVEL] = lv_level(lv_base[1], lv_tb[s]);
        u->p[P_AMODE] = 0;                                /* track 1's arpeggiator feeds track 2 through trk_note_on */
        u->p[P_MUTE] = 0;
        u->p[P_GLIDE] = (int16_t)((fmini_patch[1][FG_SW2] & FT_GLIDE) ? fmini_patch[1][FG_GLID] : 0u);   /* (as fmg_glide_sync does for track 1) */
        lv_layer = 1;
    } else {
        u->p[P_MUTE] = 1;
    }
    lv_cur = (uint8_t)s;
    ui.force = 1;
}

static void lv_enter(void)
{
    lv_prev = trk[0].p[P_E7] >= 0 && trk[0].p[P_E7] < (int32_t)FMINI_NSLOT ? trk[0].p[P_E7] : (int16_t)-1;
    fa_sv = fa_ar = fa_sc = fa_sq = fa_sq_rec = 0;        /* the pop-ups close */
    fa_tp = 0;
    fa_pset_pop = 0;
    fmg_fx = 0;
    fa_gl_close();
    lv_map_build();
    lv_cur = 0xFF;
    lv_req = 0;
    lv_hd = 0;
    lv_want = lv_row;
    lv_want_ms = fm1_ms - LV_LOAD_MS;                     /* the slot the list was on loads now */
    fm_live = 1;
    ui.force = 1;
}

static void lv_exit(void)
{
    fm_live = 0;
    lv_layer = 0;
    lv_req = 0;
    lv_cur = 0xFF;
    lv_want = 0xFF;
    lv_hd = 0;
    panic_req |= 3u;
    trk[1].p[P_MUTE] = 1;
    if (lv_prev >= 0) {                                   /* the sound that was there (a patch of the player's own cannot come back) */
        apply_preset_to(&trk[0], (uint32_t)lv_prev);
        trk[0].p[P_E7] = lv_prev;
        fa_factory_extras(&trk[0], (uint32_t)lv_prev);
    }
    if (lv_dirty)
        lv_cfg_save();
    ui.force = 1;
}

/* HOME held LV_HOLD_MS: 1, once. The press itself is HOME's tap (fa_buttons); only the hold is this */
static int lv_home_hold(uint32_t pressed)
{
    if ((pressed >> panel.btn[B_HOME]) & 1u) {
        lv_hd = 1;
        lv_hd_t0 = fm1_ms;
        return 0;
    }
    if (!lv_hd)
        return 0;
    if (!((fm1_in.buttons >> panel.btn[B_HOME]) & 1u)) {
        lv_hd = 0;
        return 0;
    }
    if ((uint32_t)(fm1_ms - lv_hd_t0) >= LV_HOLD_MS) {
        lv_hd = 0;
        return 1;
    }
    return 0;
}

/* the main loop's share, every pass of fa_input: the slot a note asked for, the slot the list stayed on, the settings */
static void lv_poll(void)
{
    if (!fm_live) {
        lv_req = 0;
        return;
    }
    if (lv_req) {
        uint32_t s = (uint32_t)lv_req - 1u;
        if (s < LV_N)
            lv_apply(s);
        lv_req = 0;                                       /* the ISR goes on with the queue, now on the loaded slot */
    }
    if (lv_want < LV_N && (uint32_t)(fm1_ms - lv_want_ms) >= LV_LOAD_MS) {
        uint32_t s = lv_want;
        lv_want = 0xFF;
        if (lv_cur != s)
            lv_apply(s);
    }
    if (lv_dirty && (uint32_t)(fm1_ms - lv_dirty_ms) >= LV_SAVE_MS)
        lv_cfg_save();
}

/* a slot changed: the table, the settings later, and the slot loads again once the list rests */
static void lv_touch(void)
{
    lv_dirty = 1;
    lv_dirty_ms = fm1_ms;
    lv_map_build();
    lv_cur = 0xFF;
    lv_want = lv_row;
    lv_want_ms = fm1_ms;
    ui.force = 1;
}

/* a turn of the 200 sounds: one a detent when slow (fmg_acc's first step is two), more when fast */
static int32_t lv_acc(uint32_t role, int32_t s)
{
    int32_t a = fmg_acc(role, s, 200);
    return a == 2 * s ? s : a;
}

/* a volume changed: heard now on the slot that is loaded (no reload: the notes go on), kept in the settings later */
static void lv_volume(uint32_t b, int32_t d)
{
    int8_t *v = b ? &lv_tb[lv_row] : &lv_ta[lv_row];
    int32_t n = clamp((int32_t)*v + d, LV_TMIN, LV_TMAX);
    if (n == *v)
        return;
    *v = (int8_t)n;
    if (lv_cur == lv_row) {
        trk[b].p[P_LEVEL] = lv_level(lv_base[b], n);
    }
    lv_dirty = 1;
    lv_dirty_ms = fm1_ms;
    ui.force = 1;
}

static void lv_input(uint32_t pressed)
{
    int32_t s;
    if (lv_home_hold(pressed)) {
        lv_exit();
        return;
    }
    if ((s = panel_enc(EN_SELECT)) != 0) {
        lv_row = (uint8_t)((lv_row + LV_N + (s > 0 ? 1u : LV_N - 1u)) % LV_N);
        lv_want = lv_row;
        lv_want_ms = fm1_ms;
        ui.force = 1;
    }
    if ((s = panel_enc(EN_K1)) != 0) {                    /* CHANNEL: 1..16, then OFF */
        int32_t c = clamp((int32_t)lv_ch[lv_row] + (s > 0 ? 1 : -1), 0, (int32_t)LV_OFF);
        if (c != lv_ch[lv_row]) {
            lv_ch[lv_row] = (uint8_t)c;
            lv_touch();
        }
    }
    {   /* SOUND: K2 and the PRESETS knob, round 000..199 */
        int32_t d = 0, v;
        if ((s = panel_enc(EN_K2)) != 0)
            d += lv_acc(EN_K2, s);
        if ((s = panel_enc(EN_PRESET)) != 0)
            d += s > 0 ? 1 : -1;
        if (d) {
            v = ((int32_t)lv_a[lv_row] + d) % (int32_t)FMINI_NSLOT;
            lv_a[lv_row] = (uint8_t)(v < 0 ? v + (int32_t)FMINI_NSLOT : v);
            lv_touch();
        }
    }
    if ((s = panel_enc(EN_K3)) != 0) {                    /* LAYER: NONE, 000..199 (left of 000 is NONE) */
        int32_t v = (lv_b[lv_row] == LV_NOB ? -1 : (int32_t)lv_b[lv_row]) + lv_acc(EN_K3, s);
        v = clamp(v, -1, (int32_t)FMINI_NSLOT - 1);
        if ((uint8_t)(v < 0 ? LV_NOB : v) != lv_b[lv_row]) {
            lv_b[lv_row] = (uint8_t)(v < 0 ? LV_NOB : v);
            lv_touch();
        }
    }
    if ((s = panel_enc(EN_K4)) != 0)                      /* VOLUME of A: 1 dB a detent */
        lv_volume(0, s);
    if ((s = panel_enc(EN_ALGO)) != 0 && lv_b[lv_row] != LV_NOB)   /* VOLUME of B (a slot without a layer has none) */
        lv_volume(1, s);
    if ((pressed >> panel.btn[B_OCTDN]) & 1u)
        song.octave = song.octave > -3 ? song.octave - 1 : song.octave;
    if ((pressed >> panel.btn[B_OCTUP]) & 1u)
        song.octave = song.octave < 3 ? song.octave + 1 : song.octave;
}

/* ------------------------------------------------------------ screen --- */
/* "042 NAME", the name cut to fit w pixels (a layer's or a sound's: preset k) */
static void lv_label(char *d, uint32_t k, int32_t w)
{
    uint32_t n;
    fa_slot_num(d, k);
    d[3] = ' ';
    str_cpy(d + 4, fmini_pre[k % FMINI_NSLOT].name, 13);
    for (n = str_len(d); n > 4u && text_w(&AF_S, d) > w; n--)
        d[n - 1u] = 0;
}
static void lv_ch_str(char *b, uint32_t c)
{
    if (c >= 16u) {
        str_cpy(b, "OFF", 4);
        return;
    }
    b[0] = 'C';
    b[1] = 'H';
    b[2] = (char)('0' + (c + 1u) / 10u);
    b[3] = (char)('0' + (c + 1u) % 10u);
    b[4] = 0;
}

/* a volume: "+3 DB", "0 DB", "-12 DB" */
static void lv_db_str(char *b, int32_t v)
{
    uint32_t n = 0, m = (uint32_t)(v < 0 ? -v : v);
    if (v)
        b[n++] = v < 0 ? '-' : '+';
    if (m >= 10u)
        b[n++] = (char)('0' + m / 10u);
    b[n++] = (char)('0' + m % 10u);
    str_cpy(b + n, " DB", 4);
}

static void lv_scene(void)
{
    const uint16_t bgd = FA_RGB(0x1B, 0x19, 0x17), hot = FA_RGB(0x2E, 0x28, 0x22), dark = FA_RGB(0x0E, 0x0D, 0x0C);
    uint32_t i, first = (uint32_t)(lv_row / 8u) * 8u;
    char b[24];
    cv_rect(8, 0, 224, 22, dark);                         /* the header: the logo, the LIVE tab */
    {
        int32_t lx = 12;
        cv_text_on(lx, 3, &AF_S, "FM", FA_OR, dark);
        lx += text_w(&AF_S, "FM");
        cv_text_on(lx, 3, &AF_S, "ini-", FA_CR, dark);
        lx += text_w(&AF_S, "ini-");
        cv_text_on(lx, 3, &AF_S, "1", FA_OR, dark);
    }
    cv_rect(188, 2, 38, 18, FA_OR);
    cv_text_on(188 + 19 - text_w(&AF_X, "LIVE") / 2, 11 - AF_X_CAP_Y - AF_X_CAP_H / 2 + 1, &AF_X, "LIVE", FA_RGB(0x1A, 0x0F, 0x06), FA_OR);
    cv_rect(12, 26, 216, 146, bgd);                       /* the list: eight slots, the page of the one the list is on */
    for (i = 0; i < 8u; i++) {
        uint32_t s = first + i;
        int32_t ry = 28 + (int32_t)i * 18, tx = ry + 5;
        int on = s == lv_row, off = lv_ch[s] >= 16u;
        uint16_t bg = on ? hot : bgd, fg = on ? FA_OR : off ? FA_MU : FA_CR;
        if (on)
            cv_rect(14, ry, 212, 17, hot);
        if (s == lv_cur)                                  /* the slot loaded now */
            cv_rect(16, ry + 6, 4, 5, FA_OR);
        b[0] = (char)('0' + (s + 1u) / 10u);
        b[1] = (char)('0' + (s + 1u) % 10u);
        b[2] = 0;
        fa_pl_text(24, tx, &AF_S, b, fg, bg);
        lv_ch_str(b, lv_ch[s]);
        fa_pl_text(48, tx, &AF_S, b, fg, bg);
        lv_label(b, lv_a[s], lv_b[s] == LV_NOB ? 118 : 96);
        fa_pl_text(88, tx, &AF_S, b, fg, bg);
        if (lv_b[s] != LV_NOB) {                          /* the layer: + and its number */
            char l[5] = {'+', 0, 0, 0, 0};
            fa_slot_num(l + 1, lv_b[s]);
            cv_text_r(222, ry + 5 - fa_cap_y(&AF_S), &AF_S, l, fg, bg);
        }
    }
    cv_rect(12, 174, 216, 30, bgd);                       /* the slot the list is on, in full: A and B, each with its volume */
    for (i = 0; i < 2u; i++) {
        int32_t y = 177 + (int32_t)i * 13;
        int none = i && lv_b[lv_row] == LV_NOB;
        cv_text_on(20, y - fa_cap_y(&AF_S), &AF_S, i ? "B" : "A", FA_MU, bgd);
        if (none) {
            fa_pl_text(34, y, &AF_S, "--- NO LAYER", FA_MU, bgd);
            continue;
        }
        lv_label(b, i ? lv_b[lv_row] : lv_a[lv_row], 130);
        fa_pl_text(34, y, &AF_S, b, FA_OR, bgd);
        lv_db_str(b, i ? lv_tb[lv_row] : lv_ta[lv_row]);
        cv_text_r(222, y - fa_cap_y(&AF_S), &AF_S, b, FA_CR, bgd);
    }
    fa_ctext(120, 208, &AF_X, "SELECT SLOT  K1 CH  K2 SOUND  K3 LAYER", FA_MU);
    fa_ctext(120, 218, &AF_X, "K4 VOLUME A  ALGORITHM VOLUME B", FA_MU);
    fa_ltext(14, 228, &AF_X, "HOLD HOME = EXIT", FA_MU);
    fa_battery(226, 227);
}

#endif /* FELUCCA_FMINI_ONLY */
#endif /* FMINI_LIVE_C */
