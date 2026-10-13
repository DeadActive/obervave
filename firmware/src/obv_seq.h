/* SPDX-License-Identifier: GPL-3.0-only */
/* The step sequencer: two tracks that share 256 steps (A from the front, B from the back, so a length change
 * never moves the other track's steps). A step holds a chord of up to four notes, a velocity, a gate (or a tie
 * into the next step), a ratchet (1-4 hits) and a chance. Each track has its own length, note value, swing and
 * transpose; both play the one OberVave sound, straight into the voices (not through the arpeggiator).
 * The player runs in the audio ISR per control block, as the arpeggiator does; the main loop edits `pat`. */
#define SEQ_STEPS 256u
#define SEQ_CHORD 4u
#define GATE_TIE 127u                           /* a gate of 127: the notes hold into the next step */
#define NPAT NBLOB                              /* patterns, one flash sector each */
#define PAT_MAGIC 0x31514553u                   /* "SEQ1" */

typedef struct { uint8_t note[SEQ_CHORD], vel, gate, ratch, chance; } step_t;   /* note 0 = none; ratch 0..3 = 1..4 hits */
typedef struct { uint16_t len; uint8_t div, swing; int8_t transp; uint8_t mute, pad[2]; } seqtrk_t;   /* swing 50..75 % */
typedef struct { uint32_t magic; seqtrk_t trk[2]; step_t st[SEQ_STEPS]; } pattern_t;

static pattern_t pat;                           /* the pattern now: the main loop edits, the ISR plays */
static step_t *seq_step(uint32_t t, uint32_t i) { return &pat.st[t ? SEQ_STEPS - 1u - i : i]; }

/* main loop -> ISR */
static volatile uint32_t ctl_seq_run, ctl_seq_restart;
/* ISR -> main loop: where each track is (step, and how far into it, Q8) */
static volatile uint32_t ui_seq_pos[2], ui_seq_frac[2], ui_seq_ticks;

/* ---- a fresh pattern, and the factory ones (D Phrygian, as the dark arpeggios) */
static void step_clear(step_t *s)
{
    uint32_t i;
    for (i = 0; i < SEQ_CHORD; i++) s->note[i] = 0;
    s->vel = 100;
    s->gate = 96;                               /* 76 % */
    s->ratch = 0;
    s->chance = 100;
}
static void pat_init(pattern_t *p)
{
    uint32_t i, t;
    p->magic = PAT_MAGIC;
    for (i = 0; i < SEQ_STEPS; i++) step_clear(&p->st[i]);
    for (t = 0; t < 2u; t++) {
        p->trk[t].len = t ? 0 : 16;             /* B off until it gets a length */
        p->trk[t].div = D_16;
        p->trk[t].swing = 50;
        p->trk[t].transp = 0;
        p->trk[t].mute = 0;
        p->trk[t].pad[0] = p->trk[t].pad[1] = 0;
    }
}
static void fac_put(pattern_t *p, uint32_t t, uint32_t i, uint32_t n1, uint32_t n2, uint32_t n3, uint32_t gate,
                    uint32_t ratch, uint32_t chance, uint32_t vel)
{
    step_t *s = &p->st[t ? SEQ_STEPS - 1u - i : i];
    s->note[0] = (uint8_t)n1; s->note[1] = (uint8_t)n2; s->note[2] = (uint8_t)n3; s->note[3] = 0;
    s->gate = (uint8_t)gate; s->ratch = (uint8_t)ratch; s->chance = (uint8_t)chance; s->vel = (uint8_t)vel;
}
static void pat_factory(uint32_t n, pattern_t *p)
{
    uint32_t i;
    pat_init(p);
    if (n == 0u) {                              /* DARK RUN: a climbing 16th line over a bass in eighths */
        static const uint8_t A[16] = {62, 57, 65, 69, 74, 69, 65, 57, 62, 58, 65, 70, 74, 70, 65, 63};
        static const uint8_t B[8] = {38, 38, 50, 38, 38, 39, 50, 36};
        p->trk[0].len = 16; p->trk[0].div = D_16;
        p->trk[1].len = 8; p->trk[1].div = D_8;
        for (i = 0; i < 16u; i++)
            fac_put(p, 0, i, A[i], 0, 0, i % 4u == 3u ? 50 : 80, i == 7u || i == 15u ? 1 : 0, i % 8u == 5u ? 60 : 100, i % 4u == 0u ? 118 : 92);
        for (i = 0; i < 8u; i++)
            fac_put(p, 1, i, B[i], 0, 0, i == 3u ? GATE_TIE : 100, i == 6u ? 1 : 0, 100, i % 2u ? 96 : 120);
    } else if (n == 1u) {                       /* CHORDS: four chords tied over two eighths each, a ratcheting bass */
        static const uint8_t C[4][3] = {{62, 65, 69}, {58, 62, 65}, {60, 63, 67}, {57, 62, 65}};
        static const uint8_t R[4] = {38, 34, 36, 33};
        p->trk[0].len = 8; p->trk[0].div = D_8; p->trk[0].swing = 58;
        p->trk[1].len = 16; p->trk[1].div = D_16;
        for (i = 0; i < 8u; i++)
            fac_put(p, 0, i, C[i / 2u][0], C[i / 2u][1], C[i / 2u][2], i % 2u ? 70 : GATE_TIE, 0, 100, 104);
        for (i = 0; i < 16u; i++)
            fac_put(p, 1, i, R[i / 4u] + (i % 4u == 2u ? 12u : 0u), 0, 0, 60, i % 4u == 3u ? 2 : 0, i % 4u == 1u ? 70 : 100, i % 4u ? 92 : 122);
    }
}

