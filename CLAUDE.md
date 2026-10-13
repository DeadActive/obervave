# OberVave

A firmware for the M-VAVE FM-1: ONE polyphonic analog-style synth in the spirit of the classic 8-voice American
poly synths of 1979-84 (OB-X / OB-8 era). Built on Dinghy (`../dinghy`, zvenson/dinghy, started from d44df7d).
GPL-3.0. Identity `FM-1_95N`. Name by the user ("OberVave": Ober... + M-VAVE).

## Hard rules

- Nothing from SLOOP / sloopDX / zp12 apps (no sequencer, no grooves, no other engines). Only this one synth,
  as deep as possible. Dinghy's platform (`platform.c`, `usb.c`, `ota.c`, hal/) stays as it is.
- No trademarks in the product: never "Oberheim", "OB-X", "OB-8" on screen, in patch names or in the release name.
  In docs only as "inspired by"; the marks belong to their owners. No artist or song names in patch names. The same
  for "Stranger Things" (Netflix): the user wants that series' dark-arpeggio sound (its theme was played on an
  Oberheim Two Voice, so this synth's family), the name stays OberVave, and the show is never named in the product.
- Keep the Felucca / SLOOP / Dinghy credits (LICENSING.md, README).
- Target: JieLi AC791N, no FPU, no libgcc: integers only, **no 64-bit division** (int64 multiply is fine),
  no constant in 0xFFC00000-0xFFD00000 (`tools/build.py` checks). RAM <= 96 KB, big buffers in `.pool`.
- Audio ISR: 256 frames per call at 44.1 kHz (5.8 ms). Stay under half of it. Measure on the device
  (`fm1_ticks()` around the render, show it on the GLOBAL page).

## Where things are

- `firmware/src/obervave.c`: the app (panel, MIDI, storage, what to draw when). Includes `platform.c` first, then
  `obv_dsp.h` (oscillators, SVF, tanh), `obv_params.h` (the parameter table, pages), `obv_fx.h` (chorus, ping-pong
  delay, plate reverb: after the voices in the ISR), `obv_engine.h` (voices, envelopes, LFO, arp: the ISR),
  `obv_patches.h` (32 factory programs), `obv_ui.h` (the drawing).
- Effects (`obv_fx.h`): all lines int16 in `.pool` (274 KB used of 336). Chorus: one line, two taps a quarter turn
  apart, 4-point Hermite reads. Delay: two 743 ms lines, ping-pong, time in note values of the tempo (1/16 .. 1/2,
  dotted, triplet), the read position slews like a tape when the time changes, a one-pole in the feedback (TONE).
  Reverb: Dattorro's plate (1997) at the full rate, two swaying allpasses in the tank, SIZE = decay 0.37 .. 0.96,
  DAMP, PRE-DLY up to 120 ms; `REV_DIV 2` compiles it at half the rate if the chip needs it (untested on the
  device). Host cost (instr/sample): chorus ~160, delay ~110, reverb ~550; worst case with all three ~2350.
  **The user's priority is maximum sound quality over CPU**: measure on the device before trading any of it.
- Filter type SEM (`F_SEM`): the 12 dB SVF's LP .. notch .. HP by MORPH (FILTER + page, knob 4), as the SEM's pot.
- `tools/gen_tables.py` -> `firmware/gen/obv_tables.h` (committed): pitch, SVF g, tanh, envelope / LFO / glide
  coefficients, pan, cos.
- `tests/host.c` + `tests/run_host.sh [script]`: the same app on the PC (WAV + screen PNGs, scripted keys /
  buttons / MIDI / `enc ID N` encoder turns). Film mode (`build/host/obervave OUT script FPS`) writes a frame every
  1/FPS s and `OUT-log.txt` (keys, key LEDs, button LEDs, encoder positions per frame). `OBV_PERF=1` prints the
  instructions per sample (Linux perf counter): the yardstick is sloopDX's engines at ~1000-1500; OberVave's worst
  case (8 voices, LP24, sync + x-mod, saw+pulse, drive) is ~1900, BRASS STAB ~1600. Not yet measured on the device:
  the GLOBAL page shows the render time in us (budget 5800, aim < 2900).
