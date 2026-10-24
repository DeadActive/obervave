/* SPDX-License-Identifier: GPL-3.0-only
 * FM-Link: 2026 DEADACTIVE (Felucca's tests/bt_fml_test.c on OberVave's harness) */
/* FM-Link's glue (firmware/src/bt_fml.c) on the host: this FM-1's core (its sequencer through bt_seq_ticks and
 * seq_tick, as the audio interrupt runs them each control block; its main loop's bt_fml_frame, core_fml_rx) against
 * another FM-1 that is the engine alone (fmlink.c), its own crystal and tick count, joined by a link that delivers at
 * 15 ms connection events. The engine's accuracy over a bad link is tests/fmlink_test.c's; here: the roles from SYNC
 * (INTERNAL and AUTO without a clock lead, LINK follows, BLE none), PLAY / STOP / tempo through the panel's transport
 * (ctl_seq_run), the follower's tempo from the leader, its own PLAY / STOP held, the sequencer itself in time with
 * FM-Link's count (whole control blocks: within CTL frames), LINK without a link as INTERNAL, a message kept while the
 * BLE unit has no room. */
#include "bt_test.h"

/* the link: packets with when they arrive (real time = this FM-1's ticks), to the peer (1) or to this FM-1 (0) */
#define EV (15u * 24000u)                            /* the connection interval, ticks */
typedef struct { uint32_t at; int to_peer; uint8_t len, b[20]; } pk_t;
static pk_t Q[512];
static uint32_t qn, link_down;
static void link_put(int to_peer, const uint8_t *b, uint32_t len)
{
    if (link_down || qn >= 512u)
        return;
    Q[qn].at = (host_ticks / EV + 1u) * EV + 12000u;   /* the next connection event, then half a millisecond */
    Q[qn].to_peer = to_peer;
    Q[qn].len = (uint8_t)len;
    memcpy(Q[qn].b, b, len);
    qn++;
}
/* the other FM-1: the engine, its ticks = this one's + an offset, 40 ppm faster */
static struct { fml_t f; uint32_t off, bpm, playing, next, req, spb; } P;
static uint32_t peer_clk(void) { return host_ticks + P.off + (uint32_t)((uint64_t)host_ticks * 40u / 1000000u); }
static int said_lead, said_follow;
static uint32_t fml_room = 1000000u, fml_sent, blocks;
static int bt_send_hook(const uint8_t *p, uint32_t len)   /* (bt_host.h's ble_fml_send records; the link carries) */
{
    if (!fml_room)
        return 0;
    fml_room--;
    fml_sent++;
    link_put(1, p, len);
    return 1;
}
static uint64_t seq_pos;                             /* this FM-1's sequencer: beats (Q16) played since its start */
static void step(void)                               /* one control block on this FM-1, the peer's blocks, the loops */
{
    uint32_t i, k = 0, len, seq_n, nt;
    uint8_t b[20];
    host_ticks += CTL * 544u;
    fm1_ms = 0;                                      /* (fm1_ticks = host_ticks: fm1_ms * 24000 + host_ticks) */
    for (i = 0; i < qn; i++) {
        if ((int32_t)(host_ticks - Q[i].at) >= 0) {
            if (Q[i].to_peer)
                fml_rx(&P.f, Q[i].b, Q[i].len, peer_clk());
            else
                bt_fml_rx(Q[i].b, Q[i].len, host_ticks);   /* (core_fml_rx) */
        } else {
            Q[k++] = Q[i];
        }
    }
    qn = k;
    bt_fml_heard = host_ticks + 256u * 544u;         /* (platform.c: the half rendered 256 frames ahead) */
    nt = bt_seq_ticks(0);
    for (i = 0; i < nt; i++)
        seq_tick();
    if (ctl_seq_run) {
        static uint64_t acc;                         /* (the remainder: no drift from the division) */
        acc += (uint64_t)nt * CTL * 65536u;
        seq_pos += acc / quarter_len();
        acc %= quarter_len();
    } else
        seq_pos = 0;
    while ((int32_t)(peer_clk() + 256u * 544u - P.next) >= 0) {
        uint32_t bpm = P.f.role == FML_FOLLOWER && P.f.l_valid ? P.f.l_bpm : P.bpm;
        P.spb = (60u * FS / bpm + 24u) / 48u * 48u;   /* (an OberVave too: its quarter rounded to 48 frames) */
        i = fml_block(&P.f, P.next, 32, bpm, P.spb, (int)P.playing, P.req, &seq_n);
        P.req = 0;
        if (i == 1u || i == 3u)
            P.playing = 1;
        else if (i == 2u)
            P.playing = 0;
        P.next += 32u * 544u;
    }
    if (++blocks % 16u == 0u) {                      /* (the main loop: every ~6 ms) */
        bs_fml_ready = !link_down;
        bt_fml_frame();                              /* (bt_service) */
        bs_fml_ready = 1;
    }
    while ((len = fml_poll(&P.f, peer_clk(), b)) != 0u)
        link_put(0, b, len);
    said_lead |= toast && str_eq(toast, "FM-LINK: LEADING");
    said_follow |= toast && str_eq(toast, "FM-LINK: FOLLOWING");
}
static void run_ms(uint32_t ms)
{
    uint32_t end = host_ticks + ms * 24000u;
    while ((int32_t)(host_ticks - end) < 0)
        step();
}
/* |this FM-1's beats - the peer's| at this moment, in ms (each from its last block) */
static double apart_ms(void)
{
    double q = (double)quarter_len();
    double a = (double)bt_fml.last_beats / 65536.0 + (double)(int32_t)(host_ticks - bt_fml.last_t) / (544.0 * q);
    double b = (double)P.f.last_beats / 65536.0 + (double)(int32_t)(peer_clk() - P.f.last_t) / (544.0 * P.spb);
    double d = (a - b) * 60000.0 / (double)ctl_tempo;
    return d < 0 ? -d : d;
}
/* the sequencer itself against FM-Link's count (frames at the tempo now): whole control blocks, so within CTL */
static int32_t seq_off(void) { return (int32_t)(((int64_t)seq_pos - (int64_t)bt_fml.beats) * (int64_t)quarter_len() / 65536); }
static double worst_ms(uint32_t ms, int32_t *seq_worst)
{
    double w = 0, d;
    uint32_t i;
    for (i = 0; i < ms / 10u; i++) {
        int32_t s;
        run_ms(10);
        d = apart_ms();
        w = d > w ? d : w;
        s = seq_off();
        s = s < 0 ? -s : s;
        if (s > *seq_worst)
            *seq_worst = s;
    }
    return w;
}
static void fresh(uint32_t sync, uint32_t peer_role)  /* power-on, MIDI mode, linked to the other FM-1 */
{
    bt_set_t s;
    bt_fresh(0);
    memset(&s, 0, sizeof s);
    s.mode = BT_MODE_MIDI;
    bt_set_put(&s);
    bt_sync_set(sync);
    memset(&bt_fml, 0, sizeof bt_fml);
    memset(&bt_fo, 0, sizeof bt_fo);
    ctl_seq_run = ctl_seq_restart = 0;
    bt_seq_was = 0;
    bt_seq_owed = 0;
    ctl_ext_quarter = 0;
    seq_pos = 0;
    qn = 0;
    link_down = 0;
    fml_room = 1000000u;
    host_ticks = 123456789u;
    ctl_tempo = set.tempo = 97;
    memset(&P, 0, sizeof P);
    fml_init(&P.f, peer_role);
    P.off = 0x9E3779B9u;
    P.bpm = 120;
    P.next = peer_clk() + 256u * 544u;
    bt_cpy(conn_name, "Felucca FM-1", sizeof conn_name);
    conn_up = 1;
    bs_fml_ready = 1;
    bt_link(1);
    said_lead = said_follow = 0;
    toast = 0;
}
static void play(void) { ctl_seq_restart = 1; ctl_seq_run = 1; }   /* the panel's PLAY (app_main) */
static void stop(void) { ctl_seq_run = 0; }

