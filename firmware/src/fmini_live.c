/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef FMINI_LIVE_C
#define FMINI_LIVE_C
#if FELUCCA_FMINI_ONLY
/* FMini-1 LIVE mode: a library of 16 slots. Each slot has a MIDI channel (1..16, or OFF), a sound (a preset 000..199) and,
 * if you want, a second sound, the layer, that plays together with the first.
 *
 * HOME held 0.5 s opens it (and closes it: the tap on HOME still goes to the first page as before). In LIVE the screen is the
 * list; SELECT picks a slot, K1 its CHANNEL, K2 (or PRESETS) its SOUND, K3 its LAYER (a turn left of 000 is NONE). The slot
 * the list is on is loaded after 0.12 s, so the keys play it while you edit. A note that comes on a slot's channel loads that
 * slot first: its sound goes into track 1 and its layer into track 2, and the keys, MIDI, the arpeggiator and the sequencer
 * of track 1 play track 2 too (voice.c trk_note_on). One slot is loaded at a time: a note on another slot's channel replaces
 * it. The notes, pitch bend and CCs of a channel no slot is set to are dropped, and so is Program Change (the slot is the
 * sound). CCs and the mod wheel move the first sound only.
 *
 * Who does what: the audio ISR (seq.c events_block, lv_route) only looks up the slot a channel plays (lv_map) and, for a note
 * on a slot that is not loaded, asks for it (lv_req) and waits with that message at the head of the queue; the main loop
 * (lv_poll, from fa_input) loads the slot, since a sound load reads the flash. The slots are kept in the settings
 * record, favorites.factory rows 11 and 13 (the favourites of engines no screen of this firmware shows), behind a marker and
 * a checksum: anything else in those bytes reads as the defaults (slot n: channel n, preset n, no layer).
 * Included by fmini_app.c before fa_scene. */

#define LV_N 16u                                          /* the slots */
#define LV_OFF 16u                                        /* a slot's channel: OFF, no MIDI channel plays it */
#define LV_NOB 0xFFu                                      /* a slot without a layer */
#define LV_HOLD_MS 500u                                   /* HOME held this long: LIVE on / off */
#define LV_LOAD_MS 120u                                   /* the list stays on a slot this long: it loads (the keys play it) */
#define LV_SAVE_MS 2000u                                  /* an edit is written to the settings this long after the last one */
#define LV_MAGIC 0xF1u
#define LV_VERSION 1u
#define LV_BYTES 51u                                      /* marker, version, 16 x (channel, sound, layer), checksum */

