/* SPDX-License-Identifier: GPL-3.0-only */
/* OberVave: one 8-voice analog-style polysynth for the M-VAVE FM-1, in the spirit of the classic American
 * poly synths of 1979-84. Built on Dinghy's platform (platform.c: boot, USB, updates, audio ISR, settings).
 *
 *   obv_dsp.h       oscillators (polyBLEP events), filter (trapezoidal SVF), tanh
 *   obv_params.h    the parameter table: patches, pages, MIDI CC
 *   obv_engine.h    the voices, envelopes, LFO, arpeggiator: all in the audio ISR
 *   obv_patches.h   the factory programs
 *   this file       the panel, MIDI, the screen, the storage
 *
 * Integers only (no FPU, no 64-bit division); tests/host.c runs this on a computer. */
#include "platform.c"
#include "obv_dsp.h"
#include "obv_params.h"
#include "obv_engine.h"
#include "obv_patches.h"

static void app_audio(int32_t *out, uint32_t n)
{
#ifndef OBV_HOST
    uint32_t t0 = fm1_ticks();
#endif
    engine_audio(out, n);
#ifndef OBV_HOST
    ui_render_us = (fm1_ticks() - t0) / FM1_TICKS_PER_US;
#endif
}

/* ---- what survives a power-off: the user programs and the globals, one settings sector */
#define SET_VERSION 1u
static struct {
    uint8_t version, prog, tempo, midi_ch;      /* midi_ch 0 = omni, else 1..16 */
    int8_t tune, oct, pad[2];
    uint32_t used;                              /* bit n: user program n saved */
    patch_t user[NPROG];
} set;
static uint32_t set_dirty, set_t;

static uint32_t prog, edited, page = PG_PERFORM, hold, save_armed, save_t, quick_uni_prev = 255;
static uint8_t cc_map[128];                     /* MIDI CC -> parameter + 1, 0 = none */

static void load_program(uint32_t n)
{
    patch_t p;
    n %= NPROG;
    if (set.used >> n & 1u) p = set.user[n];
    else patch_from_factory(&p, &FACTORY[n]);
    live = p;
    prog = n;
    edited = 0;
    quick_uni_prev = 255;
}

static void cc_map_init(void)
{
    uint32_t i, next = 20;
    for (i = 0; i < NPARAM; i++)
        if (PDEF[i].cc) cc_map[PDEF[i].cc] = (uint8_t)(i + 1u);
    for (i = 0; i < NPARAM; i++) {
        if (PDEF[i].cc) continue;
        while (next < 120u && (cc_map[next] || next == 64u)) next++;
        if (next < 120u) cc_map[next++] = (uint8_t)(i + 1u);
    }
}

static void set_param(uint32_t p, int32_t v)
{
    v = clampi(v, 0, PDEF[p].max);
    if (live.v[p] == v) return;
    live.v[p] = (uint8_t)v;
    edited = 1;
}

