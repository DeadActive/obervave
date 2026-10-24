/* SPDX-License-Identifier: GPL-3.0-only
 * BLE MIDI: 2026 DEADACTIVE (from Felucca's ble-kit smc.c, after Hortator's) */
/* FELUCCA_BLE (obervave.c includes it after bt_set.c, before bt_midi.c): the M-VAVE SMC-Mixer in its DAW (Mackie
 * Control) mode on OberVave's four tracks and its mixer (Felucca's layout, smc-seq's mapping checked on a unit).
 * In (bt_midi.c, SMC DAW mode):
 *   faders = pitch bend on channels 1..8: 1..4 the tracks' LEVEL, 5..8 the tracks' REVERB send, with pickup (9, the
 *     master: nothing);
 *   knobs 1..4 = CC 16..19 on channel 1, sign-magnitude clicks: the tracks' PAN; knobs 5..8 = CC 20..23: the selected
 *     track's four PERFORM parameters (HOME: CUTOFF RESO ENV AMT RELEASE);
 *   buttons = notes on channel 1, presses only: REC 0x00.. (the track selected, then REC as on the FM-1), SOLO 0x08..,
 *     MUTE 0x10.. (the mixer's, as GLO + keys 9-12 / 5-8), SELECT 0x18.. for strips 1..4 (5..8: nothing); STOP 0x5D,
 *     PLAY 0x5E, REC 0x5F; BANK / CHANNEL < > 0x2E 0x30 / 0x2F 0x31 the previous / next track.
 * Out (smc_frame, ble_midi_send): the LEDs as notes 127 / 0, only changes, all of them on a new link; each fader's
 *   position as E0+n v v, which the mixer blinks the fader's LED against. Main loop only. */
#define SMC_C 1
#define SMC_FADERS 8u
#define SMC_LEDS 35u                                  /* notes 0x00..0x1F, then 0x5D..0x5F */
typedef char smc_four_tracks[NTRK == 4u ? 1 : -1];
static uint32_t bt_mode_now(void);                    /* bt_midi.c */
static struct {
    uint32_t lit[2], known[2];      /* the LEDs as sent: [0] bit = note 0x00..0x1F (REC SOLO MUTE SELECT rows), [1]
                                     * bits 0..2 = STOP PLAY REC (0x5D..0x5F); known 0 = to send */
    uint8_t val[SMC_FADERS];        /* each fader's value as the fader set it or its position was sent */
    uint8_t last[SMC_FADERS];       /* each fader's last report (crossing; valid with its seen bit) */
    uint8_t up, on;                 /* linked; refreshed for SMC mode since the link (0: smc_fresh first) */
    uint8_t dark;                   /* SMC mode ended while linked: LEDs still to turn off (SMC_LEDS -> 0) */
    uint8_t picked, seen, pend;     /* bits per fader: picked up; last[] valid; position to send */
} smc;

/* fader n's value: track n's LEVEL (faders 1..4), track n - 4's REVERB send (5..8) */
static uint8_t *smc_fptr(uint32_t n) { return n < 4u ? &set.mix_lvl[n & 3u] : &set.mix_rev[n & 3u]; }
static uint32_t smc_fval(uint32_t n) { uint32_t v = *smc_fptr(n); return v > 127u ? 127u : v; }

/* a new link, or SMC mode again: every LED unknown, every fader unpicked, every position to send */
static void smc_fresh(void)
{
    uint32_t i;
    smc.known[0] = smc.known[1] = 0;
    smc.picked = smc.seen = 0;
    smc.pend = 0xFFu;
    smc.dark = 0;
    for (i = 0; i < SMC_FADERS; i++)
        smc.val[i] = (uint8_t)smc_fval(i);
    smc.on = 1;
}

/* fader n's value changed by other means (the FM-1's mixer, a pattern's mix): unpicked, the new position sent */
static void smc_notice(uint32_t n)
{
    uint32_t v = smc_fval(n);
    if (v != smc.val[n]) {
        smc.val[n] = (uint8_t)v;
        smc.picked &= (uint8_t)~(1u << n);
        smc.pend |= (uint8_t)(1u << n);
    }
}