- MIDI CC: 74 cutoff, 71 reso, 73 amp attack, 72 amp release, 5 glide, 1 mod depth (the wheel and the knob are one
  value), 7 volume, 64 sustain, 120/123 all off; every other parameter from CC 20 up in table order, skipping 64
  and the fixed ones (`cc_map_init`): the first 50 parameters take 20..63 and 65 (ARP GATE), the appended ones
  66 MORPH, 67-69 chorus, 70 delay time, 75-77 delay, 78-81 reverb. Program change picks the program. New
  parameters go at the end of the table so these numbers stay.
- The video: `video/song.py` (nine OberVaves from the host build, E minor 100 BPM, 32 bars of dark arpeggios,
  echoes and plate; stems/, f/<part>/ frames + logs, track.wav) and `video/make_video.py` -> `video/obervave-1.1.mp4`.
  The renders are git-ignored. The host's splash takes 1.2 s: a script's first key must come after it (T0 = 2 s).
- `tools/build.py`: `./build.sh [--release X.Y]` -> `build/obervave[-X.Y].fwsc`, identity FM-1_95Y.
- Docs from Dinghy: docs/API.md (platform calls), docs/RULES.md (the chip), docs/MAKE-IT-YOURS.md.
- Workspace file: `../obervave.code-workspace` (opened in VSCodium Insiders, flatpak).

## Status (2026-10-13, third session)

Third session added the effects (chorus / delay / plate reverb, three FX pages), the SEM filter morph, the dark
arpeggio programs (01 DARK ARP, 26 UPSIDE ARP, 29 NEON PAD; BRASS STAB moved to 13) with effects on most of the
others, the blue look, and the second video. Patches are 80 bytes (`SET_VERSION` 2). Host-tested; the device
package builds (103 KB image, 21 KB RAM, 274 KB pool) but has **still not been run on an FM-1**: first thing next
time, install it, check the CPU meter (GLO page) with DARK ARP and with all three effects on a big patch, the
knobs' feel, the splash. If the render time is over ~4000 us, `REV_DIV 2` (half-rate reverb) is the first lever;
ask the user before taking any quality away. Open: the look's polish, MIDI clock is tempo-following only, the arp
steps on 16-frame blocks, the delay time does not follow an external clock's tempo changes smoothly.

## Plan (agreed in the first session, 2026-10-12)

Priorities from the user: **the audio engine first, the look second**. Max depth of the one synth.

### Engine (all in the ISR, control rate CTL = 16 frames)

- 8 voices. Modes: POLY (round-robin allocation over the 8 "voice cards", steal oldest released, then oldest),
  DUAL (2 detuned voices per note, 4 notes), UNISON (2-8 voices on one note, detune spread, pan spread), MONO
  (last-note priority, legato without retrigger). Glide in every mode (also poly).
- Per voice: OSC1 + OSC2, each SAW / PULSE / SAW+PULSE / OFF, PW + PWM from the LFO. OSC2: semis -24..+24,
  fine detune, HARD SYNC to OSC1, FILTER ENV -> OSC2 pitch (sync sweeps), X-MOD (OSC2 -> OSC1 linear FM, clamp
  the step >= inc/16). Noise (per-voice xorshift). Free-running phases, random start.
- Anti-aliasing: polyBLEP as discontinuity events with a one-sample delay, so natural wraps AND sync resets use
  the same code: a jump of height H at fraction t (time from the event to the current sample, 0..1) adds
  `prev += H/2 * t^2`, `cur -= H/2 * (1-t)^2`. Pulse = saw(ph) - saw(ph + pw) (two event streams, mean 0).
  One 32-bit division per wrap (t = ph / (inc >> 15)), as sloopdx dsp.c `blep`.
- Mixer -> DRIVE (tanh table) -> filter. Filter types: 12 dB state-variable (Simper trapezoidal, Q13 coefficients
  per block, as sloopdx dsp.c `tsvf_*`) LP / BP / HP / NOTCH with a soft saturation on the band-pass state (warm,
  bounded self-oscillation); 24 dB LP = two cascaded SVFs (resonance on one). Cutoff in Q8 semitones:
  base + filter env * amount (bipolar) + key track 0..100 % + LFO + velocity + per-voice drift; a generated
  table tan(pi f / fs) (tools/gen_tables.py -> committed header).
- Envelopes: filter ADSR + amp ADSR, analog RC curves (attack aims at ~1.3 and stops at 1.0), per-block
  coefficients from a time table 0.5 ms .. 10 s, amp interpolated per sample (no zipper).
