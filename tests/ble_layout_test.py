#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# BLE MIDI: 2026 DEADACTIVE
"""The Bluetooth build's memory placement gate (tools/build.py ble_placement) and firmware/app_ble.ld, on the host.

SLOOP's Bluetooth build lost its sound on an FM-1 (2026-10-24, parked): it is the one Bluetooth build that runs a
second core and that moved the BT controller's memory (the libraries' *_bss) into the SRAM below the RAM region
(.lowram) and .bd_base to the pool's end. OberVave keeps Felucca's layout (the one the FM-1 has run Bluetooth with) and
one core; this checks that the gate refuses each of SLOOP's moves and that app_ble.ld has none of them."""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import build  # noqa: E402

fails = 0


def ok(what, good):
    global fails
    print(("ok    " if good else "FAIL  ") + what)
    fails += not good


REG = build.ld_regions()
SYMS = "01c2a000 l     O .pool  00005c00 ble_heap\n01c2fc00 l     O .pool  000017d0 tasks\n"
GOOD = (" .bd_base       0x0000000001c1a000     0x1064 /tmp/lto.o\n"
        " .ble_ll_bss    0x0000000001c1b100       0x57 /tmp/lto.o\n"
        " .bt_stack_bss  0x0000000001c1b200      0x534 /tmp/lto.o\n")
ok("Felucca's layout: accepted", build.ble_placement(GOOD, SYMS, REG) == [])
lowram = GOOD.replace("0x0000000001c1b100", "0x0000000001c07cb8")
ok("the controller's data below the RAM region (SLOOP's .lowram): refused",
   any(".ble_ll_bss" in e for e in build.ble_placement(lowram, SYMS, REG)))
pool = GOOD.replace("0x0000000001c1a000", "0x0000000001c716c8")
ok(".bd_base at the pool's end (SLOOP's): refused", any(".bd_base" in e for e in build.ble_placement(pool, SYMS, REG)))
heap_bss = SYMS.replace("01c2a000", "01c16798")
ok("the BLE heap in .bss (SLOOP's): refused", any("ble_heap" in e for e in build.ble_placement(GOOD, heap_bss, REG)))
ok("a second core's code or stack linked: refused",
   any("second-core" in e for e in build.ble_placement(GOOD, SYMS + "01c01b1c g     F .c1_text  00000030 fm1_c1_main\n", REG)))
ok("the BLE unit's heap missing: refused", any("not found" in e for e in build.ble_placement(GOOD, "", REG)))

ld = (ROOT / "firmware" / "app_ble.ld").read_text()
body = re.sub(r"/\*.*?\*/", "", ld, flags=re.S)
ok("app_ble.ld: the same MEMORY block as app.ld", build.ld_regions(ROOT / "firmware" / "app_ble.ld") == REG)
ok("app_ble.ld: no .lowram, no second core's sections", ".lowram" not in body and "c1_text" not in body and
   "c1stack" not in body)
ok("app_ble.ld: the libraries' zeroed data and .bd_base in .bss (> RAM), the pool only .pool",
   re.search(r"\.bss \(NOLOAD\) : \{[^}]*\*\(\.bd_base\)[^}]*\*\(\.\*_bss\)[^}]*\} > RAM", body) is not None and
   re.search(r"\.pool \(NOLOAD\) : \{ _pool_start = \.; \*\(\.pool\*\) \. = ALIGN\(4\); _pool_end = \.; \} > POOL", body)
   is not None)
print("ble_layout: all checks passed" if not fails else f"ble_layout: {fails} FAILED")
sys.exit(1 if fails else 0)
