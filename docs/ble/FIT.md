# The Bluetooth build on OberVave: does it fit?

BLE MIDI: 2026 DEADACTIVE. OberVave 3.0.1 with Felucca's BLE unit (`FELUCCA_BLE=1 sh ./build.sh`), measured against
the app slot (581,564 B), the RAM region (98,304 B: `.data` + `.bss`) and the pool (344,064 B). The budgets every
Felucca port keeps: 4 KB of `.data` + `.bss` and 8 KB of pool spare.

| | image B | slot free B | `.data + .bss` B | spare | pool B | spare | RAM code B |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| OberVave 3.0.1, plain | 138,860 | 442,704 | 62,800 | 35,504 | 318,708 | 25,356 | (925 insns) |
| **the Bluetooth build** | **287,148** | **294,416** | **93,808** | **4,496** | **333,976** | **10,088** | **6,940** |

Flash is no question (OberVave is small). The pool is: the BLE unit's heap (23,552 B) and its three task stacks
take about 31 KB of it, and the plain build has 25 KB spare. So the Bluetooth build moves two buffers the pool held
into `.bss` (`#if FELUCCA_BLE` only; the plain build keeps them in the pool): the sequencer's undo pattern
(`undo_pat`, 13,416 B) and the 1.4 settings' conversion buffer (`app_main.old`, 2,572 B, read once at a 1.4 update).
Nothing audible changes (the effects' lines, the canvas, the queued and the left patterns stay in the pool), nothing
is left out, no feature cut, no fit tool needed (`BLE_CONSOLE` stays at its default, the BLE unit at `-Os`).

The BT memory is placed as Felucca's (`firmware/app_ble.ld`): the libraries' zeroed data and `.bd_base` in `.bss` of
the RAM region, the heap and task stacks in the pool, nothing below 0x01C08000 but RAM code (the flash driver and the
radio calibration's), one core. `tools/build.py` refuses anything else (BLUETOOTH.md: SLOOP's silence).