/* REC: a new sound within useful ranges */
static uint32_t rnd;
static uint32_t rn(uint32_t lo, uint32_t hi) { return lo + xorshift(&rnd) % (hi - lo + 1u); }
static void randomize(void)
{
    uint32_t i, r;
    for (i = 0; i < NPARAM; i++) live.v[i] = PDEF[i].def;
    live.v[P_O1_WAVE] = (uint8_t)rn(1, 3);
    live.v[P_O2_WAVE] = (uint8_t)rn(0, 3);
    live.v[P_O1_PW] = (uint8_t)rn(10, 110);
    live.v[P_O2_PW] = (uint8_t)rn(10, 110);
    live.v[P_O2_LVL] = (uint8_t)rn(60, 127);
    r = rn(0, 9);
    live.v[P_O2_SEMI] = (uint8_t)(r < 4 ? 24 : r < 6 ? 12 : r < 8 ? 36 : r == 8 ? 31 : 43);
    live.v[P_O2_FINE] = (uint8_t)rn(60, 76);
    live.v[P_NOISE] = (uint8_t)(rn(0, 5) ? 0 : rn(10, 70));
    live.v[P_SYNC] = (uint8_t)(rn(0, 4) == 0);
    live.v[P_XMOD] = (uint8_t)(rn(0, 4) ? 0 : rn(20, 100));
    live.v[P_ENV_O2] = (uint8_t)(live.v[P_SYNC] ? rn(70, 110) : 64);
    live.v[P_CUTOFF] = (uint8_t)rn(30, 110);
    live.v[P_RESO] = (uint8_t)rn(0, 90);
    live.v[P_FENV_AMT] = (uint8_t)rn(60, 120);
    live.v[P_FTYPE] = (uint8_t)(rn(0, 2) ? F_LP24 : rn(0, 4));
    live.v[P_DRIVE] = (uint8_t)(rn(0, 2) ? 0 : rn(10, 70));
    live.v[P_FA] = (uint8_t)rn(0, 90); live.v[P_FD] = (uint8_t)rn(30, 100); live.v[P_FS] = (uint8_t)rn(0, 100); live.v[P_FR] = (uint8_t)rn(20, 100);
    live.v[P_AA] = (uint8_t)rn(0, 80); live.v[P_AD] = (uint8_t)rn(30, 100); live.v[P_AS] = (uint8_t)rn(0, 127); live.v[P_AR] = (uint8_t)rn(20, 100);
    live.v[P_LFO_SHAPE] = (uint8_t)rn(0, 5);
    live.v[P_LFO_RATE] = (uint8_t)rn(20, 100);
    live.v[P_LFO_PITCH] = (uint8_t)(rn(0, 2) ? 0 : rn(5, 40));
    live.v[P_LFO_PWM] = (uint8_t)(rn(0, 1) ? 0 : rn(20, 90));
    live.v[P_LFO_FLT] = (uint8_t)(rn(0, 2) ? 0 : rn(10, 70));
    live.v[P_LFO_DELAY] = (uint8_t)rn(0, 70);
    live.v[P_LFO_PHASE] = (uint8_t)rn(0, 2);
    r = rn(0, 9);
    live.v[P_MODE] = (uint8_t)(r < 6 ? M_POLY : r < 7 ? M_DUAL : r < 9 ? M_UNISON : M_MONO);
    live.v[P_UNI_N] = (uint8_t)rn(2, 6);
    live.v[P_UNI_DET] = (uint8_t)rn(10, 80);
    live.v[P_GLIDE] = (uint8_t)(live.v[P_MODE] >= M_UNISON && rn(0, 1) ? rn(5, 40) : 0);
    live.v[P_VINTAGE] = (uint8_t)rn(0, 100);
    live.v[P_SPREAD] = (uint8_t)rn(20, 127);
    edited = 1;
}

/* ---- the screen (obv_ui.h draws; this decides what and when) */
#include "obv_ui.h"

