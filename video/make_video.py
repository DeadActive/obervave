#!/usr/bin/env python3
"""OberVave video (1920 x 1080, 30 fps, ~75 s): nine OberVaves play one track (song.py: the real firmware's
host build, its screens filmed at 30 fps, its audio). The part in focus left on a drawn FM-1 (screen, knobs, buttons,
keys lit as the firmware lights them), what happens right; bars 24-28 all of them at once; the end card.
  python3 song.py && python3 make_video.py [--stills]"""
import math, subprocess, sys
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = Path(__file__).resolve().parent
A = HERE
OUT = HERE / "obervave-1.0.mp4"
W, H, FPS = 1920, 1080, 30
BPM, T0 = 108, 2.0
BAR = 4 * 60 / BPM
BG, BG2, PANEL = (26, 26, 30), (36, 36, 41), (46, 46, 52)
CREAM, DIM, AMBER, RED = (236, 226, 198), (150, 142, 128), (255, 170, 40), (255, 44, 32)
STRIPES = [(230, 40, 30), (245, 120, 30), (255, 190, 50)]
IF = "/run/host/fonts/rsms-inter-fonts/"
def font(n, s): return ImageFont.truetype(IF + n, int(s))
F_LIGHT = lambda s: font("InterDisplay-Light.ttf", s)
F_BOLD = lambda s: font("InterDisplay-Bold.ttf", s)
F_REG = lambda s: font("Inter-Regular.ttf", s)
F_SEMI = lambda s: font("Inter-SemiBold.ttf", s)

PARTS = ["pad", "arp", "bass", "brass", "lead", "strings"]
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

# ---- the drawing
def stripes(d, x0, x1, y, h=6, gap=3):
    for i, c in enumerate(STRIPES):
        d.rectangle([x0, y + i * (h + gap), x1, y + i * (h + gap) + h - 1], fill=c)

def base():
    im = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, W, 96], fill=BG2)
    d.text((60, 28), "OberVave", font=F_BOLD(40), fill=CREAM)
    d.text((60 + d.textlength("OberVave ", font=F_BOLD(40)), 40), "an 8-voice analog-style poly for the M-VAVE FM-1", font=F_REG(24), fill=DIM)
    stripes(d, W - 380, W - 60, 36)
    d.rectangle([0, H - 8, W, H], fill=(60, 56, 50))
    return im

def screen(part, f, scale=3, pad=None):
    s = Image.open(A / "f" / part / f"{min(f, NF - 1):05d}.png").convert("RGB").resize((240 * scale, 240 * scale), Image.NEAREST)
    pad = 8 * scale if pad is None else pad
    b = Image.new("RGBA", (s.width + 2 * pad, s.height + 2 * pad), (0, 0, 0, 0))
    ImageDraw.Draw(b).rounded_rectangle([0, 0, b.width - 1, b.height - 1], radius=9 * scale, fill=(14, 14, 16), outline=(80, 76, 70), width=2)
    b.paste(s, (pad, pad))
    return b

