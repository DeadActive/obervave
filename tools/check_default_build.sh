#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# BLE MIDI: 2026 DEADACTIVE (from Felucca's ble-kit; Hortator)
# Gate 5: the plain build (no FELUCCA_BLE) does not depend on the Bluetooth code. This tree's plain build is
# byte-identical to the same tree's with every Bluetooth file deleted, both built now. A plain build that includes,
# links or reads a Bluetooth file outside the #if FELUCCA_BLE guards fails here (it does not build, or builds
# differently). It rebuilds build/ (the plain build).
#   tools/check_default_build.sh
set -e
cd "$(dirname "$0")/.."
BT="firmware/src/ble firmware/src/core_ble_api.c firmware/src/bt_store.c firmware/src/bt_set.c firmware/src/bt_midi.c
    firmware/src/bt_fml.c firmware/src/bt_devs.c firmware/src/bt_page.c firmware/src/smc.c firmware/src/fmlink.c
    firmware/hal/fm1_ble_hal.h firmware/hal/fm1_ctx.h firmware/hal/fm1_ctx.S firmware/app_ble.ld tools/ble_libs.py
    tools/ble_base.txt tools/check_update_path.py tools/check_ble_boot.py tests/bt_host.h"
bld() {                                         # a build, retried (up to 5) on Docker's "exec format error"
    for i in 1 2 3 4 5; do
        if out=$("$@" 2>&1); then return 0; fi
        echo "$out" | grep -q "exec format error" || { echo "$out" | tail -20; return 1; }
    done
    echo "$out" | tail -20
    return 1
}
W=build/gate5/nobt
rm -rf build/gate5
mkdir -p "$W"
git ls-files -z | rsync -a --from0 --files-from=- ./ "$W/"   # the tracked files as they are in the working tree
for p in $BT; do
    [ -e "$W/$p" ] || { echo "gate 5: $p is not in the tree (update tools/check_default_build.sh's list)"; exit 1; }
    rm -rf "${W:?}/$p"
done
(cd "$W" && bld env -u FELUCCA_BLE -u BLE_MEASURE sh ./build.sh) || {
    echo "gate 5: the plain build fails without the Bluetooth files"; exit 1; }
bld env -u FELUCCA_BLE -u BLE_MEASURE sh ./build.sh
if cmp -s "$W/build/obervave.bin" build/obervave.bin; then
    echo "gate 5: the plain build is byte-identical without the Bluetooth files ($(wc -c < build/obervave.bin) B)"
    rm -rf build/gate5
else
    echo "gate 5: the plain build differs without the Bluetooth files: $(cmp "$W/build/obervave.bin" build/obervave.bin \
        | head -1); kept in $W"
    exit 1
fi
