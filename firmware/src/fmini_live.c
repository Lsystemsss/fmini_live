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
 * Who does what: the audio ISR (seq.c events_block, fl_route) only looks up the slot a channel plays (fl_map) and, for a note
 * on a slot that is not loaded, asks for it (fl_req) and waits with that message at the head of the queue; the main loop
 * (fl_poll, from fa_input) loads the slot, since a sound load reads the flash. The slots are kept in the settings
 * record, favorites.factory rows 11 and 13 (the favourites of engines no screen of this firmware shows), behind a marker and
 * a checksum: anything else in those bytes reads as the defaults (slot n: channel n, preset n, no layer).
 * Included by fmini_app.c before fa_scene. */

#define FL_N 16u                                          /* the slots */
#define FL_OFF 16u                                        /* a slot's channel: OFF, no MIDI channel plays it */
#define FL_NOB 0xFFu                                      /* a slot without a layer */
#define FL_HOLD_MS 500u                                   /* HOME held this long: LIVE on / off */
#define FL_LOAD_MS 120u                                   /* the list stays on a slot this long: it loads (the keys play it) */
#define FL_SAVE_MS 2000u                                  /* an edit is written to the settings this long after the last one */
#define FL_MAGIC 0xF1u
#define FL_VERSION 1u
#define FL_BYTES 51u                                      /* marker, version, 16 x (channel, sound, layer), checksum */

static uint8_t fl_ch[FL_N];                               /* the slot's MIDI channel 0..15, FL_OFF */
static uint8_t fl_a[FL_N];                                /* its sound: a preset 0..FMINI_NSLOT - 1 */
static uint8_t fl_b[FL_N];                                /* its layer: a preset, FL_NOB none */
static uint8_t fl_row;                                    /* the slot the list is on */
static uint8_t fl_want = 0xFF;                            /* the slot to load when the list has stayed on it (0xFF none) */
static uint32_t fl_want_ms;
static uint8_t fl_dirty;                                  /* the slots changed, not yet written to the settings */
static uint32_t fl_dirty_ms;
static int16_t fl_prev = -1;                              /* the preset track 1 had when LIVE opened: it comes back on exit */
static uint8_t fl_hd;                                     /* HOME is down and may be held */
static uint32_t fl_hd_t0;

/* ------------------------------------------------------------ slots --- */
static void fl_defaults(void)
{
    uint32_t s;
    for (s = 0; s < FL_N; s++) {
        fl_ch[s] = (uint8_t)s;                            /* slot n plays on channel n + 1 */
        fl_a[s] = (uint8_t)s;                             /* with preset n */
        fl_b[s] = FL_NOB;
    }
}

/* the channel -> slot table the ISR reads: the first slot set to each channel, + 1 */
static void fl_map_build(void)
{
    uint8_t m[16];
    uint32_t s;
    for (s = 0; s < 16u; s++)
        m[s] = 0;
    for (s = 0; s < FL_N; s++)
        if (fl_ch[s] < 16u && !m[fl_ch[s]])
            m[fl_ch[s]] = (uint8_t)(s + 1u);
    fm1_irq_off();                                        /* (the ISR never sees half a table) */
    for (s = 0; s < 16u; s++)
        fl_map[s] = m[s];
    fm1_irq_on();
}

