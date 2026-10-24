/* SPDX-License-Identifier: GPL-3.0-only
 * BLE MIDI: 2026 DEADACTIVE (from Felucca's ble-kit bt_midi.c, after Hortator's)
 * FM-Link: 2026 DEADACTIVE */
/* FELUCCA_BLE (obervave.c includes it after bt_set.c and smc.c): the core's side of Bluetooth MIDI on OberVave.
 * In: each USB-MIDI packet the BLE unit received goes to the SMC-Mixer layout (SMC DAW mode, smc.c) or, in MIDI mode,
 *   into a ring of its own (USB and TRS share midi_in_q, which carries no source), which midi_in reads after
 *   midi_in_q: ordinary MIDI from there (notes by channel, bend, sustain, CCs, program change) and the realtime bytes
 *   with SYNC AUTO or BLE (bt_set.c bt_clock_ok: SYNC BLE follows this device's only).
 * Out, MIDI mode: what the keys send to USB MIDI out (on the track's channel) also goes to the connected device
 *   (bt_keys_send). Queued here (one producer at a time: the main loop with interrupts off, or the audio interrupt's
 *   clock), handed to the BLE unit by the main loop.
 * Clock out: while OberVave runs on its own tempo (SYNC INTERNAL, or AUTO with no MIDI clock coming in) and is linked in
 *   MIDI mode it is the master of the BLE device (another FM-1 on SYNC BLE, a computer): Start on PLAY (and when the
 *   sequencer starts again from the top), 24 clock pulses a beat counted from the audio's own samples (so they never
 *   drift from its steps), Stop on STOP. A unit following a clock itself sends none: no loop. bt_clock_block, from
 *   bt_fml.c bt_seq_ticks (the audio ISR, every block).
 * FM-LINK mode: no MIDI either way (received packets dropped, nothing sent, no clock out): the link carries FM-Link
 *   only (bt_fml.c runs in every mode).
 * The link down: what the device held is released (CC 64 = 0 and CC 123 on each channel it played), as a pulled cable
 *   would otherwise leave its notes sounding; BT_REL_MS after the drop, so a note-on it sent just before has reached
 *   the engine first (OberVave's engine takes CC 123, ctl_panic, before the notes queued in the same block). */
#define BT_MIDI_C 1
#define BT_OUT_Q 32u
#define BT_RX_Q 32u
#define BT_REL_MS 15u                                /* the release after a drop: two audio halves on */
static struct {
    uint32_t drops;                                  /* MIDI mode: packets refused, the ring full */
    uint32_t out_drops;                              /* MIDI out: messages lost to a full queue here */
    uint16_t held, release;                          /* channels the device played / still to be released */
    uint32_t rel_ms;                                 /* when the link went down (the release waits BT_REL_MS) */
    uint8_t smc;                                     /* the linked device's name starts with "SMC" (bt_link) */
    uint8_t up;                                      /* linked (subscribed to the device's BLE-MIDI) */
    volatile uint8_t out_on;                         /* MIDI out wanted: linked, MIDI mode (bt_midi_frame) */
} bt_in;
static uint32_t bt_out_q[BT_OUT_Q];
static volatile uint32_t bt_out_w, bt_out_r;
static uint32_t bt_rx_q[BT_RX_Q], bt_rx_w, bt_rx_r;  /* the device's MIDI for midi_in (main loop both sides) */
static void smc_in(uint32_t pkt);                    /* smc.c */

/* 1 = the SMC-Mixer DAW layout, 0 = MIDI: MODE, or AUTO by the connected device's name */
static uint32_t bt_mode_now(void)
{
    bt_set_t s;
    bt_set_get(&s);
    if (s.mode != BT_MODE_AUTO)
        return s.mode == BT_MODE_SMC;
    return bt_in.smc;
}
static int bt_link_only(void)                        /* MODE FM-LINK: no MIDI either way */
{
    bt_set_t s;
    bt_set_get(&s);
    return s.mode == BT_MODE_LINK;
}

/* core_ble_link: the link up (the connected device's name read once) / down */
static void bt_link(int up)
{
    char n[24];
    bt_in.up = (uint8_t)(up != 0);
    bt_in.smc = (uint8_t)(up && ble_ui_conn_name(n) && n[0] == 'S' && n[1] == 'M' && n[2] == 'C');
    bt_in.out_on = (uint8_t)(up && !bt_mode_now() && !bt_link_only());   /* (at once: bt_midi_frame follows MODE from
                                                                           * then on) */
    if (!up) {
        bt_in.release |= bt_in.held;                 /* (bt_midi_frame: into the ring when it has room) */
        bt_in.held = 0;
        bt_in.rel_ms = fm1_ms;
        bt_out_r = bt_out_w;                         /* nothing queued for a device that is gone */
    }
}

/* the realtime F8 FA FB FC (as their status), channel voice with CIN = the status' high nibble and data bytes < 0x80
 * (Cx Dx: one); 0 = dropped (as usb.c's filter) */
static uint32_t bt_midi_filter(uint32_t pkt)
{
    uint32_t cin = pkt & 15u, st = (pkt >> 8) & 0xFFu;
    if (cin == 0xFu)
        return st == 0xF8u || st == 0xFAu || st == 0xFBu || st == 0xFCu ? 0xFu | st << 8 : 0u;
    if (cin >= 8u && cin <= 0xEu && (st >> 4) == cin && !((pkt >> 16) & 0x80u) &&
        ((cin == 0xCu || cin == 0xDu) || !(pkt & 0x80000000u)))
        return pkt;
    return 0;
}

