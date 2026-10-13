#!/usr/bin/env python3
"""The OberVave video's music: nine OberVaves (the real firmware's host build, firmware/src/obervave.c), each with a
script of notes, CCs and knob turns. E minor, 100 BPM, 32 bars: dark arpeggios, echoes and a plate. Writes scripts/<part>.txt, runs the host build (tests/run_host.sh builds it)
(the filmed parts at 30 fps: f/<part>/NNNNN.png + f/<part>-log.txt), mixes stems/*.wav into track.wav.

  python3 song.py [--scripts-only]"""
import subprocess, sys, wave, shutil
from pathlib import Path
import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
OBV = HERE.parent
HOST = OBV / "build" / "host" / "obervave"
BPM, T0 = 100, 2000                                   # the splash takes 1.2 s; the music starts at T0 ms
BEAT = 60000 / BPM
BARS = 36                                             # 32 and the last chord's tail
END = T0 + int(BARS * 4 * BEAT) + 2500

PARAMS = ("O1_WAVE O1_PW O1_LVL NOISE O2_WAVE O2_PW O2_LVL O2_SEMI SYNC XMOD ENV_O2 O2_FINE CUTOFF RESO FENV_AMT KEYTRK "
          "FTYPE DRIVE VEL_FLT VEL_AMP FA FD FS FR AA AD AS AR LFO_SHAPE LFO_RATE LFO_DELAY LFO_PHASE LFO_PITCH LFO_PWM "
          "LFO_FLT LFO_AMP LFO_PITCH_TO LFO_PWM_TO MODE UNI_N UNI_DET GLIDE VINTAGE SPREAD BEND VOLUME MOD_DEPTH "
          "ARP_MODE ARP_DIV ARP_OCT ARP_GATE FMORPH CHO_RATE CHO_DEPTH CHO_MIX DLY_TIME DLY_FB DLY_TONE DLY_MIX "
          "REV_SIZE REV_DAMP REV_PRE REV_MIX").split()
PMAX = {"O1_WAVE": 3, "O2_WAVE": 3, "O2_SEMI": 48, "SYNC": 1, "FTYPE": 4, "LFO_SHAPE": 5, "LFO_PHASE": 2, "LFO_PITCH_TO": 2,
        "LFO_PWM_TO": 2, "MODE": 3, "UNI_N": 8, "BEND": 24, "ARP_MODE": 5, "ARP_DIV": 6, "ARP_OCT": 4, "FTYPE": 5, "DLY_TIME": 6}
FIXED = {"CUTOFF": 74, "RESO": 71, "AA": 73, "AR": 72, "GLIDE": 5, "MOD_DEPTH": 1}
CC = dict((n, c) for n, c in FIXED.items())
nxt = 20
for n in PARAMS:
    if n in FIXED: continue
    while nxt in CC.values() or nxt == 64: nxt += 1
    CC[n] = nxt; nxt += 1
PROG = {"DARKARP": 0, "SYNCLEAD": 2, "UNISON": 3, "PWM": 4, "FATBASS": 6, "SQUARELD": 15, "UPSIDEARP": 25, "NEONPAD": 28, "INIT": 31}

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

# ---- the music. E minor, four bars round and round: | Em7 | Cmaj7 | Am7 | B7 |
EM7, CMAJ7, AM7, B7 = [52, 55, 59, 62], [48, 52, 55, 59], [45, 48, 52, 55], [47, 51, 54, 57]
PROGRESSION = [EM7, CMAJ7, AM7, B7]
ROOT = [28, 24, 21, 23]                                # E1 C1 A0 B0
def chord_of(bar): return PROGRESSION[bar % 4]
def root_of(bar): return ROOT[bar % 4]

parts = {}
def P(name, film=False): parts[name] = Part(name, film); return parts[name]

# ARP: DARK ARP, the backbone: the chord held a bar, climbed in 16ths, the filter opens over 16 bars and shuts at the end
arp = P("arp", film=True)
arp.pc(T0 - 1500, PROG["DARKARP"]); arp.set(T0 - 1400, CUTOFF=38, VOLUME=105)
for bar in range(0, 32):
    arp.chord(bar, 0, chord_of(bar), 100, 3.98)
