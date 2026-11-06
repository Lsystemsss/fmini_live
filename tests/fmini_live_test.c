/* SPDX-License-Identifier: GPL-3.0-only
 * FMini-1 LIVE mode (fmini_live.c): HOME held opens / closes it, a note on a slot's channel loads the slot (its sound into
 * track 1, its layer into track 2) before it plays, other channels and Program Change are dropped, the 16 slots survive a
 * settings round trip and a bad record reads as the defaults. */
#define UI_TEST_NO_MAIN 1
#define FELUCCA_FMINI_ONLY 1
#include "ui_test.c"
#include "../firmware/src/fmini_rec.c"
#include "../firmware/src/fmini_bank.c"
#include "../firmware/src/fmini_app.c"
#include <stdio.h>
static void fframe(void) { host_ticks += 16000u; fm1_ms += 16u; fa_input(); }
static void fframes(uint32_t ms) { uint32_t i; for (i = 0; i < ms / 16u; i++) fframe(); }
static void fpress(uint32_t label)
{
    fm1_in.buttons |= 1u << panel.btn[label];
    host_pressed |= 1u << panel.btn[label];
    fframe();
    fm1_in.buttons &= ~(1u << panel.btn[label]);
    fframe();
}
static void fhold(uint32_t label)
{
    uint32_t k;
    fm1_in.buttons |= 1u << panel.btn[label];
    host_pressed |= 1u << panel.btn[label];
    for (k = 0; k < 52u; k++)
        fframe();
    fm1_in.buttons &= ~(1u << panel.btn[label]);
    fframe();
}
static void fturn(uint32_t role, int32_t d)
{
    host_enc[panel.enc[role]] += d * panel.dir[role];
    fframe();
    host_ticks += 200000u;
}
static uint32_t lv_gates(track_t *t)
{
    uint32_t i, n = 0;
    for (i = 0; i < NVOICE; i++)
        n += t->v[i].gate != 0;
    return n;
}
/* a MIDI message in, as the audio ISR gets it (USB), and one block of events */
static void note(uint32_t ch, uint32_t n, uint32_t vel)
{
    midi_enqueue(0x9u | (0x90u | ch) << 8 | n << 16 | vel << 24, 1u);
    events_block(CTL);
}
static void noteoff(uint32_t ch, uint32_t n)
{
    midi_enqueue(0x8u | (0x80u | ch) << 8 | n << 16, 1u);
    events_block(CTL);
}
#define FAIL(...) do { printf(__VA_ARGS__); printf("\n"); return 1; } while (0)