static uint8_t lv_ch[LV_N];                               /* the slot's MIDI channel 0..15, LV_OFF */
static uint8_t lv_a[LV_N];                                /* its sound: a preset 0..FMINI_NSLOT - 1 */
static uint8_t lv_b[LV_N];                                /* its layer: a preset, LV_NOB none */
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
static uint8_t *lv_cfg_byte(uint32_t i) { return i < 32u ? &favorites.factory[11][i] : &favorites.factory[13][i - 32u]; }
static uint8_t lv_cfg_sum(void)
{
    uint32_t i, h = 0xA5u;
    for (i = 2; i < LV_BYTES - 1u; i++)
        h = (h * 31u + *lv_cfg_byte(i)) & 0xFFu;
    return (uint8_t)h;
}
static void lv_cfg_load(void)
{
    uint32_t s, ok = *lv_cfg_byte(0) == LV_MAGIC && *lv_cfg_byte(1) == LV_VERSION && *lv_cfg_byte(LV_BYTES - 1u) == lv_cfg_sum();
    for (s = 0; ok && s < LV_N; s++) {                    /* a value out of range: the whole record is not ours */
        uint32_t c = *lv_cfg_byte(2u + 3u * s), a = *lv_cfg_byte(3u + 3u * s), b = *lv_cfg_byte(4u + 3u * s);
        if (c > LV_OFF || a >= FMINI_NSLOT || (b != LV_NOB && b >= FMINI_NSLOT))
            ok = 0;
    }
    if (!ok) {
        lv_defaults();
    } else {
        for (s = 0; s < LV_N; s++) {
            lv_ch[s] = *lv_cfg_byte(2u + 3u * s);
            lv_a[s] = *lv_cfg_byte(3u + 3u * s);
            lv_b[s] = *lv_cfg_byte(4u + 3u * s);
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
    *lv_cfg_byte(1) = LV_VERSION;
    for (s = 0; s < LV_N; s++) {
        *lv_cfg_byte(2u + 3u * s) = lv_ch[s];
        *lv_cfg_byte(3u + 3u * s) = lv_a[s];
        *lv_cfg_byte(4u + 3u * s) = lv_b[s];
    }
    *lv_cfg_byte(LV_BYTES - 1u) = lv_cfg_sum();
    settings_save();
}

/* ------------------------------------------------------------- sound --- */
/* slot s into the tracks: its sound into track 1, its layer into track 2 (unmuted), or track 2 muted without one. What
 * sounds on the two tracks lets go first (panic_req); the layer is off while track 2 changes (the ISR) */
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
    if (b != LV_NOB) {
        apply_preset_to(u, b);
        u->p[P_E7] = (int16_t)b;
        fa_factory_extras(u, b);
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
            d += fmg_acc(EN_K2, s, 200);
        if ((s = panel_enc(EN_PRESET)) != 0)
            d += s > 0 ? 1 : -1;
        if (d) {
            v = ((int32_t)lv_a[lv_row] + d) % (int32_t)FMINI_NSLOT;
            lv_a[lv_row] = (uint8_t)(v < 0 ? v + (int32_t)FMINI_NSLOT : v);
            lv_touch();
        }
    }
    if ((s = panel_enc(EN_K3)) != 0) {                    /* LAYER: NONE, 000..199 (left of 000 is NONE) */
        int32_t v = (lv_b[lv_row] == LV_NOB ? -1 : (int32_t)lv_b[lv_row]) + fmg_acc(EN_K3, s, 200);
        v = clamp(v, -1, (int32_t)FMINI_NSLOT - 1);
        if ((uint8_t)(v < 0 ? LV_NOB : v) != lv_b[lv_row]) {
            lv_b[lv_row] = (uint8_t)(v < 0 ? LV_NOB : v);
            lv_touch();
        }
    }
    (void)panel_enc(EN_K4);
    (void)panel_enc(EN_ALGO);
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
    cv_rect(12, 176, 216, 32, bgd);                       /* the slot the list is on, in full */
    lv_label(b, lv_a[lv_row], 190);
    cv_text_on(20, 182 - fa_cap_y(&AF_S), &AF_S, "A", FA_MU, bgd);
    fa_pl_text(34, 182, &AF_S, b, FA_OR, bgd);
    if (lv_b[lv_row] == LV_NOB) {
        str_cpy(b, "--- NO LAYER", 24);
        cv_text_on(20, 196 - fa_cap_y(&AF_S), &AF_S, "B", FA_MU, bgd);
        fa_pl_text(34, 196, &AF_S, b, FA_MU, bgd);
    } else {
        lv_label(b, lv_b[lv_row], 190);
        cv_text_on(20, 196 - fa_cap_y(&AF_S), &AF_S, "B", FA_MU, bgd);
        fa_pl_text(34, 196, &AF_S, b, FA_OR, bgd);
    }
    fa_ctext(120, 213, &AF_X, "SELECT=SLOT  K1=CH  K2=SOUND  K3=LAYER", FA_MU);
    fa_ltext(14, 225, &AF_X, "HOLD HOME = EXIT", FA_MU);
    fa_battery(226, 227);
}

#endif /* FELUCCA_FMINI_ONLY */
#endif /* FMINI_LIVE_C */
