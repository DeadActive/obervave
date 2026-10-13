/* SPDX-License-Identifier: GPL-3.0-only */
/* OberVave: one 8-voice analog-style polysynth for the M-VAVE FM-1, in the spirit of the classic American
 * poly synths of 1979-84. Built on Dinghy's platform (platform.c: boot, USB, updates, audio ISR, settings).
 *
 *   obv_dsp.h       oscillators (polyBLEP events), filter (trapezoidal SVF), tanh
 *   obv_params.h    the parameter table: patches, pages, MIDI CC
 *   obv_fx.h        chorus, ping-pong delay, plate reverb (after the voices, same ISR)
 *   obv_engine.h    the voices, envelopes, LFO, arpeggiator: all in the audio ISR
 *   obv_seq.h       the step sequencer: two tracks sharing 256 steps, played in the ISR too
 *   obv_patches.h   the factory programs
 *   this file       the panel, MIDI, the screen, the storage
 *
 * Integers only (no FPU, no 64-bit division); tests/host.c runs this on a computer. */
#include "platform.c"
#include "obv_dsp.h"
#include "obv_params.h"
#include "obv_fx.h"
#include "obv_engine.h"
#include "obv_seq.h"
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
#define SET_VERSION 2u                          /* 2: 80-byte patches (the effects) */
static struct {
    uint8_t version, prog, tempo, midi_ch;      /* midi_ch 0 = omni, else 1..16 */
    int8_t tune, oct;
    uint8_t pat, pad;                           /* pat: the sequencer pattern (its own sectors) */
    uint32_t used;                              /* bit n: user program n saved */
    patch_t user[NPROG];
} set;
static uint32_t set_dirty, set_t;

static uint32_t prog, edited, page = PG_PERFORM, hold, save_armed, save_t, dice_armed, dice_t;

/* the sequencer, as the panel sees it */
static uint32_t seq_trk, seq_cur[2], seq_pg[2], rec, rec_held, rec_step, pat_edited, pat_dirty, seq_back = PG_PERFORM;
static uint32_t pen = 60;                       /* the note a tapped empty step gets: the last one played */
static int32_t step_held = -1;                  /* the step whose white key is held in the step view, -1 none */
static uint32_t step_held_t, step_turned, step_midi, clear_t, toast_t;
static uint8_t audition[SEQ_CHORD];             /* the held step's notes sounding (stopped: you hear what you hold) */
static const char *toast;
static int is_seq_page(uint32_t pg) { return pg == PG_SEQ; }
static void say(const char *s) { toast = s; toast_t = fm1_ms | 1u; }

/* the step view: the 16 white keys are steps 1-16 of the page, the black keys are the sequencer's functions */
static const int8_t KEY_STEP[27] = {0, -1, 1, -1, 2, -1, 3, 4, -1, 5, -1, 6, 7, -1, 8, -1, 9, -1, 10, 11, -1, 12, -1, 13, 14, -1, 15};
enum { KF_NONE, KF_TRK_A, KF_TRK_B, KF_PG_DN, KF_PG_UP, KF_MUTE, KF_PAT_DN, KF_PAT_UP, KF_CLEAR };
static const uint8_t KEY_FN[27] = {[1] = KF_TRK_A, [3] = KF_TRK_B, [5] = KF_PG_DN, [8] = KF_PG_UP, [10] = KF_MUTE,
                                   [13] = KF_PAT_DN, [15] = KF_PAT_UP, [17] = KF_CLEAR};   /* F#3 G#3 A#3 C#4 D#4 F#4 G#4 A#4 */
static uint32_t seq_pages(uint32_t t) { return pat.trk[t].len ? (pat.trk[t].len + 15u) / 16u : 1u; }
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
}

static void load_pattern(uint32_t n)            /* the saved one, else the factory's */
{
    pattern_t *p = &pat;
    n %= NPAT;
    ctl_panic = 1;                              /* (what the old one held lets go) */
    if (blob_load(n, p, sizeof *p) || p->magic != PAT_MAGIC) pat_factory(n, p);
    set.pat = (uint8_t)n;
    seq_cur[0] = seq_cur[1] = seq_pg[0] = seq_pg[1] = 0;
    pat_edited = 0;
}

/* REC: a note (key or MIDI) into the step at the cursor (stopped) or the step nearest now (running). The notes
 * pressed together make the chord; when all are up the cursor moves on (stopped). */
