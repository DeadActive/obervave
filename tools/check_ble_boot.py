#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# BLE MIDI: 2026 DEADACTIVE (after fm1-lsdj 548ce73's gate 4; Hortator; Felucca 1.6), on OberVave 3.0.1
"""Gate 3 (spec): no Bluetooth start-up reachable at boot. The graph is built from obervave.dis: an edge for every
function named in an instruction's `<name[+off] : addr>` operand (calls, tail gotos, and the address loads of the
BLE unit's long calls, `rN = … <fn : …>` then `call rN`). Walked from fm1_cstart, every isr_* entry and the fault
handlers; reaching a START function is an error, except through ALLOWED (the user's trigger: the Bluetooth page's
BLUETOOTH ON), and ALLOWED itself may be referenced only from TRIGGERS.

  check_ble_boot.py build/obervave.dis
  check_ble_boot.py --selftest"""
import re
import sys

START = {"ble_start", "btstack_init", "btctrler_task_init", "bt_ble_init", "bt_master_ble_init"}
ALLOWED = {"ble_user_start"}
TRIGGERS = {"ble_console", "bt_page_on"}   # the only functions that may reference ALLOWED (the user's commands:
                                           # the console's `ble start` (none in OberVave), the page's BLUETOOTH ON)
CALLERS = {"bt_page_on": {"bt_page_input", "bt_page_auto"},
           "bt_page_auto": {"bt_service"},
           "bt_service": {"app_main"}}
# a trigger in the core, only from: the page's input (OberVave: bt_page_input, noinline); AUTOSTART (bt_page_auto,
# once after a clean boot, the user's setting) and that only from the main loop's Bluetooth service, itself only from
# the main loop (app_main). Never the boot itself, an ISR or anything else
ROOTS = ("fm1_cstart", "fm1_fatal_common", "fm1_fault_c")
LABEL = re.compile(r"^([A-Za-z_.$][\w.$]*):\s*$")
REF = re.compile(r"<([A-Za-z_.$][\w.$]*)(?:\+[^:>]*)?\s*:\s*[0-9a-f]+\s*>", re.I)


def graph(text):
    g, name = {}, None
    for line in text.splitlines():
        m = LABEL.match(line)
        if m and not m.group(1).startswith("."):  # (a local label, e.g. a jump table's .GJTIS…, stays in its function)
            name = m.group(1)
            g[name] = set()
        elif name:
            g[name].update(r.strip() for r in REF.findall(line))
    return {n: {r for r in refs if r in g} for n, refs in g.items()}     # functions only (labels of the listing)


def reachable_starts(text):
    g = graph(text)
    todo = [r for r in g if r in ROOTS or r.startswith("isr_")]
    seen, bad = set(), set()
    while todo:
        f = todo.pop()
        if f in seen or f in ALLOWED:
            continue
        seen.add(f)
        if f in START:
            bad.add(f)
        todo.extend(g.get(f, ()))
    for f, refs in g.items():           # the trigger from anywhere but the user's command is a start at boot
        if f not in TRIGGERS and f not in ALLOWED:
            bad.update(f"{a} (from {f})" for a in refs & ALLOWED)
        bad.update(f"{t} (from {f})" for t in refs & CALLERS.keys() if f != t and f not in CALLERS[t])
    return sorted(bad)