static void fmt_u(char *s, uint32_t v)          /* decimal, no leading zeros */
{
    char b[12];
    uint32_t i = 0, j = 0;
    do { b[i++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (i) s[j++] = b[--i];
    s[j] = 0;
}
static uint32_t slen(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }
static void scat(char *d, const char *s) { d += slen(d); while ((*d++ = *s++)) ; }

static void fmt_ms(char *s, uint32_t ms_x10)
{
    char t[4];
    if (ms_x10 < 1000u) { fmt_u(s, ms_x10 / 10u); scat(s, "."); fmt_u(t, ms_x10 % 10u); scat(s, t); scat(s, "ms"); }
    else if (ms_x10 < 10000u) { fmt_u(s, ms_x10 / 10u); scat(s, "ms"); }
    else { fmt_u(s, ms_x10 / 10000u); scat(s, "."); fmt_u(t, (ms_x10 / 1000u) % 10u); scat(s, t); scat(s, "s"); }
}

static void param_value(char *s, uint32_t p, uint32_t v)   /* <= 7 characters */
{
    char t[8];
    s[0] = 0;
    switch (PDEF[p].kind) {
    case K_BIP: if (v < 64u) { scat(s, "-"); fmt_u(t, 64u - v); } else { scat(s, v > 64u ? "+" : ""); fmt_u(t, v - 64u); } scat(s, t); break;
    case K_ONOFF: scat(s, v ? "ON" : "OFF"); break;
    case K_WAVE: scat(s, WAVE_NAME[v & 3u]); break;
    case K_FTYPE: scat(s, FTYPE_NAME[v % 5u]); break;
    case K_MODE: scat(s, MODE_NAME[v & 3u]); break;
    case K_LSHAPE: scat(s, LSHAPE_NAME[v % 6u]); break;
    case K_LPHASE: scat(s, LPHASE_NAME[v % 3u]); break;
    case K_TO: scat(s, TO_NAME[v % 3u]); break;
    case K_ARP: scat(s, ARP_NAME[v % NARPMODE]); break;
    case K_DIV: scat(s, DIV_NAME[v % NDIV]); break;
    case K_SEMI: if (v < 24u) { scat(s, "-"); fmt_u(t, 24u - v); } else { scat(s, v > 24u ? "+" : ""); fmt_u(t, v - 24u); } scat(s, t); break;
    case K_MS:
        if (p == P_GLIDE) { if (!v) scat(s, "OFF"); else { fmt_u(s, GLIDE_MS[v & 127u]); scat(s, "ms"); } }
        else fmt_ms(s, TIME_MS_X10[v & 127u]);
        break;
    case K_HZ: { uint32_t h = LFO_HZ_X100[v & 127u]; fmt_u(s, h / 100u); scat(s, "."); fmt_u(t, (h / 10u) % 10u); scat(s, t); scat(s, "Hz"); } break;
    case K_PCT: fmt_u(s, (v * 100u + 63u) / 127u); scat(s, "%"); break;
    case K_CUT: { uint32_t hz = CUT_HZ[(v + 8u) % 137u]; if (hz < 10000u) { fmt_u(s, hz); scat(s, "Hz"); } else { fmt_u(s, hz / 1000u); scat(s, "."); fmt_u(t, (hz / 100u) % 10u); scat(s, t); scat(s, "k"); } } break;
    case K_OCT: fmt_u(s, v); scat(s, " OCT"); break;
    default: fmt_u(s, v);
    }
}

static uint32_t cut_shown_t;                    /* the header shows the cutoff for a second after it moved */
static void draw_header(void)
{
    char tags[40];
    tags[0] = 0;
    if (cut_shown_t && fm1_ms - cut_shown_t < 1000u) {
        char v[12];
        param_value(v, P_CUTOFF, live.v[P_CUTOFF]);
        scat(tags, "CUTOFF ");
        scat(tags, v);
        ui_header(prog, live.v + NPARAM, edited, tags);
        return;
    }
    scat(tags, MODE_NAME[live.v[P_MODE] & 3u]);
    scat(tags, " ");
    scat(tags, FTYPE_NAME[live.v[P_FTYPE] % 5u]);
    if (live.v[P_ARP_MODE]) scat(tags, " ARP");
    if (hold) scat(tags, " HOLD");
    ui_header(prog, live.v + NPARAM, edited, tags);
}

static void draw_page(void)
{
    knob_view_t k[4];
    uint32_t i;
    const page_t *pg = &PAGE[page];
    for (i = 0; i < 4u; i++) {
        int32_t p = pg->p[i];
        k[i].present = 0;
        k[i].bip = 0;
        k[i].value[0] = 0;
        if (page == PG_GLOBAL) {
            static const char *const GL[4] = {"TEMPO", "MIDI CH", "TUNE", "CPU"};
            k[i].label = GL[i];
            k[i].present = 1;
            if (i == 0) { fmt_u(k[i].value, set.tempo); k[i].v = set.tempo - 40; k[i].max = 200; }
            else if (i == 1) { if (set.midi_ch) fmt_u(k[i].value, set.midi_ch); else scat(k[i].value, "OMNI"); k[i].v = set.midi_ch; k[i].max = 16; }
            else if (i == 2) { scat(k[i].value, set.tune < 0 ? "-" : "+"); fmt_u(k[i].value + 1, (uint32_t)(set.tune < 0 ? -set.tune : set.tune)); scat(k[i].value, "c"); k[i].v = set.tune + 50; k[i].max = 100; k[i].bip = 1; }
            else { fmt_u(k[i].value, ui_render_us); scat(k[i].value, "us"); k[i].v = (int32_t)(ui_render_us > 5800u ? 5800u : ui_render_us); k[i].max = 5800; }
        } else if (p >= 0) {
            k[i].label = PDEF[p].name;
            k[i].v = live.v[p];
            k[i].max = PDEF[p].max;
            k[i].bip = PDEF[p].kind == K_BIP || PDEF[p].kind == K_SEMI;
            k[i].present = 1;
            param_value(k[i].value, (uint32_t)p, (uint32_t)live.v[p]);
        }
    }
    ui_page(pg->title, k, save_armed ? "SAVE?" : 0);
}

static void draw_tags(void) { draw_header(); }
static void draw_scope(void) { ui_scope(scope, SCOPE_N); }
static void draw_leds(void) { ui_leds(ui_vlevel, NV); }

/* ---- the main loop */
#define KEY0 53u                                /* key 0 = F3 */

static uint32_t cycle_page(uint32_t first, uint32_t count)   /* a button steps through its pages */
{
    if (page >= first && page < first + count) return first + (page - first + 1u) % count;
    return first;
}

static void app_main(void)
{
    uint32_t prev = 0, k, i, dirty_hdr = 1, dirty_page = 1, dirty_tags = 1, t_led = 0, t_scope = 0, t_page = 0, t_hdr = 0, leds_q = 1;
    uint32_t clk_n = 0, clk_t0 = 0, clk_last = 0, leds_prev = 0xFFFFFFFFu;
    uint8_t held_key[27] = {0};
    rnd = 0xC0FFEE42u;
    engine_init();
    cc_map_init();
    set.version = 0;
    if (settings_load(&set, sizeof set) || set.version != SET_VERSION) {
        uint32_t n;
        for (n = 0; n < sizeof set; n++) ((uint8_t *)&set)[n] = 0;
        set.version = SET_VERSION;
        set.tempo = 120;
    }
    if (set.tempo < 40u || set.tempo > 240u) set.tempo = 120;
    if (set.oct < -2 || set.oct > 2) set.oct = 0;
    if (set.midi_ch > 16u) set.midi_ch = 0;
    ctl_tempo = set.tempo;
    ctl_tune8 = set.tune * 256 / 100;
    load_program(set.prog);

    ui_splash();
    scope_state = 1;

    for (;;) {
        uint32_t keys, down, up, rel, b, lit = 0, base = (uint32_t)(KEY0 + 12 * set.oct), np = page;
        uint8_t st, d1, d2;
        int32_t e;
        platform_poll();

        /* keys */
        keys = fm1_in.notes;
        down = keys & ~prev;
        up = prev & ~keys;
        prev = keys;
        for (k = 0; k < 27u; k++) {
            if (down >> k & 1u) { held_key[k] = (uint8_t)(base + k); note_event(1, held_key[k], 100); midi_send(0x90, held_key[k], 100); }
            if (up >> k & 1u) { note_event(0, held_key[k], 0); midi_send(0x80, held_key[k], 0); }
        }

        /* buttons */
        b = fm1_input_edges(&rel);
        if (b & (1u << BTN_OCT_DOWN) && set.oct > -2) { set.oct--; set_dirty = 1; set_t = fm1_ms; }
        if (b & (1u << BTN_OCT_UP) && set.oct < 2) { set.oct++; set_dirty = 1; set_t = fm1_ms; }
        if (b & (1u << BTN_HOME)) np = PG_PERFORM;
        if (b & (1u << BTN_EDIT)) np = cycle_page(PG_OSC1, 5);
        if (b & (1u << BTN_ENV)) np = cycle_page(PG_FENV, 2);
        if (b & (1u << BTN_LFO)) np = cycle_page(PG_LFO, 3);
        if (b & (1u << BTN_FX)) np = cycle_page(PG_VOICE, 2);
        if (b & (1u << BTN_GLO)) np = PG_GLOBAL;
        if (b & (1u << BTN_ARP)) {
            static uint32_t arp_last = A_UP;
            if (page == PG_ARP) {               /* on its page the button is the switch */
                if (live.v[P_ARP_MODE]) { arp_last = live.v[P_ARP_MODE]; set_param(P_ARP_MODE, A_OFF); }
                else set_param(P_ARP_MODE, (int32_t)arp_last);
                dirty_page = dirty_tags = 1;
            } else np = PG_ARP;
        }
        if (b & (1u << BTN_PLAY)) { hold = !hold; ctl_hold = hold; dirty_tags = 1; }
        if (b & (1u << BTN_SEQ)) {              /* UNISON quick toggle, back to what it was */
            if (live.v[P_MODE] != M_UNISON) { quick_uni_prev = live.v[P_MODE]; set_param(P_MODE, M_UNISON); }
            else set_param(P_MODE, quick_uni_prev < 4u ? (int32_t)quick_uni_prev : M_POLY);
            dirty_page = dirty_tags = 1;
        }
        if (b & (1u << BTN_REC)) { randomize(); dirty_hdr = dirty_page = dirty_tags = 1; }
        if (b & (1u << BTN_SAVE)) {
            if (save_armed) {
                set.user[prog] = live;
                set.used |= 1u << prog;
                set.prog = (uint8_t)prog;
                edited = 0;
                save_armed = 0;
                set_dirty = 1; set_t = fm1_ms;
                dirty_hdr = 1;
            } else { save_armed = 1; save_t = fm1_ms; }
            dirty_page = 1;
        }
        if (save_armed && fm1_ms - save_t > 3000u) { save_armed = 0; dirty_page = 1; }
        if (np != page) { page = np; dirty_page = 1; }

        /* encoders */
        if ((e = fm1_enc_take(ENC_PRESETS)) != 0) {
            load_program((uint32_t)((int32_t)prog + e + (int32_t)NPROG * 4) % NPROG);
            set.prog = (uint8_t)prog; set_dirty = 1; set_t = fm1_ms;
            dirty_hdr = dirty_page = dirty_tags = 1;
        }
        if ((e = fm1_enc_take(ENC_ALGORITHM)) != 0) {
            set_param(P_CUTOFF, (int32_t)live.v[P_CUTOFF] + e * 2);
            cut_shown_t = fm1_ms | 1u;
            dirty_hdr = dirty_page = 1;
        }
        if (cut_shown_t && fm1_ms - cut_shown_t >= 1000u) { cut_shown_t = 0; dirty_hdr = 1; }
        if ((e = fm1_enc_take(ENC_SELECT)) != 0) {
            page = (uint32_t)((int32_t)page + e + (int32_t)NPAGE * 4) % NPAGE;
            dirty_page = 1;
        }
        for (k = 0; k < 4u; k++) {
            int32_t p = PAGE[page].p[k];
            if ((e = fm1_enc_take(ENC_KNOB1 + k)) == 0) continue;
            if (e > 1 || e < -1) e *= 3;        /* a fast turn goes further */
            if (page == PG_GLOBAL) {
                if (k == 0) { set.tempo = (uint8_t)clampi(set.tempo + e, 40, 240); ctl_tempo = set.tempo; }
                else if (k == 1) set.midi_ch = (uint8_t)clampi(set.midi_ch + e, 0, 16);
                else if (k == 2) { set.tune = (int8_t)clampi(set.tune + e, -50, 50); ctl_tune8 = set.tune * 256 / 100; }
                set_dirty = 1; set_t = fm1_ms;
            } else if (p >= 0) {
                int32_t step = PDEF[p].max >= 100 ? 2 : 1;
                set_param((uint32_t)p, (int32_t)live.v[p] + e * step);
                if (p == P_MODE || p == P_FTYPE || p == P_ARP_MODE) dirty_tags = 1;
                dirty_hdr = 1;
            }
            dirty_page = 1;
        }

        /* MIDI in */
        while (midi_get(&st, &d1, &d2)) {
            uint32_t ch = (st & 15u) + 1u, kind = st & 0xF0u;
            if (st == 0xF8u) {                  /* clock: 24 per quarter; the tempo follows, 2 s without it and it is gone */
                if (!clk_n) clk_t0 = fm1_ms;
                clk_last = fm1_ms;
                if (++clk_n == 25u) {
                    uint32_t ms = fm1_ms - clk_t0;
                    if (ms > 200u && ms < 1600u) ctl_ext_quarter = ms * 441u / 10u;
                    clk_n = 0;
                }
                continue;
            }
            if (st == 0xFAu || st == 0xFBu) { ctl_arp_restart = 1; clk_n = 0; continue; }
            if (st == 0xFCu) continue;
            if (st >= 0xF0u) continue;
            if (set.midi_ch && ch != set.midi_ch) continue;
            if (kind == 0x90u && d2) note_event(1, d1, d2);
            else if (kind == 0x80u || kind == 0x90u) note_event(0, d1, 0);
            else if (kind == 0xE0u) ctl_bend = (int32_t)(d1 | d2 << 7) - 8192;
            else if (kind == 0xC0u) { load_program(d1); dirty_hdr = dirty_page = dirty_tags = 1; }
            else if (kind == 0xB0u) {
                if (d1 == 7u) ctl_cc7 = d2;
                else if (d1 == 64u) ctl_sustain = d2 >= 64u;
                else if (d1 == 120u || d1 == 123u) ctl_panic = 1;
                else if (cc_map[d1]) {
                    uint32_t p = cc_map[d1] - 1u;
                    set_param(p, PDEF[p].max == 127 ? (int32_t)d2 : ((int32_t)d2 * PDEF[p].max + 63) / 127);
                    if (p == P_CUTOFF) cut_shown_t = fm1_ms | 1u;
                    dirty_page = dirty_hdr = 1;
                    if (p == P_MODE || p == P_FTYPE || p == P_ARP_MODE) dirty_tags = 1;
                }
            }
        }
        if (ctl_ext_quarter && fm1_ms - clk_last > 2000u) { ctl_ext_quarter = 0; clk_n = 0; }

        /* LEDs: the keys that sound, the lit buttons */
        for (k = 0; k < NV; k++)
            if (ui_vlevel[k] > 300 && ui_vnote[k] >= base && ui_vnote[k] < base + 27u)
                lit |= 1u << (ui_vnote[k] - base);
        lit |= (hold << 27) | ((uint32_t)(live.v[P_ARP_MODE] != 0) << 28) | ((uint32_t)(set.oct < 0) << 29)
             | ((uint32_t)(set.oct > 0) << 30) | ((uint32_t)(live.v[P_MODE] == M_UNISON) << 31);
        if (lit != leds_prev) {
            leds_prev = lit;
            for (k = 0; k < 27u; k++) fm1_led_key(KEY_LED(k), lit >> k & 1u);
            fm1_led_key(BTN_PLAY, lit >> 27 & 1u);
            fm1_led_key(BTN_ARP, lit >> 28 & 1u);
            fm1_led_key(BTN_OCT_DOWN, lit >> 29 & 1u);
            fm1_led_key(BTN_OCT_UP, lit >> 30 & 1u);
            fm1_led_key(BTN_SEQ, lit >> 31 & 1u);
            for (k = 0; k < 8u; k++)            /* the page buttons: the one whose page shows */
                ;
        }
        fm1_led_key(BTN_HOME, page == PG_PERFORM);
        fm1_led_key(BTN_EDIT, page >= PG_OSC1 && page <= PG_FILTER2);
        fm1_led_key(BTN_ENV, page == PG_FENV || page == PG_AENV);
        fm1_led_key(BTN_LFO, page >= PG_LFO && page <= PG_LFO2);
        fm1_led_key(BTN_FX, page == PG_VOICE || page == PG_OUTPUT);
        fm1_led_key(BTN_GLO, page == PG_GLOBAL);
        fm1_led_key(BTN_SAVE, save_armed);

        /* the screen: only what changed */
        if (dirty_hdr && fm1_ms - t_hdr >= 30u) { dirty_hdr = 0; t_hdr = fm1_ms; draw_header(); }
        if (dirty_page && fm1_ms - t_page >= 30u) { dirty_page = 0; t_page = fm1_ms; draw_page(); }
        if (dirty_tags) { dirty_tags = 0; draw_tags(); }
        if (fm1_ms - t_led >= 40u) {
            uint32_t q = 0;
            t_led = fm1_ms;
            for (k = 0; k < NV; k++) q = q * 9u + ((uint32_t)ui_vlevel[k] >> 12);   /* 8 steps a LED: redraw on a change */
            if (q != leds_q) { leds_q = q; draw_leds(); }
            if (page == PG_GLOBAL && fm1_ms - t_page >= 250u) dirty_page = 1;
        }
        if (scope_state == 3u && fm1_ms - t_scope >= 50u) {
            uint32_t v, best = 0, bl = 0;
            t_scope = fm1_ms;
            draw_scope();
            for (v = 0; v < NV; v++)            /* the stride from the loudest voice's period: ~2 cycles across */
                if ((uint32_t)ui_vlevel[v] > bl) { bl = (uint32_t)ui_vlevel[v]; best = ui_vnote[v]; }
            if (bl) {
                uint32_t inc = pitch_inc((int32_t)best << 8), period = inc ? 0xFFFFFFFFu / inc : 1000u;
                scope_stride = clampi((int32_t)(period * 2u / SCOPE_N), 1, 64);
            }
            scope_state = 1;
        } else if (scope_state == 1u && fm1_ms - t_scope >= 400u) {   /* silence: a flat line */
            t_scope = fm1_ms;
            for (i = 0; i < SCOPE_N; i++) scope[i] = 0;
            draw_scope();
        }

        if (set_dirty && engine_quiet() && fm1_ms - set_t > 2000u) {   /* when silent: an erase stalls the sound */
            settings_save(&set, sizeof set);
            set_dirty = 0;
        }
    }
}