static void rec_note(uint32_t on, uint32_t note, uint32_t vel)
{
    uint32_t t = seq_trk, len = pat.trk[t].len, k;
    step_t *st;
    if (!rec || !len) return;
    if (!on) {
        if (rec_held && !--rec_held && !ctl_seq_run) {   /* the chord is in: on to the next step, the page follows */
            seq_cur[t] = (rec_step + 1u) % len;
            seq_pg[t] = seq_cur[t] / 16u;
        }
        return;
    }
    if (!rec_held) {                            /* the first note of a chord: the step starts over */
        rec_step = ctl_seq_run ? (ui_seq_pos[t] + (ui_seq_frac[t] >= 128u)) % len : seq_cur[t] % len;
        st = seq_step(t, rec_step);
        for (k = 0; k < SEQ_CHORD; k++) st->note[k] = 0;
        st->ratch &= (uint8_t)~STEP_OFF;
    }
    rec_held++;
    st = seq_step(t, rec_step);
    for (k = 0; k < SEQ_CHORD && st->note[k] && st->note[k] != note; k++) ;
    if (k < SEQ_CHORD && !st->note[k]) { st->note[k] = (uint8_t)note; st->vel = (uint8_t)vel; }
    pat_edited = 1;
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
    live.v[P_FTYPE] = (uint8_t)(rn(0, 2) ? F_LP24 : rn(0, NFTYPE - 1));
    live.v[P_FMORPH] = (uint8_t)rn(0, 127);
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
    live.v[P_CHO_MIX] = (uint8_t)(rn(0, 2) ? 0 : rn(30, 100));
    live.v[P_DLY_TIME] = (uint8_t)rn(0, NDDIV - 1);
    live.v[P_DLY_FB] = (uint8_t)rn(20, 80);
    live.v[P_DLY_MIX] = (uint8_t)(rn(0, 1) ? 0 : rn(20, 70));
    live.v[P_REV_SIZE] = (uint8_t)rn(40, 127);
    live.v[P_REV_MIX] = (uint8_t)(rn(0, 3) ? rn(15, 60) : 0);
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
    case K_FTYPE: scat(s, FTYPE_NAME[v % NFTYPE]); break;
    case K_MODE: scat(s, MODE_NAME[v & 3u]); break;
    case K_LSHAPE: scat(s, LSHAPE_NAME[v % 6u]); break;
    case K_LPHASE: scat(s, LPHASE_NAME[v % 3u]); break;
    case K_TO: scat(s, TO_NAME[v % 3u]); break;
    case K_ARP: scat(s, ARP_NAME[v % NARPMODE]); break;
    case K_DIV: scat(s, DIV_NAME[v % NDIV]); break;
    case K_DDIV: scat(s, DDIV_NAME[v % NDDIV]); break;
    case K_MORPH:                               /* LP64 .. NOTCH .. HP63 */
        if (v == 64u) scat(s, "NOTCH");
        else { scat(s, v < 64u ? "LP " : "HP "); fmt_u(t, v < 64u ? 64u - v : v - 64u); scat(s, t); }
        break;
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
    scat(tags, FTYPE_NAME[live.v[P_FTYPE] % NFTYPE]);
    if (live.v[P_ARP_MODE]) scat(tags, " ARP");
    if (live.v[P_CHO_MIX] || live.v[P_DLY_MIX] || live.v[P_REV_MIX]) scat(tags, " FX");
    if (hold) scat(tags, " HOLD");
    if (ctl_seq_run) scat(tags, " SEQ");
    if (rec) scat(tags, " REC");
    ui_header(prog, live.v + NPARAM, edited, tags);
}

static void fmt_s(char *s, int32_t v)          /* signed: +3, -12, 0 */
{
    s[0] = 0;
    if (v) scat(s, v < 0 ? "-" : "+");
    fmt_u(s + slen(s), (uint32_t)(v < 0 ? -v : v));
}

