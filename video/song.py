#!/usr/bin/env python3
"""The OberVave video's music: nine OberVaves (the real firmware's host build, firmware/src/obervave.c), each with a
script of notes, CCs and knob turns. A minor, 108 BPM, 32 bars. Writes scripts/<part>.txt, runs the host build (tests/run_host.sh builds it)
(the filmed parts at 30 fps: f/<part>/NNNNN.png + f/<part>-log.txt), mixes stems/*.wav into track.wav.

  python3 song.py [--scripts-only]"""
import subprocess, sys, wave, shutil
from pathlib import Path
import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
OBV = HERE.parent
HOST = OBV / "build" / "host" / "obervave"
BPM, T0 = 108, 2000                                   # the splash takes 1.2 s; the music starts at T0 ms
BEAT = 60000 / BPM
BARS = 32
END = T0 + int(BARS * 4 * BEAT) + 3500

PARAMS = ("O1_WAVE O1_PW O1_LVL NOISE O2_WAVE O2_PW O2_LVL O2_SEMI SYNC XMOD ENV_O2 O2_FINE CUTOFF RESO FENV_AMT KEYTRK "
          "FTYPE DRIVE VEL_FLT VEL_AMP FA FD FS FR AA AD AS AR LFO_SHAPE LFO_RATE LFO_DELAY LFO_PHASE LFO_PITCH LFO_PWM "
          "LFO_FLT LFO_AMP LFO_PITCH_TO LFO_PWM_TO MODE UNI_N UNI_DET GLIDE VINTAGE SPREAD BEND VOLUME MOD_DEPTH "
          "ARP_MODE ARP_DIV ARP_OCT ARP_GATE").split()
PMAX = {"O1_WAVE": 3, "O2_WAVE": 3, "O2_SEMI": 48, "SYNC": 1, "FTYPE": 4, "LFO_SHAPE": 5, "LFO_PHASE": 2, "LFO_PITCH_TO": 2,
        "LFO_PWM_TO": 2, "MODE": 3, "UNI_N": 8, "BEND": 24, "ARP_MODE": 5, "ARP_DIV": 6, "ARP_OCT": 4}
FIXED = {"CUTOFF": 74, "RESO": 71, "AA": 73, "AR": 72, "GLIDE": 5, "MOD_DEPTH": 1}
CC = dict((n, c) for n, c in FIXED.items())
nxt = 20
for n in PARAMS:
    if n in FIXED: continue
    while nxt in CC.values() or nxt == 64: nxt += 1
    CC[n] = nxt; nxt += 1
PROG = {"BRASS": 0, "POLYBRASS": 1, "SYNCLEAD": 2, "UNISON": 3, "PWM": 4, "FATBASS": 6, "SWEEPPAD": 8, "ARPPLUCK": 22, "INIT": 31}

def ccval(name, v):                                   # the CC that sets parameter `name` to v (the app scales by max)
    m = PMAX.get(name, 127)
    if m == 127: return v
    d = (127 * v) // m
    while (d * m + 63) // 127 < v: d += 1
    assert (d * m + 63) // 127 == v
    return d

