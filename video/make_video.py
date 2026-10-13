#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""OberVave video 1.1 (1920 x 1080, 30 fps, ~110 s): nine OberVaves play one slow dark track (song.py: the real firmware's
host build, all nine screens filmed at 30 fps, its audio). A shot list cuts between them on the bars: one in focus on a
drawn FM-1 (screen, knobs, buttons, keys lit as the firmware lights them), two side by side, the drum trio, the wall of
nine. The kick pulses the picture, the clap flashes it, every cut tears the colours for three frames.
  python3 song.py && python3 make_video.py [--stills]"""
import math, subprocess, sys, wave
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont, ImageChops

HERE = Path(__file__).resolve().parent
A = HERE
OUT = HERE / "obervave-1.1.mp4"
W, H, FPS = 1920, 1080, 30
BPM, T0 = 94, 3.0
BAR = 4 * 60 / BPM
BG, BG2, PANEL = (10, 13, 22), (18, 23, 36), (30, 36, 52)                 # blue-black, as the firmware's screen
CREAM, DIM, BLUE, RED = (226, 232, 240), (120, 142, 172), (96, 176, 255), (255, 44, 32)
STRIPES = [(30, 70, 190), (60, 130, 235), (120, 190, 255)]
IF = "/run/host/fonts/rsms-inter-fonts/"
def font(n, s): return ImageFont.truetype(IF + n, int(s))
F_LIGHT = lambda s: font("InterDisplay-Light.ttf", s)
F_BOLD = lambda s: font("InterDisplay-Bold.ttf", s)
F_REG = lambda s: font("Inter-Regular.ttf", s)
F_SEMI = lambda s: font("Inter-SemiBold.ttf", s)

PARTS = ["arp", "bass", "pad", "lead", "arp2", "strings", "kick", "clap", "hat"]
LOG = {p: [tuple(int(x) for x in l.split()) for l in open(A / "f" / f"{p}-log.txt")] for p in PARTS}   # ms, keys, key LEDs, btn LEDs, enc x7
NF = min(len(v) for v in LOG.values())
BLACKPC = {1, 3, 6, 8, 10}
KEYS = []
nb = nw = 0
for k in range(27):
    if (53 + k) % 12 in BLACKPC: KEYS.append((k, 1, nb)); nb += 1
    else: KEYS.append((k, 0, nw)); nw += 1

def t_of(f): return f / FPS
def bar_of(f): return (t_of(f) - T0) / BAR

def stem_env(name):                                   # a stem's loudness per frame, 0..1
    with wave.open(str(A / "stems" / f"{name}.wav")) as w:
        a = np.frombuffer(w.readframes(w.getnframes()), np.int16).reshape(-1, 2).astype(np.float32) / 32768
    m = np.abs(a).max(1)
    n = 44100 // FPS
    e = np.array([m[i * n:(i + 1) * n].max() if i * n < len(m) else 0 for i in range(NF)])
    return e / max(e.max(), 1e-6)
ENV = {p: stem_env(p) for p in PARTS}

# ---- the drawing
def stripes(d, x0, x1, y, h=6, gap=3):
    for i, c in enumerate(STRIPES):
        d.rectangle([x0, y + i * (h + gap), x1, y + i * (h + gap) + h - 1], fill=c)

def base(flash=0.0):
    bg = tuple(int(c + (40 - c) * flash * 0.5) for c in BG)
    im = Image.new("RGB", (W, H), bg)
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, W, 96], fill=BG2)
    d.text((60, 28), "OberVave", font=F_BOLD(40), fill=CREAM)
    d.text((60 + d.textlength("OberVave ", font=F_BOLD(40)), 40), "an 8-voice analog-style poly with chorus, delay and plate reverb, for the M-VAVE FM-1", font=F_REG(24), fill=DIM)
    stripes(d, W - 380, W - 60, 36)
    d.rectangle([0, H - 8, W, H], fill=(40, 60, 110))
    return im

def scanlines(s):                                     # a hint of a tube: every third row darker
    a = np.asarray(s, dtype=np.float32)
    a[2::3] *= 0.82
    return Image.fromarray(a.astype(np.uint8))

def screen(part, f, scale=3, pad=None, glow=0.0):
    s = Image.open(A / "f" / part / f"{min(f, NF - 1):05d}.png").convert("RGB").resize((240 * scale, 240 * scale), Image.NEAREST)
    s = scanlines(s)
    pad = 8 * scale if pad is None else pad
    b = Image.new("RGBA", (s.width + 2 * pad, s.height + 2 * pad), (0, 0, 0, 0))
    edge = tuple(int(c0 + (c1 - c0) * glow) for c0, c1 in zip((80, 76, 70), BLUE))
    ImageDraw.Draw(b).rounded_rectangle([0, 0, b.width - 1, b.height - 1], radius=9 * scale, fill=(14, 14, 16), outline=edge, width=2 + int(glow * 3))
    b.paste(s, (pad, pad))
    return b

def shadow_paste(im, obj, xy):
    sh = Image.new("RGBA", (obj.width + 80, obj.height + 80), (0, 0, 0, 0))
    ImageDraw.Draw(sh).rounded_rectangle([40, 50, obj.width + 40, obj.height + 40], radius=30, fill=(0, 0, 0, 170))
    sh = sh.filter(ImageFilter.GaussianBlur(22))
    im.paste(sh, (xy[0] - 40, xy[1] - 40), sh)
    im.paste(obj, xy, obj)

def pulse_paste(im, obj, xy, k):                      # the kick pushes the object a little toward the viewer
    if k > 0.01:
        sw, sh = int(obj.width * (1 + 0.025 * k)), int(obj.height * (1 + 0.025 * k))
        obj = obj.resize((sw, sh), Image.BILINEAR)
        xy = (xy[0] - (sw - int(sw / (1 + 0.025 * k))) // 2, xy[1] - (sh - int(sh / (1 + 0.025 * k))) // 2)
    shadow_paste(im, obj, xy)

def wrap(d, text, f, width):
    out, line = [], ""
    for w in text.split():
        t = (line + " " + w).strip()
        if d.textlength(t, font=f) > width and line: out.append(line); line = w
        else: line = t
    out.append(line)
    return out

def keyboard(im, x0, y0, width, lit):
    d = ImageDraw.Draw(im)
    ww = width / 16; hh = ww * 3.4; bw = ww * 0.62; bh = hh * 0.6
    for k, black, i in KEYS:
        if black: continue
        x = x0 + i * ww
        on = (lit >> k) & 1
        d.rounded_rectangle([x + 2, y0 + (4 if on else 0), x + ww - 2, y0 + hh], radius=6, fill=(255, 120, 60) if on else (232, 228, 216))
    xi = 0
    for k, black, i in KEYS:
        if not black: xi += 1; continue
        x = x0 + xi * ww - bw / 2
        on = (lit >> k) & 1
        d.rounded_rectangle([x - 2, y0, x + bw + 2, y0 + bh + 2], radius=6, fill=(12, 12, 14))
        d.rounded_rectangle([x, y0 + (3 if on else 0), x + bw, y0 + bh], radius=5, fill=(255, 100, 50) if on else (30, 30, 34))
    return y0 + hh

BTN_ROWS = [[("FX", 2), ("SEL", 3), ("ENV", 4), ("LFO", 5), ("EDIT", 6), ("GLO", 7)],
            [("HOME", 8), ("SAVE", 9), ("ARP", 10), ("SEQ", 11), ("PLAY", 12), ("REC", 13)]]
KNOB_NAMES = ["SELECT", "ALGORITHM", "KNOB 1", "KNOB 2", "KNOB 3", "KNOB 4", "PRESETS"]

def panel(im, x0, y0, btn_led, enc):
    d = ImageDraw.Draw(im)
    for i, e in enumerate((2, 3, 4, 5)):
        cx, cy, r = x0 + 46 + i * 112, y0 + 46, 38
        d.ellipse([cx - r - 4, cy - r - 4, cx + r + 4, cy + r + 4], fill=(16, 16, 20))
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(50, 56, 70), outline=(110, 120, 140), width=2)
        a = math.radians(-120 + (enc[e] * 9) % 360)
        d.line([cx, cy, cx + math.sin(a) * (r - 8), cy - math.cos(a) * (r - 8)], fill=CREAM, width=5)
        d.text((cx, cy + r + 20), KNOB_NAMES[e], font=F_SEMI(18), fill=DIM, anchor="mm")
    cx, cy, r = x0 + 46 + 4 * 112 + 40, y0 + 46, 38                       # the ALGORITHM knob = CUTOFF
    d.ellipse([cx - r - 4, cy - r - 4, cx + r + 4, cy + r + 4], fill=(16, 16, 20))
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(50, 56, 70), outline=BLUE, width=3)
    a = math.radians(-120 + (enc[1] * 9) % 360)
    d.line([cx, cy, cx + math.sin(a) * (r - 8), cy - math.cos(a) * (r - 8)], fill=CREAM, width=5)
    d.text((cx, cy + r + 20), "CUTOFF", font=F_SEMI(18), fill=BLUE, anchor="mm")
    by = y0 + 128
    d.rounded_rectangle([x0 - 14, by - 14, x0 + 6 * 104 - 4, by + 2 * 74 + 2], radius=18, fill=PANEL)
    for r_, row in enumerate(BTN_ROWS):
        for c, (name, bid) in enumerate(row):
            x, y = x0 + c * 104, by + r_ * 74
            on = (btn_led >> bid) & 1
            d.rounded_rectangle([x + 2, y + 6, x + 92, y + 66], radius=14, fill=(22, 22, 26))
            d.rounded_rectangle([x, y, x + 90, y + 60], radius=14, fill=(255, 120, 60) if on else (58, 64, 80))
            d.text((x + 45, y + 30), name, font=F_BOLD(22), fill=(30, 20, 16) if on else CREAM, anchor="mm")

def caption(im, kicker, title, body, x=1010, y=180, width=840, slide=0.0):
    d = ImageDraw.Draw(im)
    x += int(60 * slide * slide)                                           # slides in over the first half second
    d.text((x, y), kicker.upper(), font=F_SEMI(24), fill=BLUE); y += 50
    for ln in wrap(d, title, F_LIGHT(62), width):
        d.text((x, y), ln, font=F_LIGHT(62), fill=CREAM); y += 74
    y += 12
    for ln in wrap(d, body, F_REG(30), width):
        d.text((x, y), ln, font=F_REG(30), fill=DIM); y += 44
    return y

def big_line(im, text, y, size=96, fill=CREAM):
    d = ImageDraw.Draw(im)
    d.text((W // 2, y), text, font=F_BOLD(size), fill=fill, anchor="mm")

# ---- the shot list: (from bar, to bar, kind, part(s), kicker, title, body)
SHOTS = [
    (0, 4, "title", "arp", "", "A slow, dark one", "Nine OberVaves. One firmware. Zero samples."),
    (4, 5, "focus", "kick", "the kick", "That kick is a filter", "No drum machine: the 12 dB filter at full resonance rings like a sine, the envelope drops its pitch. One FM-1, one kick."),
    (5, 8, "focus", "arp", "the arpeggio", "DARK ARP, all the way through", "A four-note chord held, climbed in 16ths. Two saws a hair apart into the 24 dB filter, half shut. A dotted-eighth ping-pong, a long dark plate. The filter opens over sixteen bars."),
    (8, 10, "focus", "bass", "the bass", "Root, octave, root, octave", "FAT BASS: a saw and a pulse an octave below it, drive, glide. Slow disco. The filter stays shut; this is not a happy record."),
    (10, 12, "trio", "kick clap hat", "the drums", "Drums? Also an 8-voice poly.", "Kick: a ringing filter. Clap: noise through the band-pass, three hits a flam apart, into the plate. Hats: noise through the high-pass, open on the offbeat. Sorry."),
    (12, 14, "focus", "pad", "the pad", "NEON PAD", "Pulse waves, the LFO sways their width with a different phase for every voice. A slow chorus widens it, the plate carries it. The chorus DEPTH goes up as it swells."),
    (14, 16, "duo", "arp pad", "two of nine", "The arpeggio keeps climbing", "Left: the arp, its CUTOFF a little higher every bar. Right: the pad under it. Both the same firmware, both through their own delay and reverb."),
    (16, 20, "focus", "lead", "the lead", "Six voices, slightly seasick", "UNISON LD: six voices wide apart, slow glide, VINTAGE all the way up so they drift. BEND set to an octave: the end of the phrase falls down a hole."),
    (20, 22, "focus", "bass", "the bass again", "RESO goes up", "Knob 1 on the FILTER page: the resonance climbs over two bars and the bass starts to whistle, then it comes back down."),
    (22, 24, "focus", "lead", "the lead", "And down it goes", "The pitch bend arrives over MIDI and the whole unison stack slides an octave down. The echoes follow it, in the tempo."),
    (24, 28, "duo", "arp pad", "breakdown", "The plate opens", "Drums out. The arp's filter sweeps all the way open while its REV MIX goes up on the knob, the pad swells under it. Then the clap rolls."),
    (28, 30, "focus", "arp2", "the drop", "UPSIDE ARP", "A second arpeggio, up and down over two octaves above the chord, through the 12 dB SEM-style filter. Everything else comes back under it."),
    (30, 32, "duo", "arp2 arp", "two arpeggios", "MORPH: lowpass to notch", "Knob 4 on FILTER + turns the SEM filter from lowpass toward the notch across four bars: the second arpeggio hollows out while the first keeps climbing."),
    (32, 34, "focus", "strings", "the strings", "PWM strings, two octaves up", "Pulse-width modulation, every voice its own LFO phase, panned across the field, a very long plate. The lead returns an octave higher."),
    (34, 38, "wall", "", "all nine", "Nine FM-1s at once", "Every part is the same firmware, one voice of the arrangement each. Nothing here was played by a computer, except the computer that played the FM-1s."),
    (38, 40, "focus", "lead", "the last fall", "Down the hole once more", "The drums stop, the lead falls an octave, the pad and the strings hold the last chord while the plates ring out."),
]
END_BAR = 40

def shot_at(b):
    for s in SHOTS:
        if s[0] <= b < s[1]: return s
    return None

def device_block(im, part, f, x, y, scale, kick):
    ms, keys, lit, btn, *enc = LOG[part][min(f, NF - 1)]
    pulse_paste(im, screen(part, f, scale, glow=ENV[part][min(f, NF - 1)]), (x, y), kick)
    return lit, btn, enc

def frame(f):
    b = bar_of(f)
    s = shot_at(b)
    fi = min(f, NF - 1)
    kick = float(ENV["kick"][fi]) if b >= 4 else 0.0
    clap = float(ENV["clap"][fi])
    im = base(flash=clap if b >= 8 else 0.0)
    d = ImageDraw.Draw(im)
    if s is None:                                                             # the end card
        shadow_paste(im, screen("arp", f, 2), (150, 220))
        d.text((1010, 230), "OberVave", font=F_BOLD(120), fill=CREAM)
        stripes(d, 1016, 1016 + 560, 372, 8, 4)
        d.text((1016, 412), "Like and subscribe", font=F_BOLD(60), fill=CREAM)
        d.text((1016, 488), "for the next firmware video:", font=F_REG(30), fill=DIM)
        d.text((1016, 528), "youtube.com/@sonzven", font=F_SEMI(48), fill=(255, 110, 80))
        d.text((1016, 606), "Every open-source firmware for the FM-1, one click each:", font=F_LIGHT(30), fill=CREAM)
        d.text((1016, 648), "fm1.designburgapps.com", font=F_BOLD(44), fill=BLUE)
        y = 712
        for ln in ("OberVave: free firmware, GPL-3.0. Built on Dinghy, SLOOP 2.3 (isod89) and Felucca (Hügelton Instruments).",
                   "Inspired by the classic 8-voice poly synths of 1979-84 (trademarks of their owners, no affiliation).",
                   "All sounds: OberVave on the FM-1, nine of them, chorus, delay and plate included. No samples were harmed."):
            for l2 in wrap(d, ln, F_REG(20), 840):
                d.text((1016, y), l2, font=F_REG(20), fill=DIM); y += 25
        ms, keys, lit, btn, *enc = LOG["arp"][fi]
        keyboard(im, 1016, 890, 700, lit)
        return im
    b0, b1, kind, parts, kicker, title, body = s
    slide = max(0.0, 1.0 - (b - b0) * BAR / 0.5)
    if kind == "title":
        # the arp's screen small, the name huge, the bands, a line; the kick is not in yet
        shadow_paste(im, screen("arp", f, 2), (120, 300))
        d.text((760, 250), "OberVave", font=F_BOLD(190), fill=CREAM)
        stripes(d, 770, 770 + 900, 470, 12, 6)
        d.text((770, 540), title, font=F_LIGHT(70), fill=CREAM)
        d.text((770, 640), body, font=F_REG(34), fill=DIM)
        if b >= 2:
            d.text((770, 740), "D Phrygian. 94 BPM. The filter is shut. So is the mood.", font=F_REG(28), fill=DIM)
        return im
    if kind == "wall":
        for i, p in enumerate(PARTS):
            x, y = 70 + (i % 3) * 318, 112 + (i // 3) * 310
            lv = float(ENV[p][fi])
            sc = screen(p, f, 1, pad=10, glow=lv)
            sc = sc.resize((int(sc.width * 1.08), int(sc.height * 1.08)), Image.BILINEAR)
            pulse_paste(im, sc, (x, y), kick if p == "kick" else kick * 0.3)
            d.text((x + sc.width // 2, y + sc.height + 18), p.upper(), font=F_SEMI(20), fill=BLUE if lv > 0.05 else DIM, anchor="mm")
        caption(im, kicker, title, body, x=1110, y=200, width=760, slide=slide)
        return im
    if kind == "trio":
        for i, p in enumerate(parts.split()):   # three big, in a row; the words underneath
            device_block(im, p, f, 90 + i * 580, 130, 2, kick if p == "kick" else kick * 0.3)
            d.text((90 + i * 580 + 256, 130 + 530), p.upper(), font=F_SEMI(24), fill=BLUE, anchor="mm")
        caption(im, kicker, title, body, x=90, y=720, width=1700, slide=slide)
        return im
    if kind == "duo":
        pa, pb = parts.split()
        la, _, _ = device_block(im, pa, f, 60, 150, 2, kick)
        lb, _, _ = device_block(im, pb, f, 60 + 560, 150, 2, kick * 0.5)
        d.text((60 + 256, 150 + 530), pa.upper(), font=F_SEMI(22), fill=BLUE, anchor="mm")
        d.text((620 + 256, 150 + 530), pb.upper(), font=F_SEMI(22), fill=BLUE, anchor="mm")
        keyboard(im, 60, 740, 500, la)
        keyboard(im, 620, 740, 500, lb)
        caption(im, kicker, title, body, x=1230, y=160, width=640, slide=slide)
        return im
    # focus: the device big on the left, the caption and the drawn panel + keyboard right
    lit, btn, enc = device_block(im, parts, f, 90, 150, 3, kick)
    caption(im, kicker, title, body, x=1000, y=140, width=860, slide=slide)
    panel(im, 1010, 560, btn, enc)
    keyboard(im, 1000, 850, 860, lit)
    return im

def tear(im, k):                                      # a cut: the red and blue planes slip apart for a moment
    r, g, b = im.split()
    r = ImageChops.offset(r, int(10 * k), 0)
    b = ImageChops.offset(b, -int(10 * k), 0)
    return Image.merge("RGB", (r, g, b))

total_frames = min(NF, int((T0 + (END_BAR + 4) * BAR + 3.0) * FPS))
cut_frames = set()
for s in SHOTS:
    cut_frames.add(int((T0 + s[0] * BAR) * FPS))
cut_frames.add(int((T0 + END_BAR * BAR) * FPS))

if "--stills" in sys.argv:
    for bb in (11, 15, 36):
        frame(int((T0 + bb * BAR) * FPS)).save(A / f"still-{bb}.png")
    sys.exit(0)

ff = subprocess.Popen(["nice", "-n", "15", "ffmpeg", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{W}x{H}",
                       "-r", str(FPS), "-i", "-", "-itsoffset", str(T0), "-i", str(A / "track.wav"),
                       "-af", "volume=5dB,alimiter=limit=0.89:attack=3:release=60:level=false",
                       "-c:v", "libx264", "-preset", "medium", "-crf", "18", "-pix_fmt", "yuv420p",
                       "-c:a", "aac", "-b:a", "192k", "-ar", "48000", "-movflags", "+faststart", "-shortest", str(OUT)], stdin=subprocess.PIPE)
for f in range(total_frames):
    im = frame(f)
    since_cut = min((f - c for c in cut_frames if c <= f), default=99)
    if since_cut < 3: im = tear(im, 1.0 - since_cut / 3)
    arr = np.asarray(im, dtype=np.uint8).astype(np.float32)
    k = min(1.0, (f + 1) / FPS / 0.4, (total_frames - f) / FPS / 1.2)        # fade in and out
    if k < 1: arr = arr * max(0.0, k)
    ff.stdin.write(arr.astype(np.uint8).tobytes())
    if f % 300 == 0: print(f"{f}/{total_frames}", file=sys.stderr)
ff.stdin.close(); ff.wait()
print(OUT)