def shadow_paste(im, obj, xy):
    sh = Image.new("RGBA", (obj.width + 80, obj.height + 80), (0, 0, 0, 0))
    ImageDraw.Draw(sh).rounded_rectangle([40, 50, obj.width + 40, obj.height + 40], radius=30, fill=(0, 0, 0, 170))
    sh = sh.filter(ImageFilter.GaussianBlur(22))
    im.paste(sh, (xy[0] - 40, xy[1] - 40), sh)
    im.paste(obj, xy, obj)

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
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(70, 70, 76), outline=(120, 118, 112), width=2)
        a = math.radians(-120 + (enc[e] * 9) % 360)
        d.line([cx, cy, cx + math.sin(a) * (r - 8), cy - math.cos(a) * (r - 8)], fill=CREAM, width=5)
        d.text((cx, cy + r + 20), KNOB_NAMES[e], font=F_SEMI(18), fill=DIM, anchor="mm")
    cx, cy, r = x0 + 46 + 4 * 112 + 40, y0 + 46, 38                       # the ALGORITHM knob = CUTOFF
    d.ellipse([cx - r - 4, cy - r - 4, cx + r + 4, cy + r + 4], fill=(16, 16, 20))
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(70, 70, 76), outline=AMBER, width=3)
    a = math.radians(-120 + (enc[1] * 9) % 360)
    d.line([cx, cy, cx + math.sin(a) * (r - 8), cy - math.cos(a) * (r - 8)], fill=CREAM, width=5)
    d.text((cx, cy + r + 20), "CUTOFF", font=F_SEMI(18), fill=AMBER, anchor="mm")
    by = y0 + 128
    d.rounded_rectangle([x0 - 14, by - 14, x0 + 6 * 104 - 4, by + 2 * 74 + 2], radius=18, fill=PANEL)
    for r_, row in enumerate(BTN_ROWS):
        for c, (name, bid) in enumerate(row):
            x, y = x0 + c * 104, by + r_ * 74
            on = (btn_led >> bid) & 1
            d.rounded_rectangle([x + 2, y + 6, x + 92, y + 66], radius=14, fill=(22, 22, 26))
            d.rounded_rectangle([x, y, x + 90, y + 60], radius=14, fill=(255, 120, 60) if on else (64, 64, 70))
            d.text((x + 45, y + 30), name, font=F_BOLD(22), fill=(30, 20, 16) if on else CREAM, anchor="mm")

def caption(im, kicker, title, body, x=1010, y=180, width=840):
    d = ImageDraw.Draw(im)
    d.text((x, y), kicker.upper(), font=F_SEMI(24), fill=AMBER); y += 50
    for ln in wrap(d, title, F_LIGHT(62), width):
        d.text((x, y), ln, font=F_LIGHT(62), fill=CREAM); y += 74
    y += 12
    for ln in wrap(d, body, F_REG(30), width):
        d.text((x, y), ln, font=F_REG(30), fill=DIM); y += 44
    return y

# ---- the sections, by bar
SECTIONS = [
    (0, 4, "pad", "OberVave", "An 8-voice analog-style polysynth", "Two oscillators a voice, saw and pulse, hard sync and cross-modulation. A 12 or 24 dB filter, two envelopes, an LFO, an arpeggiator. Free firmware for the M-VAVE FM-1, built on Dinghy."),
    (4, 8, "bass", "1 of 6 · the bass", "Phat mono bass", "FAT BASS: a saw and a pulse an octave below it, into the 24 dB filter with drive. Last-note priority, glide between the notes. RESO goes up a little in bar 4."),
    (8, 12, "brass", "2 of 6 · brass", "Brass stabs, the filter envelope", "BRASS STAB: two saws, slightly detuned, eight voices. The filter envelope snaps the cutoff open on every hit. ENV AMT is turned down, then back up."),
    (12, 16, "lead", "3 of 6 · sync lead", "OSC 2 hard-synced to OSC 1", "SYNC LEAD: the filter envelope sweeps OSC 2's pitch while it is synced: the classic tearing lead. CUTOFF rides on the ALGORITHM knob."),
    (16, 20, "strings", "4 of 6 · strings", "PWM strings", "PWM STRING: pulse waves, the LFO sways their width, every voice with its own LFO phase, panned left and right. Under it the arpeggiator keeps running: UP/DOWN, 1/16, two octaves."),
    (20, 24, "lead", "5 of 6 · unison", "Six voices on one key", "UNISON LD: six detuned voices, spread across the stereo field, with glide. The pitch bend at the end of the phrase comes in over MIDI."),
    (24, 28, "all", "6 of 6 · all of them", "Nine OberVaves at once", "Every part is the same firmware, nine FM-1s, one voice of the arrangement each. The drums too: a kick from the resonant filter ringing, a snare and hats from noise through the band-pass and high-pass."),
]
END_BAR = 28