/* ---- the player (ISR, per CTL block) */
typedef struct { uint32_t cnt, dur, step, sub, rat_n, rat_i, gate_at, nsnd, play, tie; uint8_t snd[SEQ_CHORD]; } seqrun_t;
static seqrun_t sr[2];
static uint32_t seq_was;

static void seq_release(seqrun_t *r)
{
    uint32_t i;
    for (i = 0; i < r->nsnd; i++) voice_release(r->snd[i], 1);
    r->nsnd = 0;
}
static uint32_t seq_note(const step_t *s, uint32_t i, int32_t transp)
{
    return (uint32_t)clampi((int32_t)s->note[i] + transp, 1, 127);
}
static void seq_sound(seqrun_t *r, const step_t *s, int32_t transp)
{
    uint32_t i;
    for (i = 0; i < SEQ_CHORD && s->note[i] && r->nsnd < SEQ_CHORD; i++) {
        uint32_t n = seq_note(s, i, transp);
        voice_trigger(n, s->vel ? s->vel : 100u);
        r->snd[r->nsnd++] = (uint8_t)n;
    }
}
static uint32_t seq_same(const seqrun_t *r, const step_t *s, int32_t transp)   /* the step plays what sounds now */
{
    uint32_t i, j, n = 0;
    for (i = 0; i < SEQ_CHORD && s->note[i]; i++, n++) {
        uint32_t m = seq_note(s, i, transp);
        for (j = 0; j < r->nsnd && r->snd[j] != m; j++) ;
        if (j == r->nsnd) return 0;
    }
    return n == r->nsnd;
}

static void seq_tick(void)
{
    uint32_t t, q;
    if (!ctl_seq_run) {
        if (seq_was) { seq_release(&sr[0]); seq_release(&sr[1]); seq_was = 0; }
        return;
    }
    if (!seq_was || ctl_seq_restart) {          /* start: step 0 of both tracks in this block */
        ctl_seq_restart = 0;
        for (t = 0; t < 2u; t++) {
            seq_release(&sr[t]);
            sr[t].step = 0xFFFFFFFFu;
            sr[t].cnt = sr[t].dur = 0;
            sr[t].play = sr[t].tie = 0;
        }
        seq_was = 1;
    }
    q = ctl_ext_quarter ? ctl_ext_quarter : 60u * FS / clampi(ctl_tempo, 40, 240);
    for (t = 0; t < 2u; t++) {
        seqrun_t *r = &sr[t];
        const seqtrk_t *k = &pat.trk[t];
        const step_t *s;
        uint32_t len = k->len;
        if (!len) { seq_release(r); r->tie = 0; continue; }
        if (r->cnt >= r->dur) {                 /* the next step */
            uint32_t base = q / DIV_PER_QUARTER[k->div % NDIV], sw;
            r->cnt -= r->dur;
            r->step = r->step + 1u >= len ? 0u : r->step + 1u;
            sw = base * (uint32_t)(clampi(k->swing, 50, 75) - 50) / 50u;
            r->dur = r->step & 1u ? base - sw : base + sw;   /* swing: the first of each pair longer */
            if (r->dur < CTL) r->dur = CTL;
            s = seq_step(t, r->step);
            r->play = s->note[0] && !k->mute && (s->chance >= 100u || xorshift(&rnd_state) % 100u < s->chance);
            if (!(r->tie && r->play && seq_same(r, s, k->transp))) {   /* a tie into the same notes holds them */
                seq_release(r);
                if (r->play) seq_sound(r, s, k->transp);
            }
            r->rat_n = (s->ratch & 3u) + 1u;
            r->sub = r->dur / r->rat_n;
            r->rat_i = 0;
            r->tie = r->play && s->gate >= GATE_TIE;
            r->gate_at = s->gate >= GATE_TIE ? 0xFFFFFFFFu : (uint32_t)clampi((int32_t)((r->sub * (s->gate + 1u)) >> 7), CTL, (int32_t)r->sub);
            ui_seq_ticks++;
        } else {
            s = seq_step(t, r->step < len ? r->step : 0u);
            if (r->play && r->rat_n > 1u && r->sub) {   /* a ratchet: the next hit */
                uint32_t i = r->cnt / r->sub;
                if (i > r->rat_i && i < r->rat_n) { r->rat_i = i; seq_release(r); seq_sound(r, s, k->transp); }
            }
        }
        if (r->nsnd && r->gate_at != 0xFFFFFFFFu && r->cnt - r->rat_i * r->sub >= r->gate_at) seq_release(r);
        r->cnt += CTL;
        ui_seq_pos[t] = r->step;
        ui_seq_frac[t] = r->dur ? r->cnt * 256u / r->dur : 0u;
    }
}