int main(int argc, char **argv)
{
    uint32_t i;
    const char *dir = argc > 1 ? argv[1] : ".";
    { uint32_t gi; for (gi = 0; gi < G_COUNT; gi++) song.g[gi] = GP[gi].def; }
    fb_names_init();
    panel_init();                                      /* the panel's encoder and button map */
    fa_init();
    fm_midi_src = 3;                                   /* every input, every channel (LIVE overrides the channel) */
    fm_midi_ch = 0;
    if (fm_live) FAIL("live at power-on");

    /* defaults: slot n is channel n + 1, preset n, no layer */
    for (i = 0; i < FL_N; i++)
        if (fl_ch[i] != i || fl_a[i] != i || fl_b[i] != FL_NOB) FAIL("default slot %u", i);

    /* HOME: a tap is the first page as before, a hold is LIVE */
    fpress(B_HOME);
    if (fm_live) FAIL("a tap on HOME opened LIVE");
    fhold(B_HOME);
    if (!fm_live) FAIL("HOME held did not open LIVE");
    fframes(300);
    if (fl_cur != fl_row) FAIL("the slot the list is on was not loaded (cur %u row %u)", fl_cur, fl_row);
    ui.force = 1; fa_draw();                           /* the list draws (no crash, the panel is not the pages') */

    /* the list: SELECT moves the row, K1 the channel, K2 the sound, K3 the layer; the row loads after resting */
    fl_ch[0] = 0; fl_a[0] = 5;  fl_b[0] = FL_NOB;       /* slot 1: channel 1, preset 5 */
    fl_ch[1] = 1; fl_a[1] = 10; fl_b[1] = 20;           /* slot 2: channel 2, preset 10 + layer 20 */
    fl_ch[2] = FL_OFF; fl_a[2] = 30; fl_b[2] = FL_NOB;  /* slot 3: OFF */
    fl_ch[3] = 0; fl_a[3] = 40; fl_b[3] = FL_NOB;       /* slot 4: channel 1 as well: the first slot (1) wins */
    fl_map_build();
    if (fl_map[0] != 1 || fl_map[1] != 2 || fl_map[2] != 0) FAIL("map %u %u %u", fl_map[0], fl_map[1], fl_map[2]);
    fl_row = 0; fl_cur = 0xFF; fl_want = 0xFF;
    fturn(EN_SELECT, 1);
    if (fl_row != 1) FAIL("SELECT: row %u", fl_row);
    fturn(EN_K1, 1);
    if (fl_ch[1] != 2) FAIL("K1: channel %u", fl_ch[1]);
    fturn(EN_K1, -1);
    {   uint8_t b0 = fl_b[1];
        fturn(EN_K3, 1);
        if (fl_b[1] == b0) FAIL("K3 does not move the layer");
    }
    fl_b[1] = 20;
    fl_touch();
    fframes(300);
    if (fl_cur != 1 || !fl_layer) FAIL("slot 2 not loaded by resting on it (cur %u layer %u)", fl_cur, fl_layer);
    if (trk[0].p[P_E7] != 10 || trk[1].p[P_E7] != 20) FAIL("sounds: %d / %d", trk[0].p[P_E7], trk[1].p[P_E7]);
    if (trk[1].p[P_MUTE]) FAIL("the layer track is muted");

    /* a note on the loaded slot's channel plays both sounds, a release lets both go */
    note(1, 60, 100);
    if (!lv_gates(&trk[0]) || !lv_gates(&trk[1])) FAIL("layer: gates %u / %u", lv_gates(&trk[0]), lv_gates(&trk[1]));
    noteoff(1, 60);
    if (lv_gates(&trk[0]) || lv_gates(&trk[1])) FAIL("layer: still gated after the release %u / %u", lv_gates(&trk[0]), lv_gates(&trk[1]));

    /* a note on another slot's channel: the message waits, the slot loads, then the note plays (one sound, track 2 muted) */
    note(0, 62, 100);
    if (fl_req != 1) FAIL("no request for slot 1 (req %u)", fl_req);
    if (lv_gates(&trk[0])) FAIL("the note played before its slot was loaded");
    fframe();                                          /* the main loop: fl_poll */
    if (fl_cur != 0 || fl_layer || fl_req) FAIL("slot 1 not loaded (cur %u layer %u req %u)", fl_cur, fl_layer, fl_req);
    if (trk[0].p[P_E7] != 5 || !trk[1].p[P_MUTE]) FAIL("slot 1: sound %d, track 2 mute %d", trk[0].p[P_E7], trk[1].p[P_MUTE]);
    events_block(CTL);                                 /* the waiting note, now on the loaded slot */
    if (!lv_gates(&trk[0]) || lv_gates(&trk[1])) FAIL("slot 1 note: gates %u / %u", lv_gates(&trk[0]), lv_gates(&trk[1]));
    noteoff(0, 62);

    /* an OFF channel, and Program Change, change nothing */
    note(2, 64, 100);                                  /* channel 3: slot 3 is OFF, no slot plays it */
    if (fl_req || lv_gates(&trk[0])) FAIL("an OFF channel was heard (req %u)", fl_req);
    {   uint32_t sound = (uint32_t)trk[0].p[P_E7];
        midi_enqueue(0xCu | 0xC1u << 8 | 77u << 16, 1u);
        events_block(CTL);
        fframe();
        if ((uint32_t)trk[0].p[P_E7] != sound || fl_req) FAIL("Program Change changed the sound");
    }

    /* the slots survive the settings: round trip, then a bad record reads as the defaults */
    fl_cfg_save();
    {   uint8_t c[FL_N], a[FL_N], b[FL_N];
        memcpy(c, fl_ch, FL_N); memcpy(a, fl_a, FL_N); memcpy(b, fl_b, FL_N);
        memset(fl_ch, 0, FL_N); memset(fl_a, 0, FL_N); memset(fl_b, 0, FL_N);
        fl_cfg_load();
        if (memcmp(c, fl_ch, FL_N) || memcmp(a, fl_a, FL_N) || memcmp(b, fl_b, FL_N)) FAIL("the slots did not survive the settings");
        if (fl_map[1] != 2) FAIL("map after load");
        *fl_cfg_byte(5) ^= 1;                          /* any change of a byte: the checksum no longer holds */
        fl_cfg_load();
        for (i = 0; i < FL_N; i++)
            if (fl_ch[i] != i || fl_a[i] != i || fl_b[i] != FL_NOB) FAIL("a bad record did not read as the defaults (slot %u)", i);
    }

    /* HOME held again: out of LIVE, track 2 muted, the pages are back */
    fhold(B_HOME);
    if (fm_live) FAIL("HOME held did not close LIVE");
    if (!trk[1].p[P_MUTE] || fl_layer) FAIL("LIVE closed with the layer on");
    note(1, 60, 100);                                  /* no routing outside LIVE: the plain channel setting (all channels) */
    if (fl_req) FAIL("a request outside LIVE");
    ui.force = 1; fa_draw();
    (void)dir;
    printf("fmini_live_test: ok\n");
    return 0;
}