static void fmt_note(char *s, uint32_t n)       /* C4, F#3 */
{
    static const char *const NN[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    s[0] = 0;
    scat(s, NN[n % 12u]);
    if (n < 12u) scat(s, "-");
    fmt_u(s + slen(s), n < 12u ? 1u : n / 12u - 1u);
}

static void seq_knob(knob_view_t *k, uint32_t i)   /* the step view's four knobs: the track's, or the held step's */
{
    static const char *const LT[4] = {"LENGTH", "NOTE", "SWING", "TRANSP"}, *const LS[4] = {"PITCH", "GATE", "RATCHET", "CHANCE"};
    seqtrk_t *tk = &pat.trk[seq_trk];
    char *v = k->value;
    k->present = 1;
    if (step_held >= 0) {
        step_t *st = seq_step(seq_trk, (uint32_t)step_held);
        uint32_t n;
        k->label = LS[i];
        if (i == 0) {
            if (!st->note[0]) scat(v, "EMPTY");
            else {
                for (n = 0; n < SEQ_CHORD && st->note[n]; n++) ;
                fmt_note(v, st->note[0]);
                if (n > 1u) { scat(v, "+"); fmt_u(v + slen(v), n - 1u); }
            }
            k->v = st->note[0]; k->max = 127;
        } else if (i == 1) { if (st->gate >= GATE_TIE) scat(v, "TIE"); else { fmt_u(v, (st->gate * 100u + 63u) / 127u); scat(v, "%"); } k->v = st->gate; k->max = 127; }
        else if (i == 2) { fmt_u(v, (st->ratch & 3u) + 1u); scat(v, "x"); k->v = st->ratch & 3u; k->max = 3; }
        else { fmt_u(v, st->chance); scat(v, "%"); k->v = st->chance; k->max = 100; }
    } else {
        k->label = LT[i];
        if (i == 0) { if (tk->len) fmt_u(v, tk->len); else scat(v, "OFF"); k->v = tk->len; k->max = SEQ_STEPS; }
        else if (i == 1) { scat(v, DIV_NAME[tk->div % NDIV]); k->v = tk->div; k->max = NDIV - 1; }
        else if (i == 2) { fmt_u(v, tk->swing); scat(v, "%"); k->v = tk->swing - 50; k->max = 25; }
        else { fmt_s(v, tk->transp); k->v = tk->transp + 24; k->max = 48; k->bip = 1; }
    }
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
        } else if (is_seq_page(page)) {
            seq_knob(&k[i], i);
        } else if (p >= 0) {
            k[i].label = PDEF[p].name;
            k[i].v = live.v[p];
            k[i].max = PDEF[p].max;
            k[i].bip = PDEF[p].kind == K_BIP || PDEF[p].kind == K_SEMI || PDEF[p].kind == K_MORPH;
            k[i].present = 1;
            param_value(k[i].value, (uint32_t)p, (uint32_t)live.v[p]);
        }
    }
    {
        static char title[24];
        const char *t = pg->title;
        if (is_seq_page(page)) {                /* SEQ A, or STEP 5 A while a step is held */
            title[0] = 0;
            if (step_held >= 0) { scat(title, "STEP "); fmt_u(title + slen(title), (uint32_t)step_held + 1u); scat(title, " "); }
            else scat(title, "SEQ ");
            scat(title, seq_trk ? "B" : "A");
            if (pat.trk[seq_trk].mute) scat(title, " MUTED");
            t = title;
        }
        ui_page(t, k, save_armed ? "SAVE?" : dice_armed ? "DICE?" : is_seq_page(page) && pat_edited ? "EDITED" : 0);
    }
}

static void draw_steps(void)
{
    static char l[32];
    uint32_t t = seq_trk, len = pat.trk[t].len, ph = len ? ui_seq_pos[t] % len : 0u;
    int32_t sel = step_held >= 0 ? step_held : rec ? (int32_t)(len ? seq_cur[t] % len : 0u) : -1;
    l[0] = 0;
    scat(l, t ? "B  PAGE " : "A  PAGE ");
    fmt_u(l + slen(l), seq_pg[t] + 1u); scat(l, "/"); fmt_u(l + slen(l), seq_pages(t));
    scat(l, "  PAT ");
    fmt_u(l + slen(l), set.pat + 1u);
    ui_steps(t, seq_pg[t] * 16u, sel, ctl_seq_run && len ? (int32_t)ph : -1, rec, l,
             rec ? (ctl_seq_run ? "REC LIVE" : "REC") : ctl_seq_run ? "PLAY" : "STOP");
}

