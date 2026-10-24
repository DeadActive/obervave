/* SPDX-License-Identifier: GPL-3.0-only
 * FM-Link: 2026 DEADACTIVE (from Felucca's ble-kit bt_fml.c) */
/* FELUCCA_BLE (obervave.c includes it after bt_midi.c): FM-Link's glue (Felucca's
 * docs/superpowers/specs/2026-10-21-fm-link-design.md), the engine (fmlink.c, Felucca's, unchanged: FM-1s on
 * different firmwares keep time with each other) with OberVave's clock, sequencer and Bluetooth link. OberVave on its
 * own tempo leads (SYNC INTERNAL, or AUTO with no MIDI clock coming in), SYNC LINK follows; on another clock (AUTO
 * following a MIDI clock, SYNC BLE) it takes no part (its HELLOs still say it is an FM-1).
 *   The audio ISR, every control block (obv_engine.h engine_audio, before the sequencer's tick): bt_seq_ticks with the
 *   tick the block is heard at (platform.c audio_t0, the half's entry, + the frames before it). The panel's (and MIDI's)
 *   PLAY / STOP are what the main loop wrote to ctl_seq_run; FM-Link decides when they act (a leader's PLAY with a
 *   follower linked: both start 80 ms later; a linked follower's own PLAY / STOP: the leader's count). The sequencer's
 *   advance is a block, a sample more or less (the follower held on the leader): OberVave's sequencer steps in whole
 *   control blocks (CTL frames), so the difference is kept and paid in whole ticks (a block with none, or two), at
 *   most CTL frames off FM-Link's own count.
 *   The main loop: bt_fml_frame (bt_service): the role from SYNC, the messages out; core_fml_rx: in (the follower's
 *   tempo set from the leader's at once, so the next block runs at it).
 * The engine's words are shared with the audio ISR: the main loop's calls run with interrupts off. */
#define BT_FML_C 1
#include "fmlink.c"
static fml_t bt_fml;
#ifdef OBV_HOST
static uint32_t bt_fml_heard;                        /* (the host: the tests set it) */
#define BT_FML_HEARD(b) bt_fml_heard
#else
#define BT_FML_HEARD(b) (audio_t0 + (HALF_FRAMES + (b)) * FML_TPS)   /* platform.c: the half's entry */
#endif
static struct {
    uint8_t b[20], len;                              /* a message ble_fml_send had no room for: first, next pass */
    uint32_t ms;                                     /* when it was made: dropped 50 ms on (a stale time or state) */
    uint8_t said;                                    /* the peer announced (say) while linked */
} bt_fo;
static uint32_t bt_seq_was;                          /* the ISR: the sequencer ran in the last block */
static int32_t bt_seq_owed;                          /* the ISR: frames FM-Link's advance is ahead of the ticks run */

static uint32_t bt_fml_role(void)                    /* from SYNC, and AUTO's clock (main loop and ISR) */
{
    uint32_t v = bt_sync();
    return v == BT_SYNC_INT || (v == BT_SYNC_AUTO && !ctl_ext_quarter) ? FML_LEADER :
           v == BT_SYNC_LINK ? FML_FOLLOWER : FML_NONE;
}

/* the audio ISR, every block of CTL frames at b in the half: the transport as FM-Link wants it, the MIDI clock out,
 * and how many sequencer ticks this block runs (1; 0 or 2 to pay FM-Link's advance) */