/* ---------------------------------------------------------- settings --- */
static uint8_t *fl_cfg_byte(uint32_t i) { return i < 32u ? &favorites.factory[11][i] : &favorites.factory[13][i - 32u]; }
static uint8_t fl_cfg_sum(void)
{
    uint32_t i, h = 0xA5u;
    for (i = 2; i < FL_BYTES - 1u; i++)
        h = (h * 31u + *fl_cfg_byte(i)) & 0xFFu;
    return (uint8_t)h;
}
static void fl_cfg_load(void)
{
    uint32_t s, ok = *fl_cfg_byte(0) == FL_MAGIC && *fl_cfg_byte(1) == FL_VERSION && *fl_cfg_byte(FL_BYTES - 1u) == fl_cfg_sum();
    for (s = 0; ok && s < FL_N; s++) {                    /* a value out of range: the whole record is not ours */
        uint32_t c = *fl_cfg_byte(2u + 3u * s), a = *fl_cfg_byte(3u + 3u * s), b = *fl_cfg_byte(4u + 3u * s);
        if (c > FL_OFF || a >= FMINI_NSLOT || (b != FL_NOB && b >= FMINI_NSLOT))
            ok = 0;
    }
    if (!ok) {
        fl_defaults();
    } else {
        for (s = 0; s < FL_N; s++) {
            fl_ch[s] = *fl_cfg_byte(2u + 3u * s);
            fl_a[s] = *fl_cfg_byte(3u + 3u * s);
            fl_b[s] = *fl_cfg_byte(4u + 3u * s);
        }
    }
    fl_map_build();
}
/* the slots into the settings' bytes; the flash copy is settings_save's (deferred while anything plays) */
static void fl_cfg_save(void)
{
    uint32_t s;
    fl_dirty = 0;
    *fl_cfg_byte(0) = FL_MAGIC;
    *fl_cfg_byte(1) = FL_VERSION;
    for (s = 0; s < FL_N; s++) {
        *fl_cfg_byte(2u + 3u * s) = fl_ch[s];
        *fl_cfg_byte(3u + 3u * s) = fl_a[s];
        *fl_cfg_byte(4u + 3u * s) = fl_b[s];
    }
    *fl_cfg_byte(FL_BYTES - 1u) = fl_cfg_sum();
    settings_save();
}

/* ------------------------------------------------------------- sound --- */
/* slot s into the tracks: its sound into track 1, its layer into track 2 (unmuted), or track 2 muted without one. What
 * sounds on the two tracks lets go first (panic_req); the layer is off while track 2 changes (the ISR) */
static void fl_apply(uint32_t s)
{
    track_t *t = &trk[0], *u = &trk[1];
    uint32_t a = fl_a[s], b = fl_b[s];
    fl_layer = 0;
    panic_req |= 3u;
    t->user = 0;
    apply_preset_to(t, a);
    t->p[P_E7] = (int16_t)a;
    fa_factory_extras(t, a);                              /* its arpeggiator, LFO, FX settings, octave */
    if (b != FL_NOB) {
        apply_preset_to(u, b);
        u->p[P_E7] = (int16_t)b;
        fa_factory_extras(u, b);
        u->p[P_AMODE] = 0;                                /* track 1's arpeggiator feeds track 2 through trk_note_on */
        u->p[P_MUTE] = 0;
        u->p[P_GLIDE] = (int16_t)((fmini_patch[1][FG_SW2] & FT_GLIDE) ? fmini_patch[1][FG_GLID] : 0u);   /* (as fmg_glide_sync does for track 1) */
        fl_layer = 1;
    } else {
        u->p[P_MUTE] = 1;
    }
    fl_cur = (uint8_t)s;
    ui.force = 1;
}

static void fl_enter(void)
{
    fl_prev = trk[0].p[P_E7] >= 0 && trk[0].p[P_E7] < (int32_t)FMINI_NSLOT ? trk[0].p[P_E7] : (int16_t)-1;
    fa_sv = fa_ar = fa_sc = fa_sq = fa_sq_rec = 0;        /* the pop-ups close */
    fa_tp = 0;
    fa_pset_pop = 0;
    fmg_fx = 0;
    fa_gl_close();
    fl_map_build();
    fl_cur = 0xFF;
    fl_req = 0;
    fl_hd = 0;
    fl_want = fl_row;
    fl_want_ms = fm1_ms - FL_LOAD_MS;                     /* the slot the list was on loads now */
    fm_live = 1;
    ui.force = 1;
}