def frame(f):
    b = bar_of(f)
    sec = next((s for s in SECTIONS if s[0] <= b < s[1]), None)
    im = base()
    d = ImageDraw.Draw(im)
    if sec is None:                                                           # the end card
        part = "pad"
        ms, keys, lit, btn, *enc = LOG[part][min(f, NF - 1)]
        shadow_paste(im, screen(part, f, 2), (150, 220))
        d.text((1010, 230), "OberVave", font=F_BOLD(120), fill=CREAM)
        stripes(d, 1016, 1016 + 560, 372, 8, 4)
        d.text((1016, 412), "Like and subscribe", font=F_BOLD(60), fill=CREAM)
        d.text((1016, 488), "for the next firmware video:", font=F_REG(30), fill=DIM)
        d.text((1016, 528), "youtube.com/@sonzven", font=F_SEMI(48), fill=(255, 110, 80))
        d.text((1016, 606), "Every open-source firmware for the FM-1, one click each:", font=F_LIGHT(30), fill=CREAM)
        d.text((1016, 648), "fm1.designburgapps.com", font=F_BOLD(44), fill=AMBER)
        y = 712
        for ln in ("OberVave: free firmware, GPL-3.0. Built on Dinghy, SLOOP 2.3 (isod89) and Felucca (Hügelton Instruments).",
                   "Inspired by the classic 8-voice poly synths of 1979-84 (trademarks of their owners, no affiliation).",
                   "All sounds: OberVave on the FM-1, nine of them. Please always check the latest firmware version!"):
            for l2 in wrap(d, ln, F_REG(20), 840):
                d.text((1016, y), l2, font=F_REG(20), fill=DIM); y += 25
        keyboard(im, 1016, 890, 700, lit)
        return im
    b0, b1, part, kicker, title, body = sec
    if part == "all":
        for i, p in enumerate(("bass", "brass", "lead", "pad")):
            shadow_paste(im, screen(p, f, 2, 8), (70 + (i % 2) * 510, 104 + (i // 2) * 470))
        caption(im, kicker, title, body, x=1100, y=180, width=760)
        d.text((1100, 640), "bass · brass · lead · pad", font=F_SEMI(26), fill=AMBER)
        d.text((1100, 680), "+ arp · strings · kick · snare · hats", font=F_REG(26), fill=DIM)
        return im
    ms, keys, lit, btn, *enc = LOG[part][min(f, NF - 1)]
    shadow_paste(im, screen(part, f, 3), (90, 150))
    if b0 == 0:
        d.text((1000, 150), "OberVave", font=F_BOLD(150), fill=CREAM)
        stripes(d, 1008, 1008 + 700, 326, 10, 5)
        d.text((1008, 380), title, font=F_LIGHT(54), fill=CREAM)
        y = 460
        for ln in wrap(d, body, F_REG(28), 860):
            d.text((1008, y), ln, font=F_REG(28), fill=DIM); y += 40
    else:
        caption(im, kicker, title, body, x=1000, y=140, width=860)
    panel(im, 1010, 560, btn, enc)
    keyboard(im, 1000, 850, 860, lit)
    return im

total_frames = min(NF, int((T0 + (END_BAR + 4) * BAR + 3.0) * FPS))
if "--stills" in sys.argv:
    for bb in (1, 5.5, 9.5, 13, 17, 21.5, 25, 30):
        frame(int((T0 + bb * BAR) * FPS)).save(A / f"still-{bb}.png")
    sys.exit(0)

ff = subprocess.Popen(["nice", "-n", "15", "ffmpeg", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{W}x{H}",
                       "-r", str(FPS), "-i", "-", "-itsoffset", str(T0), "-i", str(A / "track.wav"),
                       "-af", "alimiter=limit=0.89:attack=3:release=60:level=false",
                       "-c:v", "libx264", "-preset", "medium", "-crf", "18", "-pix_fmt", "yuv420p",
                       "-c:a", "aac", "-b:a", "192k", "-ar", "48000", "-movflags", "+faststart", "-shortest", str(OUT)], stdin=subprocess.PIPE)
for f in range(total_frames):
    im = frame(f)
    arr = np.asarray(im, dtype=np.uint8).astype(np.float32)
    k = min(1.0, (f + 1) / FPS / 0.4, (total_frames - f) / FPS / 1.2)        # fade in and out
    if k < 1: arr = arr * max(0.0, k)
    ff.stdin.write(arr.astype(np.uint8).tobytes())
    if f % 300 == 0: print(f"{f}/{total_frames}", file=sys.stderr)
ff.stdin.close(); ff.wait()
print(OUT)