static int bt_rx_put(uint32_t pkt)                   /* 0 = the ring is full */
{
    if (bt_rx_w - bt_rx_r >= BT_RX_Q)
        return 0;
    bt_rx_q[bt_rx_w % BT_RX_Q] = pkt;
    bt_rx_w++;
    return 1;
}
/* midi_in: the next message from the BLE device (after USB / TRS's); 1 = one */
static int bt_rx_get(uint8_t *st, uint8_t *d1, uint8_t *d2)
{
    uint32_t pk;
    if (bt_rx_r == bt_rx_w)
        return 0;
    pk = bt_rx_q[bt_rx_r % BT_RX_Q];
    bt_rx_r++;
    *st = (uint8_t)(pk >> 8);
    *d1 = (uint8_t)(pk >> 16);
    *d2 = (uint8_t)(pk >> 24);
    return 1;
}

static void bt_midi_in(uint32_t pkt)
{
    uint32_t cin, st;
    if (bt_link_only())
        return;                                      /* FM-LINK: MIDI from the device ignored */
    if (bt_mode_now()) {
        smc_in(pkt);
        return;
    }
    pkt = bt_midi_filter(pkt);
    if (!pkt)
        return;
    if (!bt_rx_put(pkt)) {
        bt_in.drops++;
        return;
    }
    cin = pkt & 15u;
    st = (pkt >> 8) & 0xFFu;
    if ((cin == 9u && (pkt >> 24)) || (cin == 0xBu && ((pkt >> 16) & 0xFFu) == 64u && (pkt >> 24)))
        bt_in.held |= (uint16_t)(1u << (st & 15u));  /* a note-on or the pedal down: this channel has something held */
}

static void bt_out_push(uint32_t pkt)               /* the out ring (one producer at a time: the audio ISR, or with
                                                     * interrupts off). Full: dropped and counted */
{
    if (bt_out_w - bt_out_r >= BT_OUT_Q) {
        bt_in.out_drops++;
        return;
    }
    bt_out_q[bt_out_w % BT_OUT_Q] = pkt;
    RING_PUBLISH();
    bt_out_w++;
}

/* clock out (the audio ISR, every block, bt_fml.c): run = the sequencer plays on OberVave's own tempo; restart = it
 * starts again from the top while playing */
static struct { uint8_t run; uint32_t acc; } bt_clk;  /* acc: samples since the last pulse, x 24 */
static void bt_clock_block(uint32_t n, int run, int restart)
{
    uint32_t beat;
    run = run && bt_in.out_on;
    if (!run) {
        if (bt_clk.run && bt_in.out_on)
            bt_out_push(0xFu | 0xFCu << 8);         /* Stop */
        bt_clk.run = 0;
        return;
    }
    if (!bt_clk.run || restart) {
        bt_out_push(0xFu | 0xFAu << 8);             /* Start, then the first beat's pulse */
        bt_out_push(0xFu | 0xF8u << 8);
        bt_clk.run = 1;
        bt_clk.acc = 0;
    }
    bt_clk.acc += n * 24u;
    beat = quarter_len();                           /* samples a beat at OberVave's tempo (obv_engine.h) */
    while (beat && bt_clk.acc >= beat) {
        bt_clk.acc -= beat;
        bt_out_push(0xFu | 0xF8u << 8);
    }
}

/* the keys' MIDI out (keys_in, the main loop): to USB as ever, and to the BLE device in MIDI mode */
static void bt_keys_send(uint32_t st, uint32_t d1, uint32_t d2)
{
    midi_send(st, d1, d2);
    if (!bt_in.out_on)
        return;
    fm1_irq_off();                                   /* (the audio interrupt's clock pushes too) */
    bt_out_push((st >> 4) | st << 8 | (d1 & 0x7Fu) << 16 | (d2 & 0x7Fu) << 24);
    fm1_irq_on();
}

/* the main loop (bt_service): the releases owed after a dropped link, then the out queue to the BLE unit while it
 * has room (ble_midi_send: main loop only) */
static void bt_midi_frame(void)
{
    uint32_t ch;
    for (ch = 0; ch < 16u && bt_in.release && fm1_ms - bt_in.rel_ms >= BT_REL_MS; ch++) {
        if (!((bt_in.release >> ch) & 1u))
            continue;
        if (BT_RX_Q - (bt_rx_w - bt_rx_r) < 2u)
            break;                                   /* still owed: the next pass */
        (void)bt_rx_put(0x0Bu | (0xB0u | ch) << 8 | 64u << 16);       /* the pedal up, All Notes Off */
        (void)bt_rx_put(0x0Bu | (0xB0u | ch) << 8 | 123u << 16);
        bt_in.release &= (uint16_t)~(1u << ch);
    }
    bt_in.out_on = (uint8_t)(bt_in.up && !bt_mode_now() && !bt_link_only());
    while (bt_out_r != bt_out_w && ble_midi_room()) {
        uint32_t pkt = bt_out_q[bt_out_r % BT_OUT_Q], cin = pkt & 15u;
        uint8_t m[3] = {(uint8_t)(pkt >> 8), (uint8_t)(pkt >> 16), (uint8_t)(pkt >> 24)};
        if (!ble_midi_send(m, cin == 0xFu ? 1u : cin == 0xCu || cin == 0xDu ? 2u : 3u))
            break;
        bt_out_r++;
    }
}
