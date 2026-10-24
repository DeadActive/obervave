# Bluetooth MIDI on OberVave (the Bluetooth build)

BLE MIDI: 2026 DEADACTIVE. Ported from Felucca's reference (DeadActive/Felucca, branch `ble-kit` d702dc0,
`docs/ble/PORTING.md`) onto OberVave 3.0.1. FM-1 only.

`FELUCCA_BLE=1 sh ./build.sh` builds OberVave with Bluetooth LE MIDI: **HOST**, the FM-1 keeps up to 8 saved devices
and connects to them by itself, two at once (a BLE-MIDI controller such as an M-VAVE SMC-Mixer, and another FM-1 for
FM-Link); **DEVICE**, the FM-1 is a BLE-MIDI device a computer, a tablet or another FM-1 connects to (it advertises as
"Felucca FM-1", the BLE unit's name, so every FM-1 firmware sees it as an FM-1). The package is
`build/obervave-ble.fwsc`. The plain build (`sh ./build.sh`) is OberVave 3.0.1, byte for byte; either installs over
the other. `BOARD=smk37` refuses `FELUCCA_BLE=1`.

**Not yet run on a device.** The BLE unit is Felucca's, unchanged; its place in OberVave is checked by the build's
gates and the host tests only. Nothing is left out of OberVave for it (FIT.md).

## The Bluetooth page (GLO tapped: GLOBAL, VOICE, OUTPUT, BLUETOOTH)

GLO's fourth page takes the screen under the header: a state line, a dim line (the last error; else **AUDIO GAP**,
the longest gap between two audio halves since the page opened, 5.8 ms when nothing held the audio off, and **LATE**,
the halves rendered after the DMA had moved on, since power-on), the rows **BLUETOOTH**, **AUTOSTART**, **ROLE**
(HOST / DEVICE), **MODE** (AUTO, SMC DAW, MIDI, FM-LINK) and **RECONNECT** (AUTO, FAST, SLOW), then (ROLE HOST) the
saved devices, three in view, and **+ ADD A DEVICE**. **PRESETS** moves the cursor, **any of KNOB 1-4** changes the
row (right / left), **OCT+** acts on it, **OCT-** goes back (from the ADD list or a FORGET question). GLO tapped
again, or any other page, closes it; the keys play meanwhile.

| Row | A knob | OCT+ |
| --- | --- | --- |
| BLUETOOTH | right ON, left OFF | ON / OFF |
| AUTOSTART | right ON, left OFF (saved) | ON / OFF |
| ROLE | right DEVICE, left HOST (saved) | HOST / DEVICE |
| MODE | AUTO, SMC DAW, MIDI, FM-LINK (stops at the ends; saved) | the next (wraps) |
| RECONNECT | AUTO, FAST, SLOW (stops at the ends; saved) | the next (wraps) |
| a saved device | left: `FORGET <name>?`, OCT+ yes, OCT- no | NEAR: use it now |
| + ADD A DEVICE | - | an 8 s scan for nearby devices not saved, then the list stays; NOTHING FOUND: OCT+ again |

Each device line: a dot (connected) or a ring, its name (an FM-1 as `FM-1` and the last four hex digits of its
address), `CTRL` or `FM-1`, and ON / NEAR / AWAY. The rest is Felucca's behaviour: Bluetooth is off at every power-on
unless AUTOSTART is ON (then it turns on 3 s after the start, after a clean start only: not after a watchdog or a soft
reset, a crash's or a Bluetooth fault's, `BT AUTOSTART SKIPPED`); after FAILED only a power cycle starts it again; two
links at once, one controller and one FM-1, reconnecting by themselves with a 30 s grace.

The header shows **BT** after the other tags: lit while a link is up, struck through while Bluetooth is on and
nothing is linked, nothing when off (ARP and FX give way to it; with MONO NOTCH HOLD SEQ REC all shown it is left out).

## MIDI, the clock, FM-Link