arp.chord(32, 0, EM7, 100, 0.98)
arp.sweep(arp.at(0), arp.at(16), "CUTOFF", 38, 62)
arp.sweep(arp.at(28), arp.at(32), "CUTOFF", 62, 30)
arp.btn(T0 - 900, 0)                                  # OCT -1: the chords light the keys
arp.btn(T0 - 800, 10)                                 # the ARP page
arp.btn(arp.at(20) - 200, 2); arp.btn(arp.at(20) - 150, 2); arp.btn(arp.at(20) - 100, 2)   # FX x3: the REVERB page

# BASS: FAT BASS, the root a whole note from bar 4, eighth-note pulses from bar 12, whole notes again at the end
bass = P("bass", film=True)
bass.pc(T0 - 1500, PROG["FATBASS"]); bass.set(T0 - 1400, VOLUME=110, CUTOFF=34, DRIVE=45, GLIDE=8, RESO=20)
for bar in range(4, 32):
    r = root_of(bar)
    if bar < 12 or bar >= 28:
        bass.note(bar, 0, r, 110, 3.9)
    else:
        for i in range(8):
            bass.note(bar, i * .5, r + (12 if i in (3, 7) else 0), 105 if i % 2 == 0 else 85, .45)
bass.note(32, 0, 28, 115, 4.0)
bass.sweep(bass.at(24), bass.at(28), "CUTOFF", 34, 60)
bass.sweep(bass.at(28), bass.at(29), "CUTOFF", 60, 34)
bass.btn(T0 - 900, 0); bass.btn(T0 - 850, 0)          # OCT -2
bass.btn(T0 - 800, 6); bass.btn(T0 - 750, 6); bass.btn(T0 - 700, 6); bass.btn(T0 - 650, 6)   # EDIT x4: FILTER

# PAD: NEON PAD, whole-note chords an octave up from bar 8, the last one rings out
pad = P("pad", film=True)
pad.pc(T0 - 1500, PROG["NEONPAD"]); pad.set(T0 - 1400, VOLUME=78, CUTOFF=48)
for bar in range(8, 32):
    pad.chord(bar, 0, [n + 12 for n in chord_of(bar)], 90, 4.0)
pad.chord(32, 0, [n + 12 for n in EM7], 95, 10.0)
pad.sweep(pad.at(24), pad.at(28), "CUTOFF", 48, 64)
pad.btn(T0 - 800, 2)                                  # FX: the CHORUS page

# LEAD: SQUARE LD bars 16-24 (its echoes in the tempo), UNISON LD an octave up bars 28-32
lead = P("lead", film=True)
lead.pc(T0 - 1500, PROG["SQUARELD"]); lead.set(T0 - 1400, VOLUME=95, CUTOFF=66, AR=55, DLY_MIX=45, DLY_FB=45, REV_MIX=30, REV_SIZE=100)
lead.btn(T0 - 900, 1)                                 # OCT +1
MEL = [(0, 0, 64, 2.0), (0, 2, 67, 1.0), (0, 3, 66, 1.0), (1, 0, 62, 3.0), (1, 3, 59, 1.0),
       (2, 0, 60, 2.0), (2, 2, 64, 1.0), (2, 3, 62, 1.0), (3, 0, 59, 4.0),
       (4, 0, 64, 1.5), (4, 1.5, 67, .5), (4, 2, 71, 2.0), (5, 0, 69, 2.0), (5, 2, 67, 1.0), (5, 3, 66, 1.0),
       (6, 0, 67, 1.0), (6, 1, 64, 1.0), (6, 2, 62, 2.0), (7, 0, 64, 3.5)]
for b, beat, n, dur in MEL:
    lead.note(16 + b, beat, n, 105, dur * .92)
lead.btn(lead.at(16) - 300, 2); lead.btn(lead.at(16) - 250, 2)   # FX x2: the DELAY page
lead.pc(lead.at(28) - 400, PROG["UNISON"]); lead.set(lead.at(28) - 380, VOLUME=90, CUTOFF=72, UNI_N=6, UNI_DET=55, DLY_MIX=35, REV_MIX=35)
lead.btn(lead.at(28) - 300, 7); lead.btn(lead.at(28) - 250, 7)   # GLO x2: the VOICE page
for b, beat, n, dur in MEL[:9]:
    lead.note(28 + b, beat, n + 12, 110, dur * .95)
lead.note(32, 0, 76, 110, 4.0)
lead.bend(lead.at(31, 2), lead.at(31, 3.9), 0, -8192)            # the last note falls
lead.bend(lead.at(32) - 30, lead.at(32) - 10, -8192, 0)