/* the hint lines under the strip: what the keys do right now (a toast after an action, else a slow rotation) */
static uint32_t hints_key = 0xFFFFFFFFu;
static void draw_hints(void)
{
    static const char *const ROT[4][2] = {
        {"WHITE KEYS = STEPS 1-16", "TAP: ON/OFF  HOLD+KNOB: EDIT"},
        {"F#3 G#3: TRACK A / B", "A#3 C#4: PAGE <  >"},
        {"D#4: MUTE  F#4 G#4: PATTERN", "A#4 HOLD: CLEAR THE TRACK"},
        {"REC: RECORD NOTES INTO STEPS", "PLAY: START / STOP  SEQ: BACK"}};
    uint32_t r = (fm1_ms / 3500u) % 4u, key;
    int toasting = toast && fm1_ms - toast_t < 1500u;
    key = toasting ? 100u + (toast_t & 0xFFu) : rec ? 200u : step_held >= 0 ? 300u : r;
    if (key == hints_key) return;
    hints_key = key;
    if (toasting) ui_hints(toast, "", C_AMBER);
    else if (rec) ui_hints(ctl_seq_run ? "REC: PLAY ALONG, IT LANDS" : "REC: PLAY A NOTE OR A CHORD", ctl_seq_run ? "ON THE NEAREST STEP  REC: DONE" : "SEQ: REST       REC: DONE", C_RED);
    else if (step_held >= 0) ui_hints("TURN A KNOB: EDIT THIS STEP", "PITCH  GATE  RATCHET  CHANCE", C_AMBER);
    else ui_hints(ROT[r][0], ROT[r][1], C_CREAM);
}

static void draw_tags(void) { draw_header(); }
static void draw_scope(void) { ui_scope(scope, SCOPE_N); }
static void draw_leds(void) { ui_leds(ui_vlevel, NV); }

/* a knob in the step view turned by e: the held step's, else the track's */
static void seq_turn(uint32_t knob, int32_t e)
{
    uint32_t t = seq_trk, o = t ^ 1u, k;
    seqtrk_t *tk = &pat.trk[t];
    int32_t sg = e > 0 ? 1 : -1;
    if (step_held >= 0) {
        step_t *st = seq_step(t, (uint32_t)step_held);
        step_turned = 1;
        if (knob == 0) {                        /* PITCH: the chord moves; an empty step gets the pen note */
            if (!st->note[0]) { step_clear(st); st->note[0] = (uint8_t)pen; }
            else for (k = 0; k < SEQ_CHORD && st->note[k]; k++) st->note[k] = (uint8_t)clampi(st->note[k] + sg, 1, 127);
            st->ratch &= (uint8_t)~STEP_OFF;
            pen = st->note[0];
        } else if (knob == 1) st->gate = (uint8_t)clampi((int32_t)st->gate + e * 2, 1, GATE_TIE);
        else if (knob == 2) st->ratch = (uint8_t)((st->ratch & STEP_OFF) | (uint32_t)clampi((int32_t)(st->ratch & 3u) + sg, 0, 3));
        else st->chance = (uint8_t)clampi((int32_t)st->chance + e * 5, 0, 100);
    } else if (knob == 0) {                     /* LENGTH: A and B share 256 steps; A at least 1, B 0 = off */
        tk->len = (uint16_t)clampi((int32_t)tk->len + e, t ? 0 : 1, (int32_t)(SEQ_STEPS - pat.trk[o].len));
        if (tk->len && seq_cur[t] >= tk->len) seq_cur[t] = tk->len - 1u;
        if (seq_pg[t] >= seq_pages(t)) seq_pg[t] = seq_pages(t) - 1u;
    } else if (knob == 1) tk->div = (uint8_t)clampi((int32_t)tk->div + sg, 0, NDIV - 1);
    else if (knob == 2) tk->swing = (uint8_t)clampi((int32_t)tk->swing + e, 50, 75);
    else tk->transp = (int8_t)clampi(tk->transp + e, -24, 24);
    pat_edited = 1;
}

static void audition_off(void)
{
    uint32_t k;
    for (k = 0; k < SEQ_CHORD; k++)
        if (audition[k]) { note_event(0, audition[k], 0); audition[k] = 0; }
}

/* a key went down in the step view: a white key picks its step (a tap switches it on / off when it comes up),
 * a black key is a function */
