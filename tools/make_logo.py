#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""OberVave's logo: tools/make_logo.py -> assets/logo.svg, assets/logo.png (and the same on a transparent background).
The word in a bold 70s serif as outlines that glow red (the look of a certain small-town title card), the lower half of
the letters crossed by three blue bands (the look of a certain blue synthesizer panel). The letters are paths, so the
SVG needs no fonts. An own drawing: no one's trade dress, two moods.

  python3 tools/make_logo.py [font.otf] [out-dir]"""
import subprocess, sys
from pathlib import Path
from fontTools.ttLib import TTFont
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen

HERE = Path(__file__).resolve().parent.parent
FONT = sys.argv[1] if len(sys.argv) > 1 else "/run/host/fonts/urw-base35/URWBookman-Demi.otf"
OUT = Path(sys.argv[2]) if len(sys.argv) > 2 else HERE / "assets"
SUB_FONT = "/run/host/fonts/rsms-inter-fonts/Inter-SemiBold.ttf"

WORD = "OBERVAVE"
CAP = 150                                           # the cap height in user units
TRACK = 0.06                                        # letter spacing, as a fraction of the cap height
BLUE = ["#1e46be", "#3c82eb", "#78beff"]
RED, RED_DEEP, PANEL, CREAM = "#ff2c20", "#7a1410", "#0b0e16", "#e2e8f0"

def word_paths(font_path, text, cap, track, weight=None):
    """the glyphs of `text` as SVG path data, laid out from x = 0, baseline at y = 0, scaled to cap height `cap`"""
    f = TTFont(font_path)
    if weight and "fvar" in f:
        from fontTools.varLib.instancer import instantiateVariableFont
        f = instantiateVariableFont(f, {"wght": weight})
    gs = f.getGlyphSet()
    cmap = f.getBestCmap()
    upm = f["head"].unitsPerEm
    cap_units = f["OS/2"].sCapHeight if getattr(f["OS/2"], "sCapHeight", 0) else upm * 0.7
    s = cap / cap_units
    x, paths, widths = 0.0, [], []
    for ch in text:
        g = gs[cmap[ord(ch)]]
        pen = SVGPathPen(gs)
        g.draw(TransformPen(pen, (s, 0, 0, -s, x, 0)))
        paths.append(pen.getCommands())
        widths.append(g.width * s)
        x += g.width * s + track * cap
    return paths, x - track * cap

def main():
    paths, width = word_paths(FONT, WORD, CAP, TRACK)
    sub, sub_w = word_paths(SUB_FONT, "8-VOICE ANALOG POLY  ·  M-VAVE FM-1", 22, 0.18)
    pad = 70
    W, H = width + 2 * pad, CAP + 2 * pad + 60
    ox, oy = pad, pad + CAP                          # the baseline
    band_y0 = oy - CAP * 0.47                        # the bands: three through the lower half of the letters
    band_h, gap = CAP * 0.11, CAP * 0.05
    letters = "".join(f'<path d="{p}"/>' for p in paths)
    clip_letters = "".join(f'<path d="{p}" transform="translate({ox} {oy})"/>' for p in paths)   # no <g> in a clipPath
    bands = "".join(f'<rect x="{ox - 2 * pad}" y="{band_y0 + i * (band_h + gap):.1f}" width="{W + 2 * pad}" height="{band_h:.1f}" fill="{c}"/>'
                    for i, c in enumerate(BLUE))
    def svg(background):
        bg = f'<rect width="{W:.0f}" height="{H:.0f}" rx="28" fill="{PANEL}"/>' if background else ""
        return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W:.0f} {H:.0f}" width="{W:.0f}" height="{H:.0f}">
<defs>
  <filter id="glow" x="-20%" y="-40%" width="140%" height="180%">
    <feGaussianBlur stdDeviation="7" result="b"/>
    <feColorMatrix in="b" type="matrix" values="1 0 0 0 0  0 1 0 0 0  0 0 1 0 0  0 0 0 1.6 0" result="g"/>
    <feMerge><feMergeNode in="g"/><feMergeNode in="SourceGraphic"/></feMerge>
  </filter>
  <filter id="soft" x="-20%" y="-40%" width="140%" height="180%"><feGaussianBlur stdDeviation="2.2"/></filter>
  <clipPath id="letters">{clip_letters}</clipPath>
</defs>
{bg}
<!-- the deep red body of the letters -->
<g transform="translate({ox} {oy})" fill="{RED_DEEP}" fill-opacity="0.55">{letters}</g>
<!-- the three bands, only inside the letters -->
<g clip-path="url(#letters)">{bands}</g>
<!-- the glowing outline -->
<g transform="translate({ox} {oy})" fill="none" stroke="{RED}" stroke-width="7" stroke-linejoin="round" opacity="0.55" filter="url(#soft)">{letters}</g>
<g transform="translate({ox} {oy})" fill="none" stroke="{RED}" stroke-width="3.2" stroke-linejoin="round" filter="url(#glow)">{letters}</g>
<!-- the line under it -->
<g transform="translate({ox + (width - sub_w) / 2:.1f} {oy + 52})" fill="{BLUE[2]}">{"".join(f'<path d="{p}"/>' for p in sub)}</g>
</svg>
'''
    OUT.mkdir(parents=True, exist_ok=True)
    for name, background in (("logo", True), ("logo-transparent", False)):
        p = OUT / f"{name}.svg"
        p.write_text(svg(background))
        subprocess.run(["rsvg-convert", "-w", "1600", "-o", str(OUT / f"{name}.png"), str(p)], check=True)
        print(p, OUT / f"{name}.png")

if __name__ == "__main__":
    main()