class Part:
    def __init__(self, name, film):
        self.name, self.film, self.ev = name, film, []
    def at(self, bar, beat=0.0): return T0 + int((bar * 4 + beat) * BEAT)
    def note(self, bar, beat, n, vel, dur_beats):
        t = self.at(bar, beat)
        self.ev.append((t, f"midi 0x90 {n} {vel}"))
        self.ev.append((t + int(dur_beats * BEAT) - 2, f"midi 0x80 {n} 0"))
    def chord(self, bar, beat, notes, vel, dur): [self.note(bar, beat, n, vel, dur) for n in notes]
    def cc(self, t, name, v): self.ev.append((t, f"midi 0xB0 {CC[name]} {ccval(name, v)}"))
    def set(self, t, **kv):
        for k, v in kv.items(): self.cc(t, k, v); t += 1
    def pc(self, t, prog): self.ev.append((t, f"midi 0xC0 {prog} 0"))
    def btn(self, t, b): self.ev.append((t, f"btn {b}"))
    def enc(self, t, e, n): self.ev.append((t, f"enc {e} {n}"))
    def sweep(self, t0, t1, name, v0, v1, step_ms=20):   # a CC ramp
        n = max(1, (t1 - t0) // step_ms)
        for i in range(n + 1): self.cc(t0 + i * step_ms, name, int(round(v0 + (v1 - v0) * i / n)))
    def bend(self, t0, t1, b0, b1, step_ms=15):           # pitch bend ramp, -8192..8191
        n = max(1, (t1 - t0) // step_ms)
        for i in range(n + 1):
            b = int(round(b0 + (b1 - b0) * i / n)) + 8192
            self.ev.append((t0 + i * step_ms, f"midi 0xE0 {b & 127} {b >> 7}"))
    def write(self):
        self.ev.sort(key=lambda e: e[0])
        p = HERE / "scripts" / f"{self.name}.txt"
        p.parent.mkdir(exist_ok=True)
        p.write_text("\n".join(f"{t} {s}" for t, s in self.ev) + f"\n{END} end\n")
        return p

# ---- the music. Chords: | Am7 | Fmaj7 | Dm7 | E7 | (A minor); a 4-bar turnaround, 8 times
AM7, FMAJ7, DM7, E7 = [57, 60, 64, 67], [53, 57, 60, 64], [50, 53, 57, 60], [52, 56, 59, 62]
PROGRESSION = [AM7, FMAJ7, DM7, E7]
ROOT = [33, 29, 26, 28]                                # A1 F1 D1 E1
def chord_of(bar): return PROGRESSION[bar % 4]
def root_of(bar): return ROOT[bar % 4]

parts = {}
def P(name, film=False): parts[name] = Part(name, film); return parts[name]

# PAD: SWEEP PAD, whole-note chords from bar 0, the filter opens over the first 8 bars, closes at the end
pad = P("pad", film=True)
pad.pc(T0 - 1500, PROG["SWEEPPAD"]); pad.set(T0 - 1400, VOLUME=110, AA=70, AR=100, FA=90, CUTOFF=30, SPREAD=120)
for bar in range(0, 32):
    if bar in (17, 18, 19, 20): continue             # the breakdown: the strings take over
    pad.chord(bar, 0, [n + 12 for n in chord_of(bar)], 90, 4.0 if bar < 31 else 6.0)
pad.sweep(pad.at(0), pad.at(8), "CUTOFF", 30, 70)
pad.sweep(pad.at(24), pad.at(28), "CUTOFF", 70, 85)
pad.sweep(pad.at(29), pad.at(32), "CUTOFF", 85, 20)
pad.btn(T0 - 800, 6); pad.btn(T0 - 750, 6); pad.btn(T0 - 700, 6); pad.btn(T0 - 650, 6)   # EDIT x4: the FILTER page

# ARP: ARP PLUCK (UP/DN 1/16, 3 octaves); the chord held a bar each. Bars 0-16, 17-20 (alone with strings), 21-28
arp = P("arp", film=True)
arp.pc(T0 - 1500, PROG["ARPPLUCK"]); arp.set(T0 - 1400, ARP_OCT=2, ARP_GATE=45, CUTOFF=55, VOLUME=100, AR=45)
for bar in range(0, 29):
    arp.chord(bar, 0, chord_of(bar), 100, 3.98)
arp.sweep(arp.at(12), arp.at(16), "CUTOFF", 55, 80)
arp.sweep(arp.at(16), arp.at(17), "CUTOFF", 80, 55)
arp.btn(T0 - 800, 10)                                 # the ARP page
arp.btn(T0 - 900, 0)                                  # OCT -1

# BASS: FAT BASS (mono, glide), a riff a bar, from bar 4; RESO knob up (PERFORM, knob 1) in bar 7, back in bar 8
bass = P("bass", film=True)
bass.pc(T0 - 1500, PROG["FATBASS"]); bass.set(T0 - 1400, VOLUME=115, CUTOFF=38, DRIVE=50, GLIDE=12)
RIFF = [(0, 0, .5, 110), (.75, 0, .25, 90), (1.5, 3, .25, 100), (2, 0, .5, 110), (2.75, -2, .25, 90), (3.25, 0, .25, 95), (3.5, 2, .5, 100)]
for bar in range(4, 32):
    if 17 <= bar <= 20: continue
    r = root_of(bar)
    for beat, iv, dur, vel in RIFF:
        if bar == 31 and beat > 0: break
        bass.note(bar, beat, r + iv, vel, dur if bar < 31 else 4.0)
for i in range(14): bass.enc(bass.at(7) + i * 60, 2, 1)      # RESO up 14 detents (28) over 0.8 s
for i in range(14): bass.enc(bass.at(8, 2) + i * 60, 2, -1)
bass.btn(T0 - 900, 0); bass.btn(T0 - 850, 0)                 # OCT -2: the riff lights the keys
bass.sweep(bass.at(27), bass.at(28), "CUTOFF", 38, 70)
bass.sweep(bass.at(28), bass.at(28, 2), "CUTOFF", 70, 38)

# BRASS: BRASS STAB, stabs on the off-beats, bars 8-16 and 25-28; ENV AMT knob (PERFORM, knob 2) worked in bar 10-11
brass = P("brass", film=True)
brass.pc(T0 - 1500, PROG["BRASS"]); brass.set(T0 - 1400, VOLUME=105, CUTOFF=55, SPREAD=90)
STABS = [(1.5, .35, 115), (3.0, .35, 110), (3.75, .25, 100)]
for bar in list(range(8, 16)) + list(range(24, 28)):
    for beat, dur, vel in STABS:
        brass.chord(bar, beat, chord_of(bar), vel, dur)
    if bar % 4 == 3: brass.chord(bar, 3.5, chord_of(bar + 1), 110, .25)   # a pickup into the next chord
brass.chord(28, 0, [n + 12 for n in AM7], 120, 1.5)
for i in range(14): brass.enc(brass.at(10) + i * 80, 3, -1)   # ENV AMT down (duller), then back up
for i in range(14): brass.enc(brass.at(11) + i * 80, 3, 1)
brass.btn(T0 - 900, 0)                                        # OCT -1

# LEAD: SYNC LEAD hook bars 12-16 (the cutoff sweeps with it), then UNISON LD solo bars 20-24 with a bend,
# and the hook again over the full band bars 24-28 (sync lead)
lead = P("lead", film=True)
lead.pc(T0 - 1500, PROG["SYNCLEAD"]); lead.set(T0 - 1400, VOLUME=100, CUTOFF=70, AR=45)
lead.btn(T0 - 900, 1)                                 # OCT +1
HOOK = [(0, 0, 76, .75), (0, .75, 79, .75), (0, 1.5, 81, 1.0), (0, 2.5, 79, .5), (0, 3, 76, 1.0),
        (1, 0, 72, .75), (1, .75, 74, .75), (1, 1.5, 76, 1.5), (1, 3, 74, .5), (1, 3.5, 72, .5),
        (2, 0, 69, .75), (2, .75, 72, .75), (2, 1.5, 74, 1.0), (2, 2.5, 76, .5), (2, 3, 74, 1.0),
        (3, 0, 71, 1.0), (3, 1, 68, .5), (3, 1.5, 71, .5), (3, 2, 76, 2.0)]
for base in (12, 24):
    for b, beat, n, dur in HOOK:
        lead.note(base + b, beat, n, 105, dur * .95)
lead.sweep(lead.at(12), lead.at(14), "CUTOFF", 70, 100)
lead.sweep(lead.at(14), lead.at(16), "CUTOFF", 100, 60)
lead.btn(lead.at(12) - 300, 6); lead.btn(lead.at(12) - 250, 6); lead.btn(lead.at(12) - 200, 6)   # OSC MOD: SYNC ON
lead.pc(lead.at(20) - 400, PROG["UNISON"]); lead.set(lead.at(20) - 380, VOLUME=95, CUTOFF=80, UNI_N=6, UNI_DET=60)
lead.btn(lead.at(20) - 300, 2)                        # FX: the VOICE page (UNISON, 6 voices)
SOLO = [(20, 0, 69, .5), (20, .5, 72, .5), (20, 1, 74, .5), (20, 1.5, 76, 1.5), (20, 3, 74, .5), (20, 3.5, 72, .5),
        (21, 0, 69, 1.0), (21, 1.5, 67, .5), (21, 2, 69, 2.0),
        (22, 0, 76, .5), (22, .5, 79, .5), (22, 1, 81, 2.0), (22, 3, 79, .5), (22, 3.5, 76, .5),
        (23, 0, 74, 1.0), (23, 1, 72, .5), (23, 1.5, 71, .5), (23, 2, 69, 2.0)]
for b, beat, n, dur in SOLO:
    lead.note(b, beat, n, 110, dur * .97)
lead.bend(lead.at(22, 1), lead.at(22, 1.75), -8192 // 2 * 0, 0)        # (no bend before)
lead.bend(lead.at(22, 2.2), lead.at(22, 2.9), 0, 8191)                 # up a whole tone (BEND 2)
lead.bend(lead.at(23, 2), lead.at(23, 3.8), 0, -8192)                  # down at the end
lead.bend(lead.at(23, 3.9), lead.at(24) - 50, -8192, 0)
lead.pc(lead.at(24) - 300, PROG["SYNCLEAD"]); lead.set(lead.at(24) - 280, VOLUME=100, CUTOFF=85, AR=45)
lead.btn(lead.at(24) - 250, 8)                        # HOME
lead.note(28, 0, 81, 110, 2.0)

# STRINGS: PWM STRING, the breakdown bars 16-20 (and under the solo 20-24)
strings = P("strings", film=True)
strings.pc(T0 - 1500, PROG["PWM"]); strings.set(T0 - 1400, VOLUME=100, CUTOFF=68, AA=55)
for bar in range(16, 24):
    strings.chord(bar, 0, [n + 12 for n in chord_of(bar)], 90, 4.0)
strings.chord(28, 0, [n + 12 for n in AM7], 90, 8.0)

# DRUMS: three OberVaves from INIT. Kick: the resonant 12 dB filter rings like a sine, the envelope drops its pitch.
kick = P("kick")
kick.pc(T0 - 1500, PROG["INIT"])
kick.set(T0 - 1400, MODE=3, O1_WAVE=1, O1_LVL=55, O2_WAVE=0, NOISE=0, FTYPE=0, CUTOFF=24, RESO=125, FENV_AMT=64 + 48,
         FA=0, FD=62, FS=0, AA=0, AD=84, AS=0, AR=40, KEYTRK=0, VEL_FLT=0, VEL_AMP=30, DRIVE=40, VINTAGE=0, SPREAD=0, VOLUME=127, GLIDE=0)
snare = P("snare")
snare.pc(T0 - 1500, PROG["INIT"])
snare.set(T0 - 1400, MODE=3, O1_WAVE=0, O2_WAVE=1, O2_LVL=45, NOISE=127, FTYPE=2, CUTOFF=78, RESO=35, FENV_AMT=64 + 24,
          FA=0, FD=55, FS=0, AA=0, AD=74, AS=0, AR=40, KEYTRK=0, VEL_FLT=40, VEL_AMP=50, DRIVE=25, VINTAGE=0, SPREAD=0, VOLUME=120)
hat = P("hat")
hat.pc(T0 - 1500, PROG["INIT"])
hat.set(T0 - 1400, MODE=3, O1_WAVE=0, O2_WAVE=0, NOISE=127, FTYPE=1, CUTOFF=118, RESO=25, FENV_AMT=64,
        FA=0, AA=0, AD=58, AS=90, AR=52, KEYTRK=0, VEL_AMP=80, VINTAGE=0, SPREAD=0, VOLUME=100)
KICKS = [(0, 115), (1.5, 95), (2.5, 110), (3.75, 85)]
for bar in range(4, 31):
    if 17 <= bar <= 20: continue
    for beat, vel in KICKS:
        if bar in (15, 27) and beat > 2: break         # a fill: the snare takes bar 15 / 27's end
        kick.note(bar, beat, 36, vel, .5)
    snare.note(bar, 1, 57, 110, .3); snare.note(bar, 3, 57, 112, .3)
    if bar % 4 == 3: snare.note(bar, 3.75, 57, 70, .2)
    if bar in (15, 27):
        for i in range(4): snare.note(bar, 2 + i * .5, 57, 80 + i * 12, .2)
    for i in range(8):
        beat = i * .5
        hat.note(bar, beat, 70, 100 if i % 2 == 0 else 70, .12 if i != 7 else .45)
kick.note(31, 0, 36, 120, .5); snare.note(31, 0, 57, 100, .3)
for bar in range(0, 4):                               # the intro: hats only, quiet
    for i in range(8): hat.note(bar, i * .5, 70, 60 if i % 2 == 0 else 40, .12)

GAIN = {"pad": .5, "arp": 2.4, "bass": 1.5, "brass": 2.2, "lead": 1.9, "strings": .6, "kick": 3.6, "snare": 5.5, "hat": 1.5}

def run(part):
    script = part.write()
    out = HERE / "stems" / part.name
    args = [str(HOST), str(out), str(script)] + (["30"] if part.film else [])
    subprocess.run(["nice", "-n", "15"] + args, check=True, stdout=subprocess.DEVNULL)
    if part.film:                                     # ppm -> png, the log beside them
        fd = HERE / "f" / part.name
        if fd.exists(): shutil.rmtree(fd)
        fd.mkdir(parents=True)
        for i, p in enumerate(sorted((HERE / "stems").glob(f"{part.name}-*.ppm"))):
            Image.open(p).save(fd / f"{i:05d}.png"); p.unlink()
        shutil.move(str(out) + "-log.txt", HERE / "f" / f"{part.name}-log.txt")
    with wave.open(str(out) + ".wav") as w:
        a = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).reshape(-1, 2).astype(np.float32) / 32768
    return a

def main():
    for p in parts.values(): p.write()
    if "--scripts-only" in sys.argv: return
    stems = {n: run(p) for n, p in parts.items()}
    n = max(len(a) for a in stems.values())
    mix = np.zeros((n, 2), np.float32)
    for name, a in stems.items():
        rms = float(np.sqrt((a ** 2).mean())); pk = float(np.abs(a).max())
        print(f"{name:8s} rms {20 * np.log10(rms + 1e-9):6.1f} dB  peak {20 * np.log10(pk + 1e-9):6.1f} dB  gain {GAIN[name]}")
        mix[:len(a)] += a * GAIN[name]
    mix = mix[int(T0 * 44.1):]                        # the music starts at T0
    pk = float(np.abs(mix).max())
    print(f"mix peak {20 * np.log10(pk):.1f} dB before the master")
    mix *= 0.7 / max(pk, 1e-9)                      # -3 dB peak; the video's ffmpeg limiter does the rest
    with wave.open(str(HERE / "track.wav"), "wb") as w:
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(44100); w.writeframes((mix * 32767).astype(np.int16).tobytes())
    print("track.wav", len(mix) / 44100, "s")

if __name__ == "__main__":
    main()