- **MIDI mode:** the device is a third MIDI input beside USB and TRS, read by `midi_in` from a ring of its own after
  theirs (notes on the channels as USB's, bend, sustain, CCs, program change); the notes the keys send to USB MIDI out
  go to the device too.
- **GLOBAL > SELECT pressed** (SYNC) has two more values: **BLE** (only the device's clock, Start and Stop are
  followed; USB's and TRS's are ignored) and **LINK** (FM-Link's follower). OberVave's settings byte keeps INTERNAL for
  them (the plain build reads it), the Bluetooth record the value. AUTO follows any clock, BLE's included.
- **Clock out:** on its own tempo (INTERNAL, or AUTO with no clock coming in), linked in MIDI mode, OberVave sends
  Start, 24 pulses a beat (counted from the audio's samples) and Stop to the device.
- **FM-Link:** two FM-1s in time without MIDI clock (Felucca's engine, `fmlink.c`, unchanged: OberVave, Felucca, SLOOP
  and Hortator keep time with each other): the leader on its own tempo (INTERNAL, or AUTO without a clock), the
  follower on LINK (the leader's tempo, PLAY and STOP; its own PLAY / STOP say FOLLOWING LINK). OberVave's sequencer
  steps in control blocks of 16 frames: FM-Link's sample-by-sample correction is paid in whole ticks (a block with
  none, or two), within 16 frames (0.4 ms) of FM-Link's own count (tests/bt_fml_test.c).
- **SMC DAW:** faders 1-4 the tracks' LEVEL, 5-8 their REVERB send, with pickup; knobs 1-4 the tracks' PAN, 5-8 the
  selected track's PERFORM parameters (CUTOFF RESO ENV AMT RELEASE); MUTE and SOLO (the mixer's, as GLO + keys 5-12),
  REC (the track, then REC), SELECT; PLAY, STOP, REC; BANK / CHANNEL the previous / next track.

## Where it is kept

The Bluetooth settings, SYNC BLE / LINK, this FM-1's address, the saved devices and a copy of the radio calibration
(72 bytes, the stock firmware's config store item, somewhere in 0xE5000..0xFBFFF) live in a 300-byte record of their
own, two copies in the sectors 0xB9000 and 0xBA000 (after the pattern slots; free in OberVave 3.0.1), each in the
platform's sector format (a cut save leaves the other copy). Saved once the sequencer stops and the voices are quiet,
as every OberVave save. OberVave's own settings are not grown or moved. The Bluetooth build's flash driver window
allows exactly those two sectors and no longer 1.3-1.5's pattern sectors (0xF4000..0xFBFFF), which 3.0 only reads and
which lie in the stock config store.

## The audio and the Bluetooth libraries

The audio interrupt (priority 3) renders 256-frame halves (5.8 ms; Felucca's are 2.9 ms). The BT libraries' requests
are attached at priority 2 at most (the BLE unit), under the audio and TIMER5 (4), so their handlers never run inside
the render; but their own interrupts-off stretches (up to ~12 ms measured on Felucca, at a refused reconnect) can hold
the audio interrupt off: past a half the DMA plays one half twice (a LATE, a click). The page's dim line shows the
longest gap and the count for the device test. RECONNECT AUTO already rests 8-16 s between attempts while the
sequencer plays. If the device test shows LATE climbing while playing, the next levers (each a choice for the user):
no host reconnect attempts while the sequencer plays (RECONNECT AUTO waiting for STOP), or a longer audio half in the
Bluetooth build (512 frames, 11.6 ms: rides over a 12 ms stretch, +5.8 ms of latency, the USB audio band to re-check).

## SLOOP's silence, and what OberVave does differently

SLOOP's Bluetooth build (the same lineage: Dinghy's platform comes from SLOOP / Felucca ~1.0) lost its sound
completely on an FM-1 with Bluetooth on, while FM-Link kept syncing. Not reproduced here (no device); what SLOOP's
build does that no Bluetooth build that worked on an FM-1 does:

1. **Two cores.** SLOOP keeps its second core: the audio interrupt hands core 1 part of the render and busy-waits for
   it (`fm1_cpu1_wait`, 8 ms), at raised supplies (VDC14 1.60 V, SYSVDD 1.38 V). The BLE unit assumes one core
   (`request_irq` ignores the core, `local_irq_*` is core 0's `cli`, the radio code runs with core 0's interrupts off
   only). Felucca's Bluetooth build forces one core (`FELUCCA_CPU2=0`), Skiff's runs one.
2. **The BT memory moved.** SLOOP's `app_ble.ld` put the BT controller's zeroed data (`.bt_stack_bss`,
   `.ble_ll_bss`, `.ble_rf_bss`, `.hci_controller_bss`, ...) into SRAM below the RAM region (`.lowram`,
   0x01C0744C..0x01C07DA0, right after its reverb lines, core 1's stack and the render split's scratch), `.bd_base`
   to the pool's end (0x01C716C8) and the BLE heap and task stacks into `.bss`. Felucca's and Hortator's (which ran on
   FM-1s) keep `.bd_base` and the `*_bss` in `.bss`, the heap and stacks in `.pool`.

The likeliest cause is one of these two (the second core first: it is in the audio path itself); the rest (interrupt
priorities, the vector numbers, the audio clock and DMA set-up, the libraries' register writes) is the same as
Felucca's. OberVave runs one core (it has no second core at all) and keeps Felucca's layout exactly, and the build
refuses both of SLOOP's moves: `tools/build.py` (the BT memory's place: the libraries' RAM sections inside the RAM
region, the heap and stacks in the pool, nothing below 0x01C08000 but RAM code, no core-1 code or stack), tested by
`tests/ble_layout_test.py`.

## Device test (what to check)

1. Install `obervave-3.0.1-ble-UNTESTED.fwsc`; the plain OberVave sounds and works as before with Bluetooth off.
2. GLO to BLUETOOTH, ON: STARTING, then ON, NO DEVICE SAVED (or FAILED NO RF TRIM IN FLASH: the stock calibration is
   gone from this unit; report it). Play a pattern: AUDIO GAP stays near 5.8 ms, LATE stays.
3. ADD the SMC-Mixer: MODE AUTO takes SMC DAW; the faders (pickup), PAN, MUTE / SOLO / SELECT / REC, PLAY / STOP.
4. A BLE-MIDI keyboard in MIDI mode: notes, CCs; its clock with SYNC BLE.
5. Two FM-1s: one INTERNAL, the other SYNC LINK, saved on each other's page (ROLE HOST + the other DEVICE, or both
   HOST with the FM-1 saved): PLAY on the leader starts both, tempo follows, STOP stops both.
6. Power off and on: the devices, MODE, AUTOSTART kept; with AUTOSTART ON Bluetooth comes up by itself 3 s on.
7. Throughout: watch AUDIO GAP and LATE while the sequencer plays and a device drops and reconnects (the 12 ms
   stretches): report the numbers.