- LFO (global like the originals, phase mode SYNC / FREE / SPREAD = per-voice phase offsets): TRI, SAW, RAMP,
  SQUARE, S&H, smooth random; rate 0.05-30 Hz, delay / fade-in per voice; amounts to pitch (OSC1 / OSC2 / both),
  PWM, filter, amp. Mod wheel (CC1) adds to pitch and filter depth.
- VINTAGE: per voice card fixed offsets (osc tune +-8 cents, cutoff +-1.5 semis, env times +-10 %) plus a slow
  random walk on pitch, all scaled by the knob. SPREAD: per-voice pan (alternating L/R, the originals' pan pots),
  equal-power gains. Output: sum -> gentle tanh "output stage". Nominal chord peak ~ -6 dBFS before MASTER.
- Pitch: Q8 semitones -> inc via a 12-entry octave table (Dinghy's INC_C9) x a 256-entry fine table, int64
  multiply, shift by octave. Pitch bend (range param), MIDI CC for every parameter (CC 74 cutoff, 71 reso,
  1 mod, 64 sustain, 5 glide, 73 attack, 72 release, 7 volume, the rest from CC 20 up), sustain.
- Arpeggiator in the ISR (sample-exact): UP, DOWN, UP/DN, RANDOM, AS PLAYED; 1-4 octaves; 1/4 .. 1/32 + triplets;
  gate %; tempo 40-240; latch. MIDI clock if 0xF8 reaches midi_get (check usb.c).
- Main loop -> ISR: a ring of note events {on/off, note, vel} (one writer each side); the ISR owns voices,
  held-note list, arp, sustain. ISR -> UI: per-voice level + note (volatile), and a scope capture (armed by the
  main loop, triggered on a rising zero crossing, stride from the last note's period).

### Patches

- A patch = a byte array indexed by a parameter table {label, min, max, display kind} + a 10-char name. The UI,
  MIDI CC, randomizer and storage all walk that table.
- 32 programs: factory set in const flash, user saves in the settings sector 0xFC000 (raise Dinghy's 256-byte
  `settings_*` limit to the sector, one static buffer). Save only when silent (the erase stalls the audio).
- Factory sounds to make it shine: big brass stab, poly brass swell, sync-sweep lead, unison lead, string
  machine-ish PWM pad, bell-ish x-mod, fat bass (mono), arp sequence, sweep pad, 12 dB band-pass "quack" etc.

### Controls

- PRESETS = program, ALGORITHM = CUTOFF (always), SELECT = page scroll, KNOB 1-4 = the page's four parameters.
- HOME = PERFORM (RESO, F.ENV AMT, A.RELEASE, MOD DEPTH). EDIT cycles OSC1 / OSC2 / OSC MOD / FILTER / FILTER +,
  ENV cycles FILTER ENV / AMP ENV, LFO cycles LFO / LFO DEST / LFO +, FX cycles CHORUS / DELAY / REVERB,
  ARP = arp on/off + its page, GLO cycles GLOBAL (TEMPO, MIDI CH, TUNE, CPU meter) / VOICE (mode, unison, glide) /
  OUTPUT (vintage, spread, bend, volume).
- PLAY = HOLD (latch / sustain), SEQ = UNISON quick toggle, REC = randomize (in useful ranges), SAVE = save
  (twice to confirm, PRESETS picks the slot), OCT- / OCT+ = octave. Key LEDs show sounding notes, page buttons lit.

### Look (240 x 240)

- Own design, no copied trade dress, in the colours of the originals (the user: "the Oberheim is blue"): blue-black
  panel, near-white legends, light blue accent, red LED 7-segment program number (lit and dim segments, drawn with
  rects), "OBERVAVE" in FONT_L with three blue bands through the lower half of the letters, section titles between
  thin rules like a silkscreen.
- 4 anti-aliased round knobs (coverage by distance, blended RGB565) with a blue value arc and a pointer, label
  above, value below. A scope strip (blue trace on near black). 8 round red voice LEDs glowing with each voice's
  amp envelope, tags for mode (POLY/DUAL/UNI/MONO), filter type, ARP, HOLD, FX.
- Splash at boot (~1.2 s): the stripes sweep in. Redraw only what changed; scope + LEDs ~20-25 fps.
  Raise `CV_MAX` (canvas in .pool) for bigger strips.

## Style

As Dinghy / SLOOP: C99, 4 spaces, short comments that say why, unity build. Commit messages: one line what changed.
Commit after each tested step. Host test before every device build: listen to / look at `build/host/*`.
Not on GitHub yet: ask the user before creating the public repo.
