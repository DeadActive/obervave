/* SPDX-License-Identifier: GPL-3.0-only
 * BLE MIDI: 2026 DEADACTIVE (Felucca's tests/smc_test.c on OberVave's harness) */
/* The SMC-Mixer DAW layout on OberVave's mixer (firmware/src/smc.c) on the host: faders 1-4 LEVEL and 5-8 REVERB
 * with pickup, knobs 1-4 PAN and 5-8 the selected track's PERFORM parameters, the buttons (REC SOLO MUTE SELECT, the
 * transport, track < >), the LEDs and fader positions sent (only changes, all on a new link), the mixer gone. */
#include "bt_test.h"

static void smc_pkt(uint8_t st, uint8_t d1, uint8_t d2)
{
    bt_midi_in((uint32_t)(st >> 4) | (uint32_t)st << 8 | (uint32_t)d1 << 16 | (uint32_t)d2 << 24);
}
static void fader(uint32_t n, uint32_t v) { smc_pkt((uint8_t)(0xE0u + n), (uint8_t)((v << 7) & 0x7Fu), (uint8_t)v); }
static void knob(uint32_t k, int32_t clicks) { smc_pkt(0xB0, (uint8_t)(16u + k), (uint8_t)(clicks < 0 ? 0x40 | -clicks : clicks)); }
static void button(uint32_t note) { smc_pkt(0x90, (uint8_t)note, 127); smc_pkt(0x80, (uint8_t)note, 0); }
static uint32_t led(uint32_t note)                   /* the last value sent for an LED, 0xFF none */
{
    uint32_t i, v = 0xFFu;
    for (i = 0; i < host_ble_out_n && i < 256u; i++)
        if (host_ble_out[i][0] == 0x90 && host_ble_out[i][1] == note)
            v = host_ble_out[i][2];
    return v;
}

int main(void)
{
    uint32_t n0;
    int32_t pan0;
    bt_start();
    bt_fresh(0);
    {
        bt_set_t s;
        bt_set_get(&s);
        s.mode = BT_MODE_AUTO;
        bt_set_put(&s);
    }
    bt_cpy(conn_name, "SMC-Mixer", sizeof conn_name);
    conn_up = 1;
    bt_link(1);
    smc_link(1);
    ok("the mixer by its name: SMC DAW mode", bt_mode_now());
    smc_frame();
    ok("a new link: every LED and every fader position sent", host_ble_out_n >= 35u + 8u &&
       led(0x18) == 127u && led(0x19) == 0u && led(0x5D) == 127u && led(0x5E) == 0u);
    ok("fader positions: LEVEL 100 on 1-4, REVERB 0 on 5-8", ble_out_find(0xE0, 100, 100) == 1u &&
       ble_out_find(0xE4, 0, 0) == 1u);
    n0 = host_ble_out_n;
    smc_frame();
    ok("nothing changed: nothing sent", host_ble_out_n == n0);

    /* faders, with pickup */
    fader(0, 40);
    ok("fader 1 far from LEVEL 100: not picked up", set.mix_lvl[0] == 100u);
    fader(0, 99);
    ok("fader 1 within 2 of it: picked up, LEVEL follows", set.mix_lvl[0] == 99u && ctl_mix_lvl[0] == 99u);
    fader(0, 60);
    ok("then it follows freely", set.mix_lvl[0] == 60u && set_dirty);
    fader(5, 30);
    fader(5, 1);
    ok("fader 6 (REVERB B): crossing its value picks it up", set.mix_rev[1] == 1u);
    set.mix_lvl[0] = 110;                           /* the FM-1's mixer moved it */
    smc_frame();
    ok("the FM-1 moved LEVEL A: its position sent, the fader unpicked", ble_out_find(0xE0, 110, 110) == 1u);
    fader(0, 61);
    ok("unpicked: the fader far off does nothing", set.mix_lvl[0] == 110u);

    /* knobs */
    pan0 = set.mix_pan[2];
    knob(2, 3);
    ok("knob 3: track C's PAN right by 3", set.mix_pan[2] == pan0 + 3 && ctl_mix_pan[2] == pan0 + 3);
    knob(2, -5);
    ok("knob 3 back: left by 5", set.mix_pan[2] == pan0 - 2);
    trk_select(1);
    {
        int32_t p = PAGE[PG_PERFORM].p[0], v0 = live_p[1].v[p], a0 = live_p[0].v[p];
        knob(4, 2);
        ok("knob 5: the selected track's (B) first PERFORM parameter, A's untouched", live_p[1].v[p] != v0 &&
           live_p[0].v[p] == a0);
    }

    /* buttons */
    button(0x12);
    ok("MUTE 3: track C muted (the mixer's)", set.mix_mute & 4u);
    button(0x12);
    ok("MUTE 3 again: on", !(set.mix_mute & 4u));
    button(0x09);
    ok("SOLO 2: track B solo", set.mix_solo & 2u);
    button(0x09);
    button(0x1B);
    ok("SELECT 4: track D", seq_trk == 3u);
    button(0x2E);
    ok("BANK <: the previous track", seq_trk == 2u);
    button(0x31);
    button(0x31);
    ok("CHANNEL > twice: wraps to A", seq_trk == 0u);
    button(0x1C);
    ok("strips 5-8: nothing", seq_trk == 0u);
    ctl_seq_run = 0;
    button(0x5E);
    ok("PLAY: the sequencer from the top", ctl_seq_run && ctl_seq_restart);
    button(0x01);
    ok("REC 2 while playing: track B selected, recording", seq_trk == 1u && rec == REC_ON);
    smc_frame();
    ok("LEDs: REC 2, SELECT 2, PLAY, REC lit; STOP off", led(0x01) == 127u && led(0x19) == 127u && led(0x18) == 0u &&
       led(0x5E) == 127u && led(0x5F) == 127u && led(0x5D) == 0u);
    button(0x5D);
    ok("STOP: stopped, the take ends", !ctl_seq_run && rec == REC_OFF);
    smc_frame();
    ok("LEDs: STOP lit, PLAY and REC off", led(0x5D) == 127u && led(0x5E) == 0u && led(0x5F) == 0u && led(0x01) == 0u);
    button(0x5F);
    ok("REC stopped: armed (the first note starts it)", rec == REC_READY);
    rec = REC_OFF;

    /* MODE leaves SMC DAW while linked: the LEDs off once */
    {
        bt_set_t s;
        bt_set_get(&s);
        s.mode = BT_MODE_MIDI;
        bt_set_put(&s);
    }
    smc_frame();
    ok("MODE MIDI: every LED off once", led(0x19) == 0u && led(0x5D) == 0u);
    n0 = host_ble_out_n;
    smc_frame();
    ok(".. and then nothing", host_ble_out_n == n0);
    printf(fails ? "smc: %d FAILED\n" : "smc: all checks passed\n", fails);
    return fails != 0;
}
