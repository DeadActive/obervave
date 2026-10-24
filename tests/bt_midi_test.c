/* SPDX-License-Identifier: GPL-3.0-only
 * BLE MIDI: 2026 DEADACTIVE (Felucca's tests/bt_midi_test.c on OberVave's harness) */
/* Bluetooth MIDI's core side on the host (firmware/src/bt_midi.c, bt_set.c; obervave.c midi_in): the settings
 * bytes, MIDI mode in through its own ring into midi_in (notes play the track's sound, after USB / TRS's), SMC DAW
 * mode to the layout, FM-LINK mode nothing; the realtime bytes by SYNC (AUTO: any clock; INTERNAL, LINK: none; BLE:
 * the BLE device's only, its clock followed); the release on a lost link; the keys' MIDI out to the device; the clock
 * out (Start, 24 a beat at OberVave's tempo, Stop) on its own tempo only. */
#include "bt_test.h"

static uint32_t sounding(uint32_t note)              /* voices on note with their gate open */
{
    uint32_t v, n = 0;
    for (v = 0; v < NV; v++)
        n += vc[v].gate && vc[v].note == note;
    return n;
}
static void audio(uint32_t halves)                   /* the audio ISR's halves (the engine's events, the sequencer) */
{
    static int32_t o[2u * HALF_FRAMES];
    while (halves--) {
        memset(o, 0, sizeof o);
        engine_audio(o, HALF_FRAMES);
    }
}
static void ble_in(uint8_t st, uint8_t d1, uint8_t d2)   /* a message from the BLE device (core_ble_midi_in) */
{
    bt_midi_in((uint32_t)(st >> 4) | (uint32_t)st << 8 | (uint32_t)d1 << 16 | (uint32_t)d2 << 24);
}
static void ble_rt(uint8_t st) { bt_midi_in(0xFu | (uint32_t)st << 8); }
static void link_up(const char *name, uint32_t mode)
{
    bt_set_t s;
    bt_set_get(&s);
    s.mode = (uint8_t)mode;
    bt_set_put(&s);
    bt_cpy(conn_name, name, sizeof conn_name);
    conn_up = 1;
    bt_link(1);
}