/* fader n at v: an unpicked fader acts once within 2 of the value or across it (smc-seq's soft takeover) */
static void smc_fader(uint32_t n, uint32_t v)
{
    uint32_t b = 1u << n, s, crossed;
    smc_notice(n);
    s = smc.val[n];
    crossed = (smc.seen & b) && ((smc.last[n] < s) != (v < s));
    smc.last[n] = (uint8_t)v;                         /* every report: a later unpick crosses from here */
    smc.seen |= (uint8_t)b;
    if (!(smc.picked & b)) {
        if (!crossed && (v + 2u < s || v > s + 2u))
            return;
        smc.picked |= (uint8_t)b;
    }
    if (v == s)
        return;
    smc.val[n] = (uint8_t)v;
    smc.pend |= (uint8_t)b;                           /* echoed: the mixer's pickup LED follows (as smc-seq) */
    *smc_fptr(n) = (uint8_t)v;
    mix_apply();
    set_dirty = 1;
    set_t = fm1_ms;
    dirty_steps = 1;                                  /* (the mixer page's strip) */
}

static void smc_say(uint32_t t, const char *what, const char *val)   /* "B PAN L12", "A CUTOFF 64" */
{
    static char m[24];
    m[0] = TRK_L[t % NTRK];
    m[1] = 0;
    scat(m, " ");
    scat(m, what);
    scat(m, " ");
    scat(m, val);
    say(m);
}

/* knob k (0..3) turned delta clicks: track k's PAN */
static void smc_pan(uint32_t k, int32_t delta)
{
    char v[8];
    int32_t pn;
    set.mix_pan[k] = (int8_t)clampi((int32_t)set.mix_pan[k] + delta, -64, 64);
    mix_apply();
    set_dirty = 1;
    set_t = fm1_ms;
    pn = set.mix_pan[k];
    v[0] = 0;
    if (!pn)
        scat(v, "C");
    else {
        scat(v, pn < 0 ? "L" : "R");
        fmt_u(v + 1, (uint32_t)(pn < 0 ? -pn : pn));
    }
    smc_say(k, "PAN", v);
    dirty_page = dirty_steps = 1;
}

/* knob k (4..7) turned delta clicks: the selected track's PERFORM parameter k - 4, a value step a click (recorded as
 * a knob turned on the FM-1: auto_rec) */
static void smc_knob(uint32_t k, int32_t delta)
{
    int32_t p = PAGE[PG_PERFORM].p[k & 3u], step;
    char v[12];
    if (p < 0)
        return;
    step = PDEF[p].max >= 100 ? 2 : 1;
    set_param((uint32_t)p, (int32_t)ed_patch((uint32_t)p)->v[p] + delta * step);
    auto_rec((uint32_t)p);
    param_value(v, (uint32_t)p, ed_patch((uint32_t)p)->v[p]);
    smc_say(seq_trk, PDEF[p].name, v);
    dirty_page = dirty_hdr = 1;
}

static void smc_rec(void)                             /* REC pressed as on the FM-1 (app_main's REC tap) */
{
    if (rec)
        rec = REC_OFF;
    else if (ctl_seq_run) {
        rec = REC_ON;
        undo_save();
    } else
        rec = REC_READY;
    memset(rec_open, 0, sizeof rec_open);
    hints_key = 0xFFFFFFFFu;
    dirty_tags = dirty_steps = dirty_page = 1;
}

/* a button pressed (note n): the rows REC SOLO MUTE SELECT 0x00 0x08 0x10 0x18 + strip, the transport, track < > */
static void smc_button(uint32_t n)
{
    uint32_t k = n & 7u;
    if (n < 0x20u) {
        if (k >= NTRK)                                /* strips 5..8: nothing */
            return;
        if (n < 0x08u) {                              /* REC n: that track, then REC as on the FM-1 */
            trk_select(k);
            smc_rec();
        } else if (n < 0x10u) {
            mix_solo(k);
        } else if (n < 0x18u) {
            mix_mute(k);
        } else {
            trk_select(k);
        }
        dirty_steps = dirty_page = 1;
        return;
    }
    switch (n) {
    case 0x5Eu:                                       /* PLAY: stopped -> play (as the FM-1's PLAY) */
        if (!ctl_seq_run) {
            if (rec == REC_READY) {
                rec = REC_ON;
                undo_save();
                ctl_count = set.click % NCLICK == CLICK_COUNT;
            }
            ctl_seq_restart = 1;
            ctl_seq_run = 1;
            dirty_tags = dirty_steps = 1;
        }
        break;
    case 0x5Du:                                       /* STOP: playing -> stop (a take ends with it) */
        if (ctl_seq_run) {
            ctl_seq_run = 0;
            rec = REC_OFF;
            rec_wait = 0;
            dirty_tags = dirty_steps = 1;
        }
        break;
    case 0x5Fu:                                       /* REC: the selected track */
        smc_rec();
        break;
    case 0x2Eu:
    case 0x30u:                                       /* BANK / CHANNEL <: the previous track (wrapping) */
        trk_select((seq_trk + NTRK - 1u) % NTRK);
        break;
    case 0x2Fu:
    case 0x31u:                                       /* BANK / CHANNEL >: the next */
        trk_select((seq_trk + 1u) % NTRK);
        break;
    default:                                          /* REW FF, the arrows: nothing */
        break;
    }
}