static void fl_exit(void)
{
    fm_live = 0;
    fl_layer = 0;
    fl_req = 0;
    fl_cur = 0xFF;
    fl_want = 0xFF;
    fl_hd = 0;
    panic_req |= 3u;
    trk[1].p[P_MUTE] = 1;
    if (fl_prev >= 0) {                                   /* the sound that was there (a patch of the player's own cannot come back) */
        apply_preset_to(&trk[0], (uint32_t)fl_prev);
        trk[0].p[P_E7] = fl_prev;
        fa_factory_extras(&trk[0], (uint32_t)fl_prev);
    }
    if (fl_dirty)
        fl_cfg_save();
    ui.force = 1;
}

/* HOME held FL_HOLD_MS: 1, once. The press itself is HOME's tap (fa_buttons); only the hold is this */
static int fl_home_hold(uint32_t pressed)
{
    if ((pressed >> panel.btn[B_HOME]) & 1u) {
        fl_hd = 1;
        fl_hd_t0 = fm1_ms;
        return 0;
    }
    if (!fl_hd)
        return 0;
    if (!((fm1_in.buttons >> panel.btn[B_HOME]) & 1u)) {
        fl_hd = 0;
        return 0;
    }
    if ((uint32_t)(fm1_ms - fl_hd_t0) >= FL_HOLD_MS) {
        fl_hd = 0;
        return 1;
    }
    return 0;
}

/* the main loop's share, every pass of fa_input: the slot a note asked for, the slot the list stayed on, the settings */
static void fl_poll(void)
{
    if (!fm_live) {
        fl_req = 0;
        return;
    }
    if (fl_req) {
        uint32_t s = (uint32_t)fl_req - 1u;
        if (s < FL_N)
            fl_apply(s);
        fl_req = 0;                                       /* the ISR goes on with the queue, now on the loaded slot */
    }
    if (fl_want < FL_N && (uint32_t)(fm1_ms - fl_want_ms) >= FL_LOAD_MS) {
        uint32_t s = fl_want;
        fl_want = 0xFF;
        if (fl_cur != s)
            fl_apply(s);
    }
    if (fl_dirty && (uint32_t)(fm1_ms - fl_dirty_ms) >= FL_SAVE_MS)
        fl_cfg_save();
}

/* a slot changed: the table, the settings later, and the slot loads again once the list rests */
static void fl_touch(void)
{
    fl_dirty = 1;
    fl_dirty_ms = fm1_ms;
    fl_map_build();
    fl_cur = 0xFF;
    fl_want = fl_row;
    fl_want_ms = fm1_ms;
    ui.force = 1;
}

