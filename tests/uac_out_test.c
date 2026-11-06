/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (FMini-1 fork) */
/* Host test of the USB audio playback store in src/usb.c (FELUCCA_UAC_OUT, step 2): uac_out_store turns an
 * EP3 OUT packet (16-bit stereo, little endian) into pb_ring frames (L | R << 16). Checked here: the byte
 * layout, a partial frame (n not a multiple of 4), the largest packet, wrap-around of the ring index, a full
 * ring (the packet is dropped whole and the frames already in the ring stay intact), and the room that the
 * reader (step 3) gives back by advancing pb_r. The SIE (uac_out_rx, uac_out_stream) is not touched. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#define FELUCCA_OTA 0
#define FELUCCA_CDC 0
#define FELUCCA_UAC 1
#define FELUCCA_UAC_OUT 1
#define HALF_FRAMES 128
static void fm1_delay_ms(uint32_t ms) { (void)ms; }
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"   /* SIE register macros (never touched here) */
#include "../firmware/src/usb.c"

static int fails;
static void check(const char *what, int ok)
{
    printf("%-72s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        fails++;
}

/* a packet of f frames: frame i = (L, R) = (i * 3 + 1, -(i + 1)), 16 bits each */
static uint8_t pkt[UAC_MAXP];
static void make_packet(uint32_t f)
{
    uint32_t i;
    for (i = 0; i < f; i++) {
        uint16_t l = (uint16_t)(i * 3u + 1u), r = (uint16_t)(0u - (i + 1u));
        pkt[4u * i] = (uint8_t)l;
        pkt[4u * i + 1u] = (uint8_t)(l >> 8);
        pkt[4u * i + 2u] = (uint8_t)r;
        pkt[4u * i + 3u] = (uint8_t)(r >> 8);
    }
}

static uint32_t want(uint32_t i)
{
    uint16_t l = (uint16_t)(i * 3u + 1u), r = (uint16_t)(0u - (i + 1u));
    return (uint32_t)l | (uint32_t)r << 16;
}

static void reset_ring(void)
{
    pb_w = 0;
    pb_r = 0;
    pb_frames = 0;
}

int main(void)
{
    uint32_t i, f, ok_all;

    /* 1. the byte layout: one frame, L = 0x1234, R = 0xFFFE (-2) */
    reset_ring();
    pkt[0] = 0x34; pkt[1] = 0x12; pkt[2] = 0xFE; pkt[3] = 0xFF;
    check("one frame stored (4 bytes)", uac_out_store(pkt, 4) == 1 && pb_w == 1);
    check("frame = L (low 16 bits) | R << 16, little endian", pb_ring[0] == (0x1234u | 0xFFFEu << 16));

    /* 2. a partial frame: 7 bytes give one frame, the 3 bytes of the rest are not kept */
    reset_ring();
    make_packet(2);
    check("7 bytes: one whole frame, the rest dropped (pb_w 1)", uac_out_store(pkt, 7) == 1 && pb_w == 1);

    /* 3. the largest packet (49 frames, UAC_MAXP bytes): every frame in order */
    reset_ring();
    make_packet(UAC_MAXP / 4u);
    check("largest packet (49 frames) stored", uac_out_store(pkt, UAC_MAXP) == 1 && pb_w == UAC_MAXP / 4u);
    ok_all = 1;
    for (i = 0; i < UAC_MAXP / 4u; i++)
        ok_all &= pb_ring[i] == want(i);
    check("49 frames: each frame = its packet's frame, in order", ok_all);

    /* 4. wrap-around: the index starts 3 short of the end of the ring; 10 frames wrap over it */
    reset_ring();
    pb_w = PB_N - 3u;
    pb_r = pb_w;
    make_packet(10);
    check("10 frames at the end of the ring: stored", uac_out_store(pkt, 40) == 1 && pb_w == PB_N + 7u);
    ok_all = 1;
    for (i = 0; i < 10; i++)
        ok_all &= pb_ring[(PB_N - 3u + i) & (PB_N - 1u)] == want(i);
    check("the frames wrap: slots PB_N-3 .. PB_N-1, 0 .. 6, in order", ok_all);

    /* 5. a full ring: fill it with 49-frame packets; a packet that does not fit is dropped whole */
    reset_ring();
    make_packet(UAC_MAXP / 4u);
    f = 0;
    while (uac_out_store(pkt, UAC_MAXP))
        f++;
    check("the ring takes packets until fewer than 49 frames are free", pb_w - pb_r > PB_N - 49u && pb_w <= PB_N);
    {
        uint32_t before = pb_w, frames_before = pb_frames;
        int dropped = uac_out_store(pkt, UAC_MAXP) == 0;
        check("a packet that does not fit is dropped: nothing written, pb_w unchanged",
              dropped && pb_w == before && pb_frames == frames_before);
        make_packet(1);
        check("a one-frame packet still fits in the free slots (if any)",
              PB_N - (pb_w - pb_r) == 0 || uac_out_store(pkt, 4) == 1);
    }
    ok_all = 1;
    for (i = 0; i < 49; i++)
        ok_all &= pb_ring[i] == want(i);
    check("the frames stored before the drop are intact", ok_all);

    /* 6. the reader gives room back by advancing pb_r (step 3): a full packet fits again */
    pb_r += 100u;
    make_packet(UAC_MAXP / 4u);
    check("after the reader took 100 frames, a 49-frame packet fits", uac_out_store(pkt, UAC_MAXP) == 1);

    /* ---- the reader (audio ISR, usb_play_mix): linear interpolation, the rate follows the fill ---- */
    {
        static int32_t out[2 * 64];
        uint32_t k, blocks;
        int32_t l0, r0;

        /* 7. nothing plays before PB_PRIME frames */
        reset_ring();
        uac_out_alt = 1;
        pb_play = 0;
        make_packet(UAC_MAXP / 4u);
        for (k = 0; k < 2; k++)
            uac_out_store(pkt, UAC_MAXP);                  /* 98 frames */
        memset(out, 0, sizeof out);
        usb_play_mix(out, 32, 4096);
        ok_all = 1;
        for (i = 0; i < 64; i++)
            ok_all &= out[i] == 0;
        check("98 frames (under PB_PRIME 128): nothing plays", ok_all && pb_play == 0 && pb_r == 0);

        /* 8. at the target (fill 256 after the start): the step is exactly one frame, so sample i is frame i */
        reset_ring();
        pb_play = 0;
        make_packet(UAC_MAXP / 4u);
        for (k = 0; k < 5; k++)
            uac_out_store(pkt, UAC_MAXP);                  /* 245 */
        make_packet(13);
        uac_out_store(pkt, 4u * 13u);                      /* 258 */
        memset(out, 0, sizeof out);
        usb_play_mix(out, 32, 4096);
        ok_all = pb_play == 1 && pb_r == 34 && pb_skip == 0 && pb_rep == 0;
        for (i = 0; i < 32; i++) {
            l0 = (int32_t)(int16_t)(pb_ring[i] & 0xFFFFu);
            r0 = (int32_t)(int16_t)(pb_ring[i] >> 16);
            ok_all &= out[2 * i] == l0 && out[2 * i + 1] == r0;
        }
        check("at the target: sample i is frame i, step exact, no counters", ok_all);

        /* 9. MASTER 2048: half the level (arithmetic shift) */
        reset_ring();
        pb_play = 0;
        make_packet(UAC_MAXP / 4u);
        for (k = 0; k < 5; k++)
            uac_out_store(pkt, UAC_MAXP);
        make_packet(13);
        uac_out_store(pkt, 4u * 13u);
        memset(out, 0, sizeof out);
        usb_play_mix(out, 32, 2048);
        ok_all = 1;
        for (i = 0; i < 32; i++) {
            l0 = (int32_t)(int16_t)(pb_ring[i] & 0xFFFFu);
            ok_all &= out[2 * i] == ((l0 * 2048) >> 12);
        }
        check("MASTER 2048: half the level", ok_all);

        /* 10. saturation of the sum at +32767 / -32768 */
        for (i = 0; i < 64; i++)
            out[i] = (i & 1u) ? -32768 : 32767;
        usb_play_mix(out, 32, 4096);
        check("a loud DAC signal plus the playback: saturated at +32767 / -32768",
              out[0] == 32767 && out[1] == -32768);

        /* 11. the host is fast (fill 439 at the start): the reader moves faster, a few counters */
        reset_ring();
        pb_play = 0;
        make_packet(UAC_MAXP / 4u);
        for (k = 0; k < 9; k++)
            uac_out_store(pkt, UAC_MAXP);                  /* 441 */
        memset(out, 0, sizeof out);
        usb_play_mix(out, 32, 4096);
        check("fill 439 (above 384): the reader is faster (32 or 33 frames for 32 samples), pb_skip 1",
              pb_skip == 1 && pb_r - 2u >= 32u && pb_r - 2u <= 33u);

        /* 12. the host is slow (fill 127 at the start): the reader takes fewer, pb_rep 1 */
        reset_ring();
        pb_play = 0;
        make_packet(UAC_MAXP / 4u);
        uac_out_store(pkt, UAC_MAXP);
        uac_out_store(pkt, UAC_MAXP);
        make_packet(31);
        uac_out_store(pkt, 4u * 31u);                      /* 129 frames: 127 after the start */
        memset(out, 0, sizeof out);
        usb_play_mix(out, 32, 4096);
        check("fill 127 (below 128): the reader takes fewer than 33, pb_rep 1",
              pb_rep == 1 && pb_r - 2u <= 32u && pb_r - 2u >= 31u);

        /* 13. the ring runs dry: silence after it, not playing, pb_under 1 */
        reset_ring();
        pb_play = 0;
        make_packet(UAC_MAXP / 4u);
        for (k = 0; k < 2; k++)
            uac_out_store(pkt, UAC_MAXP);
        make_packet(32);
        uac_out_store(pkt, 4u * 32u);                      /* 130 frames */
        memset(out, 0, sizeof out);
        for (blocks = 0; blocks < 8; blocks++) {
            memset(out, 0, sizeof out);
            usb_play_mix(out, 32, 4096);
            if (pb_under)
                break;
        }
        check("the ring runs dry: pb_under 1, not playing, the rest of that block is silence",
              pb_under == 1 && pb_play == 0 && out[2 * 31] == 0);

        /* 14. SET_INTERFACE / reset: the flag makes the reader start empty, from the writer's index */
        reset_ring();
        pb_play = 1;
        make_packet(UAC_MAXP / 4u);
        uac_out_store(pkt, UAC_MAXP);
        pb_flush = 1;
        usb_play_mix(out, 32, 4096);
        check("flush: pb_r moves to pb_w, not playing, flag cleared", pb_r == pb_w && pb_play == 0 && pb_flush == 0);

        /* 15. alt 0: the reader plays nothing and consumes nothing */
        reset_ring();
        pb_play = 1;
        make_packet(UAC_MAXP / 4u);
        for (k = 0; k < 4; k++)
            uac_out_store(pkt, UAC_MAXP);
        uac_out_alt = 0;
        memset(out, 0, sizeof out);
        usb_play_mix(out, 32, 4096);
        check("alt 0: nothing consumed, nothing mixed", pb_r == 0 && out[0] == 0 && pb_play == 0);
        uac_out_alt = 1;

        /* 16. a whole session: the host's clock 0.04 % slow against the DAC (44117.6 Hz), 10 s of 32-frame blocks:
         * the rate follows, the fill stays near TARGET, the ring never runs dry */
        {
            uint32_t blocks2 = 0, written = 0, target;
            reset_ring();
            pb_play = 0;
            pb_under = pb_rep = pb_skip = 0;
            while (blocks2 < 13786u) {
                target = (uint32_t)((uint64_t)(blocks2 + 1u) * 32u * 44100u / 44117u);
                while (written < target) {
                    uint32_t f = target - written > 45u ? 45u : target - written;
                    make_packet(f);
                    uac_out_store(pkt, 4u * f);
                    written += f;
                }
                usb_play_mix(out, 32, 4096);
                blocks2++;
            }
            check("10 s, host 0.04 % slow: no dry ring, still playing", pb_under == 0 && pb_play == 1);
            check("10 s: the fill settles near TARGET (within 200..320)",
                  (pb_w - pb_r) >= 200u && (pb_w - pb_r) <= 320u);
        }
    }

    printf(fails ? "USB playback store: %d FAILED\n" : "uac_out: all ok\n", fails);
    return fails != 0;
}