int main(void)
{
    double e;
    int32_t sw = 0;
    uint32_t i;
    bt_start();
    fml_send_hook = bt_send_hook;
    /* this FM-1 follows (SYNC LINK), the other leads */
    fresh(BT_SYNC_LINK, FML_LEADER);
    run_ms(3000);
    ok("SYNC LINK: the follower's role", bt_fml.role == FML_FOLLOWER);
    ok("the follower hears the leader and knows the offset", fml_linked(&bt_fml, host_ticks) && bt_fml.off_ok);
    ok("it says FM-LINK: FOLLOWING", said_follow && !said_lead);
    P.req = 1;                                       /* PLAY on the leader */
    run_ms(5);
    ok("the leader announces its start (both 80 ms on)", !P.playing && !ctl_seq_run);
    run_ms(300);
    ok("both play", P.playing && ctl_seq_run);
    ok("the follower's tempo is the leader's (from 97)", ctl_tempo == 120u && set.tempo == 120u);
    e = worst_ms(20000, &sw);
    printf("      FM-Link's count against the leader: %.3f ms at most over 20 s; the sequencer within %d frames of "
           "it\n", e, sw);
    ok("within 1.5 ms over 20 s, still playing", e <= 1.5 && ctl_seq_run);
    ok("the sequencer itself within a control block (16 frames) of FM-Link's count", sw <= (int32_t)CTL);
    stop();                                          /* its own STOP */
    run_ms(50);
    ok("a linked follower's own STOP does nothing and says FOLLOWING LINK", ctl_seq_run && toast &&
       str_eq(toast, "FOLLOWING LINK"));
    P.bpm = 133;                                     /* the leader's tempo changes */
    run_ms(1000);
    ok("the follower takes the leader's new tempo", ctl_tempo == 133u);
    sw = 0;
    e = worst_ms(5000, &sw);
    printf("      at the new tempo: %.3f ms, the sequencer within %d frames\n", e, sw);
    ok("in time at the new tempo", e <= 1.5 && sw <= (int32_t)CTL);
    link_down = 1;                                   /* the link lost: it plays on */
    run_ms(3000);
    ok("the link lost: the follower plays on at the leader's last tempo", ctl_seq_run && ctl_tempo == 133u);
    link_down = 0;
    run_ms(3000);
    P.req = 2;                                       /* STOP on the leader */
    run_ms(200);
    ok("STOP on the leader stops the follower", !P.playing && !ctl_seq_run);

    /* this FM-1 leads (SYNC INTERNAL), the other follows */
    fresh(BT_SYNC_INT, FML_FOLLOWER);
    ctl_tempo = set.tempo = 110;
    run_ms(3000);
    ok("SYNC INTERNAL: the leader's role, FM-LINK: LEADING", bt_fml.role == FML_LEADER && said_lead);
    play();
    run_ms(5);
    ok("PLAY with a follower linked: not at once", !ctl_seq_run);
    run_ms(300);
    ok("both play, the follower at the leader's tempo", ctl_seq_run && P.playing && P.f.l_bpm == 110u);
    sw = 0;
    e = worst_ms(10000, &sw);
    printf("      the follower against the core's sequencer: %.3f ms at most over 10 s\n", e);
    ok("within 1.5 ms", e <= 1.5 && sw <= (int32_t)CTL);
    stop();
    run_ms(200);
    ok("STOP stops both", !ctl_seq_run && !P.playing);
    fml_room = 0;                                    /* the BLE unit has no room: a message is kept, not lost */
    for (i = 0; i < 600u && !bt_fo.len; i++)         /* (until one is due: TIME every 100..150 ms) */
        run_ms(1);
    ok("no room: one message waits", bt_fo.len != 0u);
    fml_sent = 0;
    fml_room = 1000000u;
    run_ms(8);
    ok("room again: it goes first", fml_sent != 0u && bt_fo.len == 0u);
    link_down = 1;                                   /* the follower gone: PLAY at once again */
    run_ms(2500);
    play();
    run_ms(2);
    ok("no follower: PLAY starts at once", ctl_seq_run);

    /* SYNC AUTO without a MIDI clock leads too; with one coming in, no part */
    fresh(BT_SYNC_AUTO, FML_FOLLOWER);
    run_ms(100);
    ok("SYNC AUTO, no MIDI clock: the leader's role", bt_fml.role == FML_LEADER);
    ctl_ext_quarter = 22080;
    run_ms(100);
    ok("SYNC AUTO following a MIDI clock: no FM-Link role", bt_fml.role == FML_NONE);

    /* SYNC LINK without a link: as INTERNAL */
    fresh(BT_SYNC_LINK, FML_LEADER);
    link_down = 1;
    run_ms(100);
    play();
    run_ms(2);
    ok("SYNC LINK, no other FM-1: PLAY starts at once, at its own tempo", ctl_seq_run && ctl_tempo == 97u);
    run_ms(2000);
    ok("... and keeps playing", ctl_seq_run);

    /* SYNC BLE: no part in FM-Link */
    fresh(BT_SYNC_BLE, FML_LEADER);
    run_ms(100);
    ok("SYNC BLE: no FM-Link role", bt_fml.role == FML_NONE);
    printf(fails ? "bt_fml: %d FAILED\n" : "bt_fml: all checks passed\n", fails);
    return fails != 0;
}