static void fl_input(uint32_t pressed)
{
    int32_t s;
    if (fl_home_hold(pressed)) {
        fl_exit();
        return;
    }
    if ((s = panel_enc(EN_SELECT)) != 0) {
        fl_row = (uint8_t)((fl_row + FL_N + (s > 0 ? 1u : FL_N - 1u)) % FL_N);
        fl_want = fl_row;
        fl_want_ms = fm1_ms;
        ui.force = 1;
    }
    if ((s = panel_enc(EN_K1)) != 0) {                    /* CHANNEL: 1..16, then OFF */
        int32_t c = clamp((int32_t)fl_ch[fl_row] + (s > 0 ? 1 : -1), 0, (int32_t)FL_OFF);
        if (c != fl_ch[fl_row]) {
            fl_ch[fl_row] = (uint8_t)c;
            fl_touch();
        }
    }
    {   /* SOUND: K2 and the PRESETS knob, round 000..199 */
        int32_t d = 0, v;
        if ((s = panel_enc(EN_K2)) != 0)
            d += fmg_acc(EN_K2, s, 200);
        if ((s = panel_enc(EN_PRESET)) != 0)
            d += s > 0 ? 1 : -1;
        if (d) {
            v = ((int32_t)fl_a[fl_row] + d) % (int32_t)FMINI_NSLOT;
            fl_a[fl_row] = (uint8_t)(v < 0 ? v + (int32_t)FMINI_NSLOT : v);
            fl_touch();
        }
    }
    if ((s = panel_enc(EN_K3)) != 0) {                    /* LAYER: NONE, 000..199 (left of 000 is NONE) */
        int32_t v = (fl_b[fl_row] == FL_NOB ? -1 : (int32_t)fl_b[fl_row]) + fmg_acc(EN_K3, s, 200);
        v = clamp(v, -1, (int32_t)FMINI_NSLOT - 1);
        if ((uint8_t)(v < 0 ? FL_NOB : v) != fl_b[fl_row]) {
            fl_b[fl_row] = (uint8_t)(v < 0 ? FL_NOB : v);
            fl_touch();
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
static void fl_label(char *d, uint32_t k, int32_t w)
{
    uint32_t n;
    fa_slot_num(d, k);
    d[3] = ' ';
    str_cpy(d + 4, fmini_pre[k % FMINI_NSLOT].name, 13);
    for (n = str_len(d); n > 4u && text_w(&AF_S, d) > w; n--)
        d[n - 1u] = 0;
}
static void fl_ch_str(char *b, uint32_t c)
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

static void fl_scene(void)
{
    const uint16_t bgd = FA_RGB(0x1B, 0x19, 0x17), hot = FA_RGB(0x2E, 0x28, 0x22), dark = FA_RGB(0x0E, 0x0D, 0x0C);
    uint32_t i, first = (uint32_t)(fl_row / 8u) * 8u;
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
        int on = s == fl_row, off = fl_ch[s] >= 16u;
        uint16_t bg = on ? hot : bgd, fg = on ? FA_OR : off ? FA_MU : FA_CR;
        if (on)
            cv_rect(14, ry, 212, 17, hot);
        if (s == fl_cur)                                  /* the slot loaded now */
            cv_rect(16, ry + 6, 4, 5, FA_OR);
        b[0] = (char)('0' + (s + 1u) / 10u);
        b[1] = (char)('0' + (s + 1u) % 10u);
        b[2] = 0;
        fa_pl_text(24, tx, &AF_S, b, fg, bg);
        fl_ch_str(b, fl_ch[s]);
        fa_pl_text(48, tx, &AF_S, b, fg, bg);
        fl_label(b, fl_a[s], fl_b[s] == FL_NOB ? 118 : 96);
        fa_pl_text(88, tx, &AF_S, b, fg, bg);
        if (fl_b[s] != FL_NOB) {                          /* the layer: + and its number */
            char l[5] = {'+', 0, 0, 0, 0};
            fa_slot_num(l + 1, fl_b[s]);
            cv_text_r(222, ry + 5 - fa_cap_y(&AF_S), &AF_S, l, fg, bg);
        }
    }
    cv_rect(12, 176, 216, 32, bgd);                       /* the slot the list is on, in full */
    fl_label(b, fl_a[fl_row], 190);
    cv_text_on(20, 182 - fa_cap_y(&AF_S), &AF_S, "A", FA_MU, bgd);
    fa_pl_text(34, 182, &AF_S, b, FA_OR, bgd);
    if (fl_b[fl_row] == FL_NOB) {
        str_cpy(b, "--- NO LAYER", 24);
        cv_text_on(20, 196 - fa_cap_y(&AF_S), &AF_S, "B", FA_MU, bgd);
        fa_pl_text(34, 196, &AF_S, b, FA_MU, bgd);
    } else {
        fl_label(b, fl_b[fl_row], 190);
        cv_text_on(20, 196 - fa_cap_y(&AF_S), &AF_S, "B", FA_MU, bgd);
        fa_pl_text(34, 196, &AF_S, b, FA_OR, bgd);
    }
    fa_ctext(120, 213, &AF_X, "SELECT=SLOT  K1=CH  K2=SOUND  K3=LAYER", FA_MU);
    fa_ltext(14, 225, &AF_X, "HOLD HOME = EXIT", FA_MU);
    fa_battery(226, 227);
}

#endif /* FELUCCA_FMINI_ONLY */
#endif /* FMINI_LIVE_C */