# ARP2: UPSIDE ARP (the SEM filter), up and down over two octaves above the chord, bars 20-28; MORPH turned LP -> notch
arp2 = P("arp2", film=True)
arp2.pc(T0 - 1500, PROG["UPSIDEARP"]); arp2.set(T0 - 1400, VOLUME=80, CUTOFF=36)
for bar in range(20, 28):
    arp2.chord(bar, 0, [n + 12 for n in chord_of(bar)], 90, 3.98)
for i in range(16): arp2.enc(arp2.at(24) + i * 100, 5, 1)         # MORPH knob (FILTER +, knob 4) toward the notch
arp2.btn(T0 - 800, 6); arp2.btn(T0 - 750, 6); arp2.btn(T0 - 700, 6); arp2.btn(T0 - 650, 6); arp2.btn(T0 - 600, 6)   # EDIT x5: FILTER +

# STRINGS: PWM STRING high above, bars 24-32, the last chord long
strings = P("strings", film=True)
strings.pc(T0 - 1500, PROG["PWM"]); strings.set(T0 - 1400, VOLUME=85, CUTOFF=64, AA=60, REV_MIX=45)
for bar in range(24, 32):
    strings.chord(bar, 0, [n + 24 for n in chord_of(bar)], 85, 4.0)
strings.chord(32, 0, [n + 24 for n in EM7], 85, 10.0)
strings.btn(T0 - 900, 1)                              # OCT +1
strings.btn(T0 - 800, 5)                              # the LFO page (the PWM)

# DRUMS: three OberVaves from INIT, half time. Kick: the resonant 12 dB filter rings like a sine, the envelope drops its pitch.
kick = P("kick")
kick.pc(T0 - 1500, PROG["INIT"])
kick.set(T0 - 1400, MODE=3, O1_WAVE=1, O1_LVL=55, O2_WAVE=0, NOISE=0, FTYPE=0, CUTOFF=22, RESO=125, FENV_AMT=64 + 48,
         FA=0, FD=64, FS=0, AA=0, AD=88, AS=0, AR=40, KEYTRK=0, VEL_FLT=0, VEL_AMP=30, DRIVE=40, VINTAGE=0, SPREAD=0, VOLUME=127, GLIDE=0)
snare = P("snare")
snare.pc(T0 - 1500, PROG["INIT"])
snare.set(T0 - 1400, MODE=3, O1_WAVE=0, O2_WAVE=1, O2_LVL=50, NOISE=127, FTYPE=2, CUTOFF=70, RESO=30, FENV_AMT=64 + 24,
          FA=0, FD=60, FS=0, AA=0, AD=80, AS=0, AR=40, KEYTRK=0, VEL_FLT=40, VEL_AMP=50, DRIVE=25, VINTAGE=0, SPREAD=0, VOLUME=120,
          REV_SIZE=110, REV_DAMP=70, REV_MIX=45)
hat = P("hat")
hat.pc(T0 - 1500, PROG["INIT"])
hat.set(T0 - 1400, MODE=3, O1_WAVE=0, O2_WAVE=0, NOISE=127, FTYPE=1, CUTOFF=118, RESO=25, FENV_AMT=64,
        FA=0, AA=0, AD=55, AS=80, AR=50, KEYTRK=0, VEL_AMP=80, VINTAGE=0, SPREAD=0, VOLUME=90)
for bar in range(8, 32):
    kick.note(bar, 0, 36, 115, .5)
    if bar >= 12 and bar < 28: kick.note(bar, 2.5, 36, 95, .5)
    if bar >= 12:
        snare.note(bar, 2, 55, 112, .3)
        if bar % 4 == 3 and bar < 28: snare.note(bar, 3.5, 55, 75, .2)
    for i in range(8):
        if bar < 12 and i % 2: continue
        hat.note(bar, i * .5, 70, 95 if i % 2 == 0 else 60, .12 if i != 7 else .4)
    if bar in (19, 27):
        for i in range(4): snare.note(bar, 2 + i * .5, 55, 80 + i * 12, .2)
kick.note(32, 0, 36, 120, .5); snare.note(32, 0, 55, 105, .3)

GAIN = {"arp": 2.8, "bass": 1.6, "pad": 1.1, "lead": 1.7, "arp2": 2.2, "strings": 0.9, "kick": 2.6, "snare": 4.0, "hat": 1.1}

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