static uint32_t bt_seq_ticks(uint32_t b)
{
    uint32_t want = ctl_seq_run, ui = 0, req, seq_n = CTL, role = bt_fml.role, run;
    int lead = role == FML_LEADER;
    if (want && (!bt_seq_was || ctl_seq_restart))
        ui = bt_seq_was ? 3u : 1u;                   /* the panel's PLAY, or from the top while playing */
    else if (!want && bt_seq_was)
        ui = 2u;
    if (role != FML_NONE && role == bt_fml_role()) { /* (SYNC changed: the main loop sets the engine up first) */
        req = fml_block(&bt_fml, BT_FML_HEARD(b), CTL, ctl_tempo, quarter_len(), (int)bt_seq_was, ui, &seq_n);
        if (req == 1u || req == 3u) {
            ctl_seq_restart = 1;
            ctl_seq_run = 1;
            bt_seq_owed = 0;
        } else if (req == 2u) {
            ctl_seq_run = 0;
        } else if (ui) {                             /* held: a leader's start waits for start_at, a follower's own
                                                      * PLAY / STOP is the leader's to decide */
            ctl_seq_run = bt_seq_was;
            ctl_seq_restart = 0;
        }
    } else {
        lead = !ctl_ext_quarter && bt_sync() != BT_SYNC_BLE && bt_sync() != BT_SYNC_LINK;
    }
    run = ctl_seq_run;
    bt_clock_block(CTL, (int)(run && lead), run && ctl_seq_restart);   /* (bt_midi.c: the BLE device follows) */
    bt_seq_was = run;
    if (!run) {
        bt_seq_owed = 0;
        return 1;                                    /* (stopped: the tick takes the requests) */
    }
    bt_seq_owed += (int32_t)seq_n - (int32_t)CTL;
    if (bt_seq_owed >= (int32_t)CTL) {
        bt_seq_owed -= (int32_t)CTL;
        return 2;
    }
    if (bt_seq_owed <= -(int32_t)CTL) {
        bt_seq_owed += (int32_t)CTL;
        return 0;
    }
    return 1;
}

/* core_fml_rx (the BLE unit's ble_service): a message from the other FM-1 and the tick it came at */
static void bt_fml_rx(const uint8_t *p, uint32_t len, uint32_t t)
{
    fm1_irq_off();
    fml_rx(&bt_fml, p, len, t);
    if (bt_fml.role == FML_FOLLOWER && bt_fml.l_valid && bt_fml.l_bpm >= 40u && bt_fml.l_bpm <= 240u &&
        bt_fml.l_bpm != ctl_tempo) {
        ctl_tempo = bt_fml.l_bpm;                    /* (SYNC LINK: the leader's tempo) */
        set.tempo = (uint8_t)bt_fml.l_bpm;
    }
    fm1_irq_on();
}

/* the main loop (bt_service): the role from SYNC, the messages out while linked */
static void bt_fml_frame(void)
{
    uint32_t role = bt_fml_role(), len;
    int linked;
    if (role != bt_fml.role) {
        fm1_irq_off();
        fml_init(&bt_fml, role);
        fm1_irq_on();
        bt_fo.len = 0;
        bt_fo.said = 0;
    }
    if (bt_fml.msg_follow) {
        bt_fml.msg_follow = 0;
        say("FOLLOWING LINK");                       /* (a linked follower's own PLAY / STOP: the leader's count) */
    }
    if (!ble_fml_ready()) {                          /* (FM-Link's own link, or the device's) */
        bt_fo.len = 0;
        return;
    }
    if (bt_fo.len && fm1_ms - bt_fo.ms > 50u)
        bt_fo.len = 0;
    for (;;) {
        if (!bt_fo.len) {
            fm1_irq_off();
            len = fml_poll(&bt_fml, fm1_ticks(), bt_fo.b);
            fm1_irq_on();
            if (!len)
                break;
            bt_fo.len = (uint8_t)len;
            bt_fo.ms = fm1_ms;
        }
        if (!ble_fml_send(bt_fo.b, bt_fo.len))
            break;                                   /* no room (or not an FM-1): this one first, next pass */
        bt_fo.len = 0;
    }
    linked = fml_linked(&bt_fml, fm1_ticks());
    if (linked && !bt_fo.said && role != FML_NONE)
        say(role == FML_LEADER ? "FM-LINK: LEADING" : "FM-LINK: FOLLOWING");
    bt_fo.said = (uint8_t)linked;
}
