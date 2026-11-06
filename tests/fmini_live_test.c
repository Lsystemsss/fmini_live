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
    for (i = 0; i < LV_N; i++)
        if (lv_ch[i] != i || lv_a[i] != i || lv_b[i] != LV_NOB) FAIL("default slot %u", i);

    /* HOME: a tap is the first page as before, a hold is LIVE */
    fpress(B_HOME);
    if (fm_live) FAIL("a tap on HOME opened LIVE");
    fhold(B_HOME);
    if (!fm_live) FAIL("HOME held did not open LIVE");
    fframes(300);
    if (lv_cur != lv_row) FAIL("the slot the list is on was not loaded (cur %u row %u)", lv_cur, lv_row);
    ui.force = 1; fa_draw();                           /* the list draws (no crash, the panel is not the pages') */
    {   FILE *f; char p[256]; uint32_t k;               /* the list, as a picture: OUT/live.ppm */
        lv_ta[1] = -3; lv_tb[1] = 2; lv_b[1] = 20; lv_row = 1; ui.force = 1; fa_draw();
        snprintf(p, sizeof p, "%s/live.ppm", dir);
        f = fopen(p, "wb");
        if (f) {
            fprintf(f, "P6\n240 240\n255\n");
            for (k = 0; k < 240u * 240u; k++) {
                uint16_t c = swap16(host_screen[k]);
                uint8_t px[3] = {(uint8_t)((c >> 11) * 255u / 31u), (uint8_t)(((c >> 5) & 63u) * 255u / 63u), (uint8_t)((c & 31u) * 255u / 31u)};
                fwrite(px, 1, 3, f);
            }
            fclose(f);
        }
        lv_ta[1] = lv_tb[1] = 0; lv_b[1] = LV_NOB; lv_row = 0;
    }

    /* the list: SELECT moves the row, K1 the channel, K2 the sound, K3 the layer; the row loads after resting */
    lv_ch[0] = 0; lv_a[0] = 5;  lv_b[0] = LV_NOB;       /* slot 1: channel 1, preset 5 */
    lv_ch[1] = 1; lv_a[1] = 10; lv_b[1] = 20;           /* slot 2: channel 2, preset 10 + layer 20 */
    lv_ch[2] = LV_OFF; lv_a[2] = 30; lv_b[2] = LV_NOB;  /* slot 3: OFF */
    lv_ch[3] = 0; lv_a[3] = 40; lv_b[3] = LV_NOB;       /* slot 4: channel 1 as well: the first slot (1) wins */
    lv_map_build();
    if (lv_map[0] != 1 || lv_map[1] != 2 || lv_map[2] != 0) FAIL("map %u %u %u", lv_map[0], lv_map[1], lv_map[2]);
    lv_row = 0; lv_cur = 0xFF; lv_want = 0xFF;
    fturn(EN_SELECT, 1);
    if (lv_row != 1) FAIL("SELECT: row %u", lv_row);
    fturn(EN_K1, 1);
    if (lv_ch[1] != 2) FAIL("K1: channel %u", lv_ch[1]);
    fturn(EN_K1, -1);
    {   uint8_t b0 = lv_b[1];
        fturn(EN_K3, 1);
        if (lv_b[1] == b0) FAIL("K3 does not move the layer");
    }
    lv_b[1] = 20;
    lv_touch();
    fframes(300);
    if (lv_cur != 1 || !lv_layer) FAIL("slot 2 not loaded by resting on it (cur %u layer %u)", lv_cur, lv_layer);
    if (trk[0].p[P_E7] != 10 || trk[1].p[P_E7] != 20) FAIL("sounds: %d / %d", trk[0].p[P_E7], trk[1].p[P_E7]);
    if (trk[1].p[P_MUTE]) FAIL("the layer track is muted");

    /* VOLUME: K4 is A's, ALGORITHM B's: 1 dB a detent on the preset's own level (0.5 dB a step), heard now, no reload */
    {   int16_t a0 = trk[0].p[P_LEVEL], b0 = trk[1].p[P_LEVEL];
        uint8_t cur = lv_cur;
        if (lv_ta[1] || lv_tb[1] || lv_base[0] != a0 || lv_base[1] != b0) FAIL("volumes at the start: %d %d base %d %d", lv_ta[1], lv_tb[1], lv_base[0], lv_base[1]);
        fturn(EN_K4, -3);
        if (lv_ta[1] != -3 || trk[0].p[P_LEVEL] != a0 - 6) FAIL("K4: volume %d level %d (was %d)", lv_ta[1], trk[0].p[P_LEVEL], a0);
        if (trk[1].p[P_LEVEL] != b0) FAIL("K4 moved B");
        host_enc[panel.enc[EN_ALGO]] += 2 * panel.dir[EN_ALGO]; fframe();
        if (lv_tb[1] != 2 || trk[1].p[P_LEVEL] != lv_level(b0, 2)) FAIL("ALGORITHM: volume %d level %d", lv_tb[1], trk[1].p[P_LEVEL]);
        if (lv_cur != cur) FAIL("a volume reloaded the slot");
        fturn(EN_K4, -100);
        if (lv_ta[1] != LV_TMIN) FAIL("A volume floor %d", lv_ta[1]);
        fturn(EN_K4, 100);
        if (lv_ta[1] != LV_TMAX) FAIL("A volume ceiling %d", lv_ta[1]);
        lv_ta[1] = -3; lv_tb[1] = 2;
        trk[0].p[P_LEVEL] = lv_level(lv_base[0], -3);
        lv_row = 0;                                    /* slot 1 has no layer: ALGORITHM does nothing there */
        host_enc[panel.enc[EN_ALGO]] += 1 * panel.dir[EN_ALGO]; fframe();
        if (lv_tb[0] != 0) FAIL("ALGORITHM changed the volume of a missing layer");
        lv_row = 1;
        if (lv_level(0, 5) != 0 || lv_level(120, 7) != 127 || lv_level(3, -24) != 1) FAIL("lv_level");
    }

    /* a note on the loaded slot's channel plays both sounds, a release lets both go */
    note(1, 60, 100);
    if (!lv_gates(&trk[0]) || !lv_gates(&trk[1])) FAIL("layer: gates %u / %u", lv_gates(&trk[0]), lv_gates(&trk[1]));
    noteoff(1, 60);
    if (lv_gates(&trk[0]) || lv_gates(&trk[1])) FAIL("layer: still gated after the release %u / %u", lv_gates(&trk[0]), lv_gates(&trk[1]));

    /* a note on another slot's channel: the message waits, the slot loads, then the note plays (one sound, track 2 muted) */
    note(0, 62, 100);
    if (lv_req != 1) FAIL("no request for slot 1 (req %u)", lv_req);
    if (lv_gates(&trk[0])) FAIL("the note played before its slot was loaded");
    fframe();                                          /* the main loop: lv_poll */
    if (lv_cur != 0 || lv_layer || lv_req) FAIL("slot 1 not loaded (cur %u layer %u req %u)", lv_cur, lv_layer, lv_req);
    if (trk[0].p[P_E7] != 5 || !trk[1].p[P_MUTE]) FAIL("slot 1: sound %d, track 2 mute %d", trk[0].p[P_E7], trk[1].p[P_MUTE]);
    events_block(CTL);                                 /* the waiting note, now on the loaded slot */
    if (!lv_gates(&trk[0]) || lv_gates(&trk[1])) FAIL("slot 1 note: gates %u / %u", lv_gates(&trk[0]), lv_gates(&trk[1]));
    noteoff(0, 62);

    /* an OFF channel, and Program Change, change nothing */
    note(2, 64, 100);                                  /* channel 3: slot 3 is OFF, no slot plays it */
    if (lv_req || lv_gates(&trk[0])) FAIL("an OFF channel was heard (req %u)", lv_req);
    {   uint32_t sound = (uint32_t)trk[0].p[P_E7];
        midi_enqueue(0xCu | 0xC1u << 8 | 77u << 16, 1u);
        events_block(CTL);
        fframe();
        if ((uint32_t)trk[0].p[P_E7] != sound || lv_req) FAIL("Program Change changed the sound");
    }

    /* the slots survive the settings: round trip, then a bad record reads as the defaults */
    lv_cfg_save();
    lv_ta[15] = LV_TMIN; lv_tb[15] = LV_TMAX; lv_ch[15] = 15; lv_a[15] = 199; lv_b[15] = 198;   /* the last slot: the last bits of the 62 bytes */
    lv_cfg_save();
    {   uint8_t c[LV_N], a[LV_N], b[LV_N];
        int8_t ta[LV_N], tb[LV_N];
        memcpy(c, lv_ch, LV_N); memcpy(a, lv_a, LV_N); memcpy(b, lv_b, LV_N); memcpy(ta, lv_ta, LV_N); memcpy(tb, lv_tb, LV_N);
        memset(lv_ch, 0, LV_N); memset(lv_a, 0, LV_N); memset(lv_b, 0, LV_N); memset(lv_ta, 0, LV_N); memset(lv_tb, 0, LV_N);
        lv_cfg_load();
        if (memcmp(c, lv_ch, LV_N) || memcmp(a, lv_a, LV_N) || memcmp(b, lv_b, LV_N)) FAIL("the slots did not survive the settings");
        if (memcmp(ta, lv_ta, LV_N) || memcmp(tb, lv_tb, LV_N)) FAIL("the volumes did not survive the settings (%d %d)", lv_ta[1], lv_tb[1]);
        if (lv_ta[1] != -3 || lv_tb[1] != 2 || lv_ta[15] != LV_TMIN || lv_tb[15] != LV_TMAX) FAIL("volumes: %d %d %d %d", lv_ta[1], lv_tb[1], lv_ta[15], lv_tb[15]);
        if (lv_map[1] != 2) FAIL("map after load");
        *lv_cfg_byte(5) ^= 1;                          /* any change of a byte: the checksum no longer holds */
        lv_cfg_load();
        for (i = 0; i < LV_N; i++)
            if (lv_ch[i] != i || lv_a[i] != i || lv_b[i] != LV_NOB || lv_ta[i] || lv_tb[i]) FAIL("a bad record did not read as the defaults (slot %u)", i);
    }

    /* HOME held again: out of LIVE, track 2 muted, the pages are back */
    fhold(B_HOME);
    if (fm_live) FAIL("HOME held did not close LIVE");
    if (!trk[1].p[P_MUTE] || lv_layer) FAIL("LIVE closed with the layer on");
    note(1, 60, 100);                                  /* no routing outside LIVE: the plain channel setting (all channels) */
    if (lv_req) FAIL("a request outside LIVE");
    ui.force = 1; fa_draw();
    (void)dir;
    printf("fmini_live_test: ok\n");
    return 0;
}