static void seq_key_down(uint32_t k)
{
    static char msg[20];
    uint32_t t = seq_trk, len = pat.trk[t].len, n, i;
    seqtrk_t *tk = &pat.trk[t];
    if (KEY_STEP[k] >= 0) {
        n = seq_pg[t] * 16u + (uint32_t)KEY_STEP[k];
        if (n >= len) { say(len ? "PAST THE TRACK'S LENGTH" : "TRACK OFF: TURN LENGTH"); return; }
        step_held = (int32_t)n;
        step_held_t = fm1_ms;
        step_turned = step_midi = 0;
        seq_cur[t] = n;
        if (!ctl_seq_run) {                     /* stopped: you hear the step you hold */
            const step_t *st = seq_step(t, n);
            audition_off();
            for (i = 0; i < SEQ_CHORD && st->note[i]; i++) {
                audition[i] = (uint8_t)clampi(st->note[i] + tk->transp, 1, 127);
                note_event(1, audition[i], st->vel ? st->vel : 100u);
            }
        }
        return;
    }
    switch (KEY_FN[k]) {
    case KF_TRK_A: case KF_TRK_B:
        seq_trk = KEY_FN[k] == KF_TRK_B;
        say(seq_trk ? (pat.trk[1].len ? "TRACK B" : "TRACK B IS OFF: TURN LENGTH") : "TRACK A");
        break;
    case KF_PG_DN: case KF_PG_UP:
        if (KEY_FN[k] == KF_PG_DN && seq_pg[t]) seq_pg[t]--;
        else if (KEY_FN[k] == KF_PG_UP && seq_pg[t] + 1u < seq_pages(t)) seq_pg[t]++;
        msg[0] = 0; scat(msg, "PAGE "); fmt_u(msg + slen(msg), seq_pg[t] + 1u); scat(msg, " OF "); fmt_u(msg + slen(msg), seq_pages(t));
        say(msg);
        break;
    case KF_MUTE:
        tk->mute = !tk->mute;
        pat_edited = 1;
        say(tk->mute ? (t ? "TRACK B MUTED" : "TRACK A MUTED") : (t ? "TRACK B ON" : "TRACK A ON"));
        break;
    case KF_PAT_DN: case KF_PAT_UP:
        n = (uint32_t)clampi((int32_t)set.pat + (KEY_FN[k] == KF_PAT_UP ? 1 : -1), 0, NPAT - 1);
        if (n != set.pat) { load_pattern(n); set_dirty = 1; set_t = fm1_ms; }
        msg[0] = 0; scat(msg, "PATTERN "); fmt_u(msg + slen(msg), set.pat + 1u); scat(msg, " OF 8");
        say(msg);
        break;
    case KF_CLEAR:
        clear_t = fm1_ms | 1u;
        say("KEEP HOLDING TO CLEAR");
        break;
    default:
        break;
    }
}