/* bt_midi_in in SMC mode: one USB-MIDI packet (CIN | status << 8 | d1 << 16 | d2 << 24) */
static void smc_in(uint32_t pkt)
{
    uint32_t st = (pkt >> 8) & 0xFFu, ch = st & 15u, d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu;
    if (!smc.on)
        smc_fresh();
    switch (st >> 4) {
    case 0xEu:                                        /* fader ch + 1 (9: the master, ignored) */
        if (ch < SMC_FADERS)
            smc_fader(ch, (d2 << 7 | d1) >> 7);
        break;
    case 0xBu:                                        /* knobs 1..4 PAN, 5..8 the selected track's; bit 6 = ccw */
        if (!ch && d1 >= 16u && d1 <= 23u && (d2 & 0x3Fu)) {
            int32_t clicks = d2 & 0x40u ? -(int32_t)(d2 & 0x3Fu) : (int32_t)(d2 & 0x3Fu);
            if (d1 <= 19u)
                smc_pan(d1 - 16u, clicks);
            else
                smc_knob(d1 - 20u, clicks);
        }
        break;
    case 0x9u:                                        /* velocity 0 = a release */
        if (!ch && d2)
            smc_button(d1);
        break;
    default:                                          /* note-offs (releases), the rest: nothing */
        break;
    }
}

/* core_ble_link: the link up / down (bt_link first: the mode). Up: everything sent again, the faders unpicked */
static void smc_link(int up)
{
    smc.up = (uint8_t)(up != 0);
    smc.on = smc.dark = 0;
}

static int smc_send(uint32_t a, uint32_t b, uint32_t c)   /* 1 = queued (ble_midi_send); 0: try again next frame */
{
    uint8_t m[3];
    m[0] = (uint8_t)a;
    m[1] = (uint8_t)b;
    m[2] = (uint8_t)c;
    return ble_midi_room() && ble_midi_send(m, 3u) == 1;   /* only what fits: out_drops = real losses */
}

/* bt_service, after ble_service: linked in SMC mode, the LED changes and the fader positions waiting; a refused
 * message stays waiting. Nothing changed: no message. */
static void smc_frame(void)
{
    uint32_t w[2], i, j, b;
    if (!smc.up || !bt_mode_now()) {
        if (smc.on && smc.up)                         /* SMC mode ended while linked: the LEDs off once */
            smc.dark = SMC_LEDS;
        smc.on = 0;                                   /* SMC mode again: everything again */
        for (; smc.dark; smc.dark--)                  /* notes 0x00..0x1F, 0x5D..0x5F at 0 */
            if (!smc_send(0x90u, SMC_LEDS - smc.dark < 32u ? SMC_LEDS - smc.dark : 0x5Du + 3u - smc.dark, 0))
                return;
        return;
    }
    if (!smc.on)
        smc_fresh();
    for (i = 0; i < SMC_FADERS; i++)
        smc_notice(i);
    w[0] = (rec == REC_ON ? 1u << (seq_trk & 3u) : 0u) | (uint32_t)(set.mix_solo & 15u) << 8 |
           (uint32_t)(set.mix_mute & 15u) << 16 | 1u << (24u + (seq_trk & 3u));
    w[1] = (ctl_seq_run ? 2u : 1u) | (rec ? 4u : 0u);
    for (j = 0; j < 2u; j++) {
        uint32_t dirty = ((w[j] ^ smc.lit[j]) | ~smc.known[j]) & (j ? 7u : 0xFFFFFFFFu);
        for (i = 0; dirty && i < 32u; i++) {
            b = 1u << i;
            if (!(dirty & b))
                continue;
            if (!smc_send(0x90u, j ? 0x5Du + i : i, (w[j] & b) ? 127u : 0u))
                return;
            smc.lit[j] = (smc.lit[j] & ~b) | (w[j] & b);
            smc.known[j] |= b;
            dirty &= ~b;
        }
    }
    for (i = 0; smc.pend && i < SMC_FADERS; i++)
        if ((smc.pend >> i) & 1u) {
            if (!smc_send(0xE0u + i, smc.val[i], smc.val[i]))
                return;
            smc.pend &= (uint8_t)~(1u << i);
        }
}