int main(void)
{
    bt_set_t s;
    uint32_t i, n0, f8;
    bt_start();

    bt_fresh(0);
    bt_set_get(&s);
    ok("settings: none saved -> MODE AUTO, AUTOSTART OFF, ROLE HOST, RECONNECT AUTO",
       s.mode == BT_MODE_AUTO && !s.autostart && !s.role && s.rate == BT_RATE_AUTO);
    s.mode = BT_MODE_MIDI;
    s.autostart = 1;
    s.role = 1;
    s.rate = BT_RATE_SLOW;
    bt_set_put(&s);
    memset(&s, 0, sizeof s);
    bt_set_get(&s);
    ok("settings: MODE, AUTOSTART, ROLE, RECONNECT read back", s.mode == BT_MODE_MIDI && s.autostart && s.role &&
       s.rate == BT_RATE_SLOW);
    ok("settings: RECONNECT SLOW rests long, also stopped", bt_set_slow(0));
    s.rate = BT_RATE_AUTO;
    bt_set_put(&s);
    ok("settings: RECONNECT AUTO: long while playing, short stopped", bt_set_slow(1) && !bt_set_slow(0));
    bts.set[7] ^= 1u;
    bt_set_get(&s);
    ok("settings: a damaged check byte -> defaults", s.mode == BT_MODE_AUTO && !s.autostart);

    /* MIDI mode in: the device plays the track's sound */
    bt_fresh(0);
    set.midi_ch = 0;
    trk_select(2);                                  /* (track C: WARM PAD, poly, no arp: a note is a voice) */
    link_up("Xkey Air 25", BT_MODE_AUTO);
    ok("AUTO: a device not named SMC -> MIDI mode, MIDI out on", !bt_mode_now() && bt_in.out_on);
    ble_in(0x90, 60, 100);
    ok("MIDI in: queued in the BLE ring (not USB's)", bt_rx_w - bt_rx_r == 1u && mq_w == mq_r);
    midi_in(0);
    audio(2);
    ok("MIDI in: midi_in takes it, the note sounds", sounding(60) > 0u && bt_rx_r == bt_rx_w);
    ble_in(0x80, 60, 0);
    midi_in(0);
    audio(2);
    ok("MIDI in: its note-off ends it", sounding(60) == 0u);
    ble_in(0xB0, 74, 10);
    midi_in(0);
    ok("MIDI in: a CC is the panel's (CC 74 cutoff)", ed_patch(P_CUTOFF)->v[P_CUTOFF] == (PDEF[P_CUTOFF].max == 127 ? 10 :
       (10 * PDEF[P_CUTOFF].max + 63) / 127));
    ble_in(0x91, 62, 100);                          /* channel 2 with OMNI: still the panel's track */
    mq[mq_w % 64u][0] = 0x90; mq[mq_w % 64u][1] = 64; mq[mq_w % 64u][2] = 90; mq_w++;   /* USB at the same time */
    midi_in(0);
    audio(2);
    ok("MIDI in: USB's and the device's both (USB first)", sounding(62) && sounding(64));
    ble_in(0x81, 62, 0);
    mq[mq_w % 64u][0] = 0x80; mq[mq_w % 64u][1] = 64; mq[mq_w % 64u][2] = 0; mq_w++;
    midi_in(0);
    for (i = 0; i < 40u; i++)
        ble_in(0x90, 70, 1);
    ok("MIDI in: a full ring drops (counted), never blocks", bt_in.drops == 40u - BT_RX_Q);
    midi_in(0);
    for (i = 0; i < 40u; i++)
        ble_in(0x80, 70, 0);
    midi_in(0);
    bt_in.drops = 0;
    ble_in(0xF0, 1, 2);                             /* SysEx and the like: dropped by the filter */
    ok("MIDI in: anything but channel voice and F8 FA FB FC is dropped", bt_rx_w == bt_rx_r);

    /* the link lost: what the device held is released */
    bt_fresh(0);
    link_up("Xkey Air 25", BT_MODE_MIDI);
    audio(4);
    ble_in(0x93, 65, 100);                          /* channel 4 */
    ble_in(0xB3, 64, 127);                          /* its pedal down */
    midi_in(0);
    conn_up = 0;
    bt_link(0);
    bt_midi_frame();
    ok("link lost: nothing at once (the device's last note reaches the engine first)", bt_rx_w == bt_rx_r);
    audio(2);
    fm1_ms += BT_REL_MS;
    bt_midi_frame();
    ok("link lost: CC 64 = 0 and CC 123 on the channel it played", bt_rx_w - bt_rx_r == 2u &&
       bt_rx_q[bt_rx_r % BT_RX_Q] == (0x0Bu | 0xB3u << 8 | 64u << 16) &&
       bt_rx_q[(bt_rx_r + 1u) % BT_RX_Q] == (0x0Bu | 0xB3u << 8 | 123u << 16));
    midi_in(0);
    audio(40);
    ok("link lost: the note it held ends", sounding(65) == 0u && !ctl_sustain);

    /* the realtime bytes by SYNC */
    bt_fresh(0);
    link_up("Xkey Air 25", BT_MODE_MIDI);
    bt_sync_set(BT_SYNC_AUTO);
    ctl_seq_run = 0;
    ble_rt(0xFA);
    midi_in(0);
    ok("SYNC AUTO: the device's Start runs the sequencer", ctl_seq_run);
    ble_rt(0xFC);
    midi_in(0);
    ok("SYNC AUTO: its Stop stops it", !ctl_seq_run);
    bt_sync_set(BT_SYNC_INT);
    ble_rt(0xFA);
    midi_in(0);
    ok("SYNC INTERNAL: the device's Start ignored", !ctl_seq_run);
    bt_sync_set(BT_SYNC_LINK);
    ble_rt(0xFA);
    midi_in(0);
    ok("SYNC LINK: ignored", !ctl_seq_run);
    bt_sync_set(BT_SYNC_BLE);
    mq[mq_w % 64u][0] = 0xFA; mq[mq_w % 64u][1] = 0; mq[mq_w % 64u][2] = 0; mq_w++;   /* USB's Start */
    midi_in(0);
    ok("SYNC BLE: USB's Start ignored", !ctl_seq_run);
    ble_rt(0xFA);
    midi_in(0);
    ok("SYNC BLE: the device's Start runs it", ctl_seq_run);
    {                                               /* its clock at 100 BPM: 24 a quarter, the tempo followed */
        uint32_t q = 60u * FS / 100u, k;
        ctl_ext_quarter = 0;
        clk_n = 0;
        for (k = 0; k < 4u * 24u + 2u; k++) {
            ble_rt(0xF8);
            midi_in(0);
            eng_t += q / 24u;
            fm1_ms += 25u;
        }
        ok("SYNC BLE: the device's clock sets the tempo (100 BPM)", ctl_ext_quarter && ctl_ext_quarter > q - 100u &&
           ctl_ext_quarter < q + 100u);
    }
    ble_rt(0xFC);
    midi_in(0);
    ok("SYNC BLE: the device's Stop stops it", !ctl_seq_run);
    ctl_ext_quarter = 0;

    /* SMC DAW and FM-LINK modes */
    bt_fresh(0);
    link_up("SMC-Mixer", BT_MODE_AUTO);
    ok("AUTO: a device named SMC... -> the SMC-Mixer layout, no MIDI out", bt_mode_now() && !bt_in.out_on);
    ble_in(0x90, 60, 100);
    ok("SMC DAW: its notes go to the layout, not into midi_in", bt_rx_w == bt_rx_r);
    bt_fresh(0);
    link_up("Xkey Air 25", BT_MODE_LINK);
    ble_in(0x90, 60, 100);
    ok("FM-LINK: the device's MIDI is dropped, nothing goes out", bt_rx_w == bt_rx_r && !bt_in.out_on);

    /* out: the keys to the device, in MIDI mode */
    bt_fresh(0);
    link_up("Xkey Air 25", BT_MODE_MIDI);
    bt_keys_send(0x90, 60, 99);
    bt_keys_send(0x80, 60, 0);
    bt_midi_frame();
    ok("MIDI out: the keys' notes to the device too", ble_out_find(0x90, 60, 99) == 1u && ble_out_find(0x80, 60, 0) == 1u);
    host_ble_room = 0;
    bt_keys_send(0x90, 61, 99);
    bt_midi_frame();
    ok("MIDI out: no room in the BLE unit -> kept, sent when there is", ble_out_find(0x90, 61, 99) == 0u &&
       bt_out_w - bt_out_r == 1u);
    host_ble_room = 64;
    bt_midi_frame();
    ok("MIDI out: .. then it goes", ble_out_find(0x90, 61, 99) == 1u);
    conn_up = 0;
    bt_link(0);
    bt_keys_send(0x90, 62, 99);
    ok("MIDI out: not linked -> nothing queued", bt_out_w == bt_out_r);

    /* the clock out: OberVave on its own tempo, linked in MIDI mode */
    bt_fresh(0);
    link_up("Xkey Air 25", BT_MODE_MIDI);
    bt_sync_set(BT_SYNC_INT);
    ctl_tempo = 120;
    ctl_ext_quarter = 0;
    memset(&bt_fml, 0, sizeof bt_fml);
    bt_fml_frame();                                 /* (the role: leader, no link) */
    ctl_seq_restart = 1;
    ctl_seq_run = 1;
    n0 = host_ble_out_n;
    for (i = 0; i < (uint32_t)(quarter_len() * 4u / CTL); i++) {   /* four beats of blocks */
        uint32_t k, nt = bt_seq_ticks(0);
        for (k = 0; k < nt; k++)
            seq_tick();
        if (i % 16u == 0u)
            bt_midi_frame();
    }
    bt_midi_frame();
    f8 = ble_out_find(0xF8, 0, 0);
    ok("clock out: Start on PLAY", ble_out_find(0xFA, 0, 0) == 1u && host_ble_out[n0 % 256u][0] == 0xFA);
    printf("      %u clock pulses in four beats\n", f8);
    ok("clock out: 24 a beat (96 in four, the first with Start)", f8 >= 96u && f8 <= 97u);
    ctl_seq_run = 0;
    (void)bt_seq_ticks(0);
    bt_midi_frame();
    ok("clock out: Stop on STOP", ble_out_find(0xFC, 0, 0) == 1u);
    bt_sync_set(BT_SYNC_BLE);                       /* following the device's clock: none back */
    bt_fml_frame();
    n0 = host_ble_out_n;
    ctl_seq_restart = 1;
    ctl_seq_run = 1;
    for (i = 0; i < 2000u; i++)
        (void)bt_seq_ticks(0);
    bt_midi_frame();
    ok("clock out: none while it follows a clock (SYNC BLE): no loop", host_ble_out_n == n0);
    ctl_seq_run = 0;
    printf(fails ? "bt_midi: %d FAILED\n" : "bt_midi: all checks passed\n", fails);
    return fails != 0;
}