static void seq_key_up(uint32_t k)
{
    uint32_t t = seq_trk;
    if (KEY_STEP[k] >= 0) {
        uint32_t n = seq_pg[t] * 16u + (uint32_t)KEY_STEP[k];
        if (step_held == (int32_t)n) {
            if (!step_turned && !step_midi && fm1_ms - step_held_t < 400u) {   /* a tap: on / off; empty gets the pen note */
                step_t *st = seq_step(t, n);
                if (!st->note[0]) { step_clear(st); st->note[0] = (uint8_t)pen; }
                else st->ratch ^= STEP_OFF;
                pat_edited = 1;
            }
            step_held = -1;
            audition_off();
        }
    } else if (KEY_FN[k] == KF_CLEAR)
        clear_t = 0;
}

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
    uint32_t clk_n = 0, clk_t0 = 0, clk_last = 0, leds_prev = 0xFFFFFFFFu, glow = 0, glow_prev = 0xFFFFFFFFu, t_steps = 0, ticks_seen = 0, dirty_steps = 1;
    uint8_t held_key[27] = {0}, key_step[27] = {0};
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
    load_pattern(set.pat);
    ctl_panic = 0;

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
        for (k = 0; k < 27u; k++) {             /* the step view: keys are steps / functions; else (and in REC) notes */
            if (down >> k & 1u) {
                key_step[k] = is_seq_page(page) && !rec;
                if (key_step[k]) seq_key_down(k);
                else { held_key[k] = (uint8_t)(base + k); note_event(1, held_key[k], 100); midi_send(0x90, held_key[k], 100); rec_note(1, held_key[k], 100); pen = held_key[k]; }
                dirty_steps = dirty_page = 1;
            }
            if (up >> k & 1u) {
                if (key_step[k]) seq_key_up(k);
                else { note_event(0, held_key[k], 0); midi_send(0x80, held_key[k], 0); rec_note(0, held_key[k], 0); }
                dirty_steps = dirty_page = 1;
            }
        }
        if (clear_t && fm1_ms - clear_t > 1500u) {   /* A#4 held long enough: the track's steps are empty */
            uint32_t n;
            for (n = 0; n < pat.trk[seq_trk].len; n++) step_clear(seq_step(seq_trk, n));
            clear_t = 0;
            pat_edited = 1;
            say(seq_trk ? "TRACK B CLEARED" : "TRACK A CLEARED");
            dirty_steps = dirty_page = 1;
        }

        /* buttons */
        b = fm1_input_edges(&rel);
        if (b & (1u << BTN_OCT_DOWN) && set.oct > -2) { set.oct--; set_dirty = 1; set_t = fm1_ms; }
        if (b & (1u << BTN_OCT_UP) && set.oct < 2) { set.oct++; set_dirty = 1; set_t = fm1_ms; }
        if (b & (1u << BTN_HOME)) np = PG_PERFORM;
        if (b & (1u << BTN_EDIT)) np = cycle_page(PG_OSC1, 5);
        if (b & (1u << BTN_ENV)) np = cycle_page(PG_FENV, 2);
        if (b & (1u << BTN_LFO)) np = cycle_page(PG_LFO, 3);
        if (b & (1u << BTN_FX)) np = cycle_page(PG_CHORUS, 3);
        if (b & (1u << BTN_GLO)) np = page == PG_GLOBAL ? PG_VOICE : page == PG_VOICE ? PG_OUTPUT : PG_GLOBAL;
        if (b & (1u << BTN_ARP)) {
            if (page == PG_ARP) { hold = !hold; ctl_hold = hold; dirty_tags = 1; }   /* on its page: HOLD (latch) */
            else np = PG_ARP;
        }
        if (b & (1u << BTN_PLAY)) {             /* the sequencer: start from step 1 / stop */
            if (ctl_seq_run) ctl_seq_run = 0;
            else { ctl_seq_restart = 1; ctl_seq_run = 1; }
            dirty_tags = dirty_steps = 1;
        }
        if (b & (1u << BTN_REC)) {             /* record: into the selected step (stopped) or the nearest (running) */
            rec = !rec;
            rec_held = 0;
            if (rec) { if (!is_seq_page(page)) seq_back = page; np = PG_SEQ; }
            dirty_tags = dirty_steps = dirty_page = 1;
        }
        if (b & (1u << BTN_SEQ)) {
            uint32_t len = pat.trk[seq_trk].len;
            if (rec && len && !ctl_seq_run) {   /* recording: a rest, and on */
                step_t *st = seq_step(seq_trk, seq_cur[seq_trk] % len);
                for (k = 0; k < SEQ_CHORD; k++) st->note[k] = 0;
                seq_cur[seq_trk] = (seq_cur[seq_trk] + 1u) % len;
                seq_pg[seq_trk] = seq_cur[seq_trk] / 16u;
                pat_edited = 1;
                dirty_steps = dirty_page = 1;
            } else if (is_seq_page(page))       /* the step view off: back to where you were, keys are notes again */
                np = seq_back;
            else { seq_back = page; np = PG_SEQ; }
        }
        if (b & (1u << BTN_SEL)) {              /* the SELECT knob pressed twice: a new sound (dice) */
            if (dice_armed) { dice_armed = 0; randomize(); dirty_hdr = dirty_tags = 1; }
            else { dice_armed = 1; dice_t = fm1_ms; }
            dirty_page = 1;
        }
        if (dice_armed && fm1_ms - dice_t > 3000u) { dice_armed = 0; dirty_page = 1; }
        if (b & (1u << BTN_SAVE)) {
            if (save_armed) {
                if (is_seq_page(page)) pat_dirty = 1;   /* the pattern: saved once the sequencer stops and all is quiet */
                else {
                    set.user[prog] = live;
                    set.used |= 1u << prog;
                    set.prog = (uint8_t)prog;
                    edited = 0;
                    set_dirty = 1; set_t = fm1_ms;
                }
                save_armed = 0;
                dirty_hdr = 1;
            } else { save_armed = 1; save_t = fm1_ms; }
            dirty_page = 1;
        }
        if (save_armed && fm1_ms - save_t > 3000u) { save_armed = 0; dirty_page = 1; }
        if (np != page) {
            if (is_seq_page(page) && !is_seq_page(np)) rec = 0;   /* leaving the step view ends a recording */
            page = np; dirty_page = dirty_steps = 1; step_held = -1; audition_off(); hints_key = 0xFFFFFFFFu; clear_t = 0; leds_q = 0xFFFFFFFFu;
        }

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
            if (is_seq_page(page)) { rec = 0; step_held = -1; audition_off(); }
            page = (uint32_t)((int32_t)page + e + (int32_t)NPAGE * 4) % NPAGE;
            dirty_page = dirty_steps = dirty_tags = 1;
            hints_key = 0xFFFFFFFFu;
            leds_q = 0xFFFFFFFFu;
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
            } else if (is_seq_page(page)) {
                seq_turn(k, e);
                dirty_steps = 1;
            } else if (p >= 0) {
                int32_t step = PDEF[p].max >= 100 ? 2 : 1;
                set_param((uint32_t)p, (int32_t)live.v[p] + e * step);
                if (p == P_MODE || p == P_FTYPE || p == P_ARP_MODE || p >= P_CHO_RATE) dirty_tags = 1;
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
            if (st == 0xFAu || st == 0xFBu) {   /* start / continue: the arp and the sequencer from the top */
                ctl_arp_restart = 1; clk_n = 0;
                ctl_seq_restart = 1; ctl_seq_run = 1;
                dirty_tags = dirty_steps = 1;
                continue;
            }
            if (st == 0xFCu) { ctl_seq_run = 0; dirty_tags = dirty_steps = 1; continue; }
            if (st >= 0xF0u) continue;
            if (set.midi_ch && ch != set.midi_ch) continue;
            if (kind == 0x90u && d2) {
                note_event(1, d1, d2); rec_note(1, d1, d2); pen = d1;
                if (step_held >= 0 && !rec) {   /* a held step takes what MIDI plays: the first note starts the chord */
                    step_t *sp = seq_step(seq_trk, (uint32_t)step_held);
                    uint32_t q;
                    if (!step_midi++) { for (q = 0; q < SEQ_CHORD; q++) sp->note[q] = 0; sp->ratch &= (uint8_t)~STEP_OFF; }
                    for (q = 0; q < SEQ_CHORD && sp->note[q] && sp->note[q] != d1; q++) ;
                    if (q < SEQ_CHORD && !sp->note[q]) { sp->note[q] = d1; sp->vel = d2; }
                    pat_edited = 1;
                }
                dirty_steps = dirty_page = 1;
            }
            else if (kind == 0x80u || kind == 0x90u) { note_event(0, d1, 0); rec_note(0, d1, 0); dirty_page |= rec; }
            else if (kind == 0xE0u) ctl_bend = (int32_t)(d1 | d2 << 7) - 8192;
            else if (kind == 0xC0u) { load_program(d1); dirty_hdr = dirty_page = dirty_tags = 1; set.prog = (uint8_t)prog; }
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

        /* LEDs: the keys that sound, the lit buttons; in the step view the white keys are the steps (glowing: a note
         * there, bright: playing now or held) and the black keys their functions (bright: on, glowing: available) */
        glow = 0;
        if (is_seq_page(page) && !rec) {
            uint32_t t = seq_trk, len = pat.trk[t].len, w0 = seq_pg[t] * 16u, ph = len ? ui_seq_pos[t] % len : 0u;
            for (k = 0; k < 27u; k++) {
                if (KEY_STEP[k] >= 0) {
                    uint32_t n = w0 + (uint32_t)KEY_STEP[k];
                    const step_t *st;
                    if (n >= len) continue;
                    st = seq_step(t, n);
                    if (st->note[0] && !(st->ratch & STEP_OFF)) glow |= 1u << k;
                    if ((ctl_seq_run && n == ph) || (int32_t)n == step_held) lit |= 1u << k;
                    continue;
                }
                switch (KEY_FN[k]) {
                case KF_TRK_A: if (t == 0u) lit |= 1u << k; else glow |= 1u << k; break;
                case KF_TRK_B: if (t == 1u) lit |= 1u << k; else glow |= 1u << k; break;
                case KF_PG_DN: if (seq_pg[t]) glow |= 1u << k; break;
                case KF_PG_UP: if (seq_pg[t] + 1u < seq_pages(t)) glow |= 1u << k; break;
                case KF_MUTE: if (pat.trk[t].mute) lit |= 1u << k; else glow |= 1u << k; break;
                case KF_PAT_DN: if (set.pat) glow |= 1u << k; break;
                case KF_PAT_UP: if (set.pat + 1u < NPAT) glow |= 1u << k; break;
                case KF_CLEAR: if (clear_t) lit |= 1u << k; else glow |= 1u << k; break;
                default: break;
                }
            }
        } else {
            for (k = 0; k < NV; k++)
                if (ui_vlevel[k] > 300 && ui_vnote[k] >= base && ui_vnote[k] < base + 27u)
                    lit |= 1u << (ui_vnote[k] - base);
            if (rec && !ctl_seq_run && pat.trk[seq_trk].len) {   /* recording: the keys of the selected step */
                const step_t *st = seq_step(seq_trk, seq_cur[seq_trk] % pat.trk[seq_trk].len);
                for (k = 0; k < SEQ_CHORD && st->note[k]; k++)
                    if (st->note[k] >= base && st->note[k] < base + 27u) lit |= 1u << (st->note[k] - base);
            }
        }
        lit |= (ctl_seq_run << 27) | ((uint32_t)(live.v[P_ARP_MODE] != 0) << 28) | ((uint32_t)(set.oct < 0) << 29)
             | ((uint32_t)(set.oct > 0) << 30) | (rec << 31);
        if (lit != leds_prev || glow != glow_prev) {
            leds_prev = lit;
            glow_prev = glow;
            for (k = 0; k < 27u; k++) { fm1_led_key(KEY_LED(k), lit >> k & 1u); fm1_led_key_glow(KEY_LED(k), glow >> k & 1u); }
            fm1_led_key(BTN_PLAY, lit >> 27 & 1u);
            fm1_led_key(BTN_ARP, lit >> 28 & 1u);
            fm1_led_key(BTN_OCT_DOWN, lit >> 29 & 1u);
            fm1_led_key(BTN_OCT_UP, lit >> 30 & 1u);
            fm1_led_key(BTN_REC, lit >> 31 & 1u);
        }
        fm1_led_key(BTN_SEQ, is_seq_page(page));
        fm1_led_key(BTN_HOME, page == PG_PERFORM);
        fm1_led_key(BTN_EDIT, page >= PG_OSC1 && page <= PG_FILTER2);
        fm1_led_key(BTN_ENV, page == PG_FENV || page == PG_AENV);
        fm1_led_key(BTN_LFO, page >= PG_LFO && page <= PG_LFO2);
        fm1_led_key(BTN_FX, page >= PG_CHORUS && page <= PG_REVERB);
        fm1_led_key(BTN_GLO, page >= PG_VOICE && page <= PG_GLOBAL);
        fm1_led_key(BTN_SAVE, save_armed);

        /* the screen: only what changed */
        if (dirty_hdr && fm1_ms - t_hdr >= 30u) { dirty_hdr = 0; t_hdr = fm1_ms; draw_header(); }
        if (dirty_page && fm1_ms - t_page >= 30u) { dirty_page = 0; t_page = fm1_ms; draw_page(); }
        if (dirty_tags) { dirty_tags = 0; draw_tags(); }
        if (fm1_ms - t_led >= 40u) {
            uint32_t q = 0;
            t_led = fm1_ms;
            for (k = 0; k < NV; k++) q = q * 9u + ((uint32_t)ui_vlevel[k] >> 12);   /* 8 steps a LED: redraw on a change */
            if (is_seq_page(page)) draw_hints();   /* the step view: the hints instead of the voice LEDs */
            else if (q != leds_q) { leds_q = q; draw_leds(); }
            if (page == PG_GLOBAL && fm1_ms - t_page >= 250u) dirty_page = 1;
        }
        if (is_seq_page(page) || rec) {        /* the sequencer's strip instead of the scope */
            if (ui_seq_ticks != ticks_seen) { ticks_seen = ui_seq_ticks; dirty_steps = 1; }
            if (dirty_steps && fm1_ms - t_steps >= 30u) { dirty_steps = 0; t_steps = fm1_ms; draw_steps(); }
        } else if (scope_state == 3u && fm1_ms - t_scope >= 50u) {
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

        if (set_dirty && !ctl_seq_run && engine_quiet() && fm1_ms - set_t > 2000u) {   /* when silent: an erase stalls the sound */
            settings_save(&set, sizeof set);
            set_dirty = 0;
        }
        if (pat_dirty && !ctl_seq_run && engine_quiet()) {
            blob_save(set.pat, &pat, sizeof pat);
            pat_dirty = pat_edited = 0;
            dirty_page = 1;
        }
    }
}