def selftest():
    def fn(name, *body):
        return f"{name}:\n" + "".join(f"  2000000:    00 49    \t{b}\n" for b in body)
    boot = fn("fm1_cstart", "call -4 <app_main : 2000010 >")
    assert reachable_starts(boot + fn("app_main", "call -4 <ble_start : 2000030 >") + fn("ble_start", "rts")) == \
        ["ble_start"], "a direct call to ble_start not caught"
    # the trigger itself is allowed only from the user's command (the Bluetooth page): the main loop calling it at
    # boot (a saved "on") must stop the build
    hook = boot + fn("app_main", "call -4 <ble_user_start : 2000030 >") + fn("ble_user_start",
                                                                              "call -4 <ble_start : 2000040 >")
    assert reachable_starts(hook + fn("ble_start", "rts")) == ["ble_user_start (from app_main)"], \
        "the trigger called from outside the page not caught"
    page = boot + fn("app_main", "call -4 <bt_page_input : 2000030 >") + \
        fn("bt_page_input", "call -4 <bt_page_on : 2000038 >") + \
        fn("bt_page_on", "call -4 <ble_user_start : 2000040 >") + fn("ble_user_start", "call -4 <ble_start : 2000048 >")
    assert reachable_starts(page + fn("ble_start", "rts")) == [], "the Bluetooth page's BLUETOOTH ON flagged"
    # the page's trigger itself only from the page's input: from the main loop or the boot (a saved "on") it is a
    # start at boot
    for hook in ("app_main", "fm1_cstart"):
        bad = fn("fm1_cstart", "call -4 <app_main : 2000010 >" if hook != "fm1_cstart" else "call -4 <bt_page_on : 2000038 >") + \
            (fn("app_main", "call -4 <bt_page_on : 2000038 >") if hook == "app_main" else "") + \
            fn("bt_page_on", "call -4 <ble_user_start : 2000040 >") + fn("ble_user_start", "call -4 <ble_start : 2000048 >")
        assert reachable_starts(bad + fn("ble_start", "rts")) == [f"bt_page_on (from {hook})"], \
            f"the page's trigger called from {hook} not caught"
    # AUTOSTART (bt_page.c bt_page_auto): the page's trigger from the main loop's Bluetooth service, never from the
    # boot itself or an ISR
    auto = boot + fn("app_main", "call -4 <bt_service : 2000030 >") + fn("bt_service", "call -4 <bt_page_auto : 2000034 >") + \
        fn("bt_page_auto", "call -4 <bt_page_on : 2000038 >") + fn("bt_page_on", "call -4 <ble_user_start : 2000040 >") + \
        fn("ble_user_start", "call -4 <ble_start : 2000048 >")
    assert reachable_starts(auto + fn("ble_start", "rts")) == [], "AUTOSTART from the main loop's service flagged"
    for hook in ("fm1_cstart", "fm1_timer5_irq"):
        bad = fn(hook, "call -4 <bt_page_auto : 2000034 >") + fn("bt_page_auto", "call -4 <bt_page_on : 2000038 >") + \
            fn("bt_page_on", "call -4 <ble_user_start : 2000040 >") + fn("ble_user_start", "call -4 <ble_start : 2000048 >")
        assert reachable_starts(bad + fn("ble_start", "rts")) == [f"bt_page_auto (from {hook})"], \
            f"AUTOSTART called from {hook} not caught"
    bad = boot + fn("fm1_alnk0_irq", "call -4 <bt_service : 2000030 >") + fn("bt_service", "rts")
    assert reachable_starts(bad) == ["bt_service (from fm1_alnk0_irq)"], "the service from an ISR not caught"
    lng = boot + fn("app_main", "r0 = 33554496 <ble_start : 2000040 >", "call r0") + fn("ble_start", "rts")
    assert reachable_starts(lng) == ["ble_start"], "a long call (address load) not caught"
    isr = fn("isr_timer5", "call -4 <fm1_timer5_irq : 2000050 >") + fn("fm1_timer5_irq",
                                                                     "goto 4 <btstack_init+0x4 : 2000064 >")
    assert reachable_starts(isr + fn("btstack_init", "rts")) == ["btstack_init"], "an ISR path not caught"
    data = boot + fn("app_main", "r1 = 29411328 <ble_heap : 1c0c800 >") + fn("ble_start", "rts")
    assert reachable_starts(data) == [], "a data symbol followed as a call"
    # a jump table's local labels (.GJTIS…, .GJTIE…) are inside the function: its calls after them count
    jt = boot + fn("app_main", "goto 8 <.GJTIE1_0_0_+0x8 : 2000048 >") + \
        fn(".GJTIS1_0_0_", "call -4 <ble_start : 2000060 >") + fn("ble_start", "rts")
    assert reachable_starts(jt) == ["ble_start"], "a call after a jump table's label not caught"
    print("check_ble_boot: selftest ok")


def main(argv):
    if argv[1:] == ["--selftest"]:
        selftest()
        return 0
    bad = reachable_starts(open(argv[1]).read())
    if bad:
        print("gate 3: BT start-up reachable at boot:", ", ".join(bad))
        return 1
    print("gate 3: no BT start-up reachable at boot (only through", ", ".join(sorted(ALLOWED)) + ")")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
