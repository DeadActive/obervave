#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# BLE MIDI: 2026 DEADACTIVE (from Felucca's ble-kit; fm1-lsdj 548ce73; Hortator; Felucca 1.6), on OberVave 3.0.1
"""Gate 2 (Felucca's docs/ble): the update path in a Bluetooth build.
  2a: its sources are the base upstream release's (tools/ble_base.txt UPSTREAM): check_update_path.py --sources [BASE];
      platform.c (Dinghy's platform: the start-up, TIMER5, the main loop's update hook, the flash driver's window, and
      the audio interrupt) may differ only by #if FELUCCA_BLE blocks: its plain view (those blocks as the plain build
      sees them) is the base's, byte for byte
  2b: its machine code equals the reference core's: the same core object linked with empty BLE entry points
      (tools/build.py links it in every BLE build: build/obervave_ref.dis; build.py calls compare())
      check_update_path.py build/obervave.dis build/obervave_ref.dis
  check_update_path.py --selftest

Functions compared: every function whose name matches PATTERNS, plus everything in .ram_text (the flash
driver). Instructions are compared as text with addresses and encodings dropped and branch / load targets
normalised to symbol names, so moving code around is fine and changing it is not."""
import re
import sys

PATTERNS = [r"^usb_", r"^ep[0-4]_", r"^e0_", r"^sie_", r"^sysex_byte$", r"^midi_in_event$", r"^midi_enqueue$",
            r"^ota_", r"^fm1_timer5_irq$", r"^fm1_cstart$", r"^fl_", r"^st_read$", r"^uac_",
            # OberVave (Dinghy's platform.c): the main loop's update hook (the update session, the UBOOT key) is
            # platform_poll; the USB rescue (recovery.c) and the boot guard
            r"^platform_poll$", r"^recovery_", r"^bootguard_", r"^fm1_enter_uboot$", r"^fm1_wdt_feed$",
            # the rest of usb.c, libc.c, midi_uart.c (TIMER5 polls the TRS jack), and the HAL functions the update
            # path and the fault handlers run
            r"^midi_out_event$", r"^mem(cpy|set|cmp)$", r"^uart_midi_", r"^fm1_delay_ms$", r"^fm1__?p33_",
            r"^fm1_fault_c$", r"^fm1_fatal_common$", r"^fm1_reboot$", r"^fm1_fault$",
            # .. and those the compiler inlines today (compared when it stops)
            r"^get_desc$", r"^usb_start$", r"^usb_detach$", r"^usb_retry$"]
# (not cdc_task: the console's commands inline into it and change with them; the CDC transport is in usb_poll)
LABEL = re.compile(r"^([A-Za-z_.$][\w.$]*):\s*$")
INSN = re.compile(r"^\s*[0-9a-f]+:\s+(?:[0-9a-f]{2} )+\s*\t(.*)$")
COMMENT = re.compile(r"\s*#\s*<[^>]*>")          # objdump's "# <sym : addr>" notes after a constant
BARE = re.compile(r"(?<![\w<+-])(\d{8})(?![\w>])")   # an 8-digit decimal constant (RAM / XIP addresses have 8)
ANNOT = re.compile(r"(-?\d+) <([^:>]+?)\s*:\s*([0-9a-f]+)\s*>", re.I)


def normalise(text, fn):
    """`N <sym+off : addr>` operands, independent of where things were placed: a call keeps its callee's name; a
    branch inside the function keeps its offset; any other RAM / XIP address is a data address, `<addr>` (objdump
    names the same address differently from build to build, e.g. _bss_end / an empty pool after it); anything else
    is a plain constant objdump annotated with the nearest symbol, and stays the number N"""
    def one(m):
        num, sym, addr = m.groups()
        sym = sym.strip()
        if not 0x01C00000 <= int(addr, 16) < 0x02100000:
            return num
        if text.lstrip().startswith("call"):
            return f"<{sym.split('+')[0]}>"
        if sym == fn or sym.startswith(fn + "+"):
            return f"<{sym[len(fn):] or '+0'}>"
        return "<addr>"

    def bare(m):                    # an unannotated constant in the RAM / XIP range: an address (it moves)
        return "<addr>" if 0x01C00000 <= int(m.group(1)) < 0x02100000 else m.group(0)
    return BARE.sub(bare, ANNOT.sub(one, COMMENT.sub("", text))).strip()


ADDR = re.compile(r"^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*\t")
RELCALL = re.compile(r"^call (-?\d+)$")


def entries(text):
    """address -> function, from each label's first instruction (for calls objdump left without a name)"""
    at, pending = {}, None
    for line in text.splitlines():
        m = LABEL.match(line)
        if m:
            pending = None if m.group(1).startswith(".") else m.group(1)
            continue
        m = ADDR.match(line)
        if m and pending:
            at[int(m.group(1), 16)] = pending
            pending = None
    return at


def functions(text):
    out, name, at = {}, None, entries(text)
    for line in text.splitlines():
        m = LABEL.match(line)
        if m and m.group(1).startswith("."):  # a local label (a jump table's .GJTIS…): inside the function
            continue
        if m:
            name = m.group(1)
            out[name] = []
            continue
        m = INSN.match(line)
        if m and name:
            rel, a = RELCALL.match(m.group(1).strip()), ADDR.match(line)
            if rel and a:           # `call N` with no annotation: the callee is at this instruction's end + N
                callee = at.get(int(a.group(1), 16) + len(a.group(2).split()) + int(rel.group(1)))
                if callee:
                    out[name].append(f"call <{callee}>")
                    continue
            out[name].append(normalise(m.group(1), name))
        elif line.startswith("| ") and name:      # a reference file's instruction line (render)
            out[name].append(line[2:].rstrip("\n"))
    for body in out.values():                     # alignment padding after the last instruction is not code
        while body and body[-1] == "nop":
            body.pop()
    return out


def selected(funcs):
    return {n: body for n, body in funcs.items() if any(re.search(p, n) for p in PATTERNS)}


def render(funcs):
    return "".join(f"{n}:\n" + "".join(f"| {i}\n" for i in funcs[n]) for n in sorted(funcs))


def compare(dis_text, ref_text):
    now, ref = selected(functions(dis_text)), functions(ref_text)
    errors = [f"missing in this build: {n}" for n in ref if n not in now]
    errors += [f"changed: {n}" for n in ref if n in now and now[n] != ref[n]]
    errors += [f"new update-path function (not in the reference): {n}" for n in now if n not in ref and n not in NEW_2B]
    return errors


# gate 2c: the update path's functions whose code FELUCCA_BLE changes, and why. Anything else that differs between
# the plain compile and the Bluetooth compile of the core (tools/build.py gate2c_listings) stops the build.
# For a listed function the instructions cannot be compared (the allocator renames registers around the change), so
# its callees are: (why, callees only the Bluetooth compile may have, callees only the plain compile may have).
KNOWN_2C = {
    "fm1_cstart": ("core_ble_api.c core_irq_attach is a second user of fm1_guard_lock_top, so it is called instead of "
                   "inlined; the update record's clean-up (ota_boot_cleanup, inlined) erases through the flash "
                   "driver's window, FL_RANGE_OK (platform.c: the Bluetooth build's store, not 1.3-1.5's pattern "
                   "sectors)", {"fm1_guard_lock_top"}, set()),
    "fm1_reboot": ("core_ble_api.c core_reboot is a second user of fm1_reboot, so it is a function of its own",
                   {"fm1__p33_op"}, set()),
    "fm1_fault_c": ("it calls fm1_reboot instead of inlining it (above)", {"fm1_reboot"}, set()),
    "midi_out_event": ("core_ble_api.c core_midi_out is a second user of usb.c's midi_out_event, so it is a function "
                       "of its own (inlined into its callers in the plain build)", set(), set()),
}
# the RAM flash driver's functions (hal/fm1_flash.h), which check every erase and write against FL_RANGE_OK: in the
# Bluetooth build the window has the Bluetooth store (0xB9000..0xBAFFF) and not 1.3-1.5's pattern sectors
# (0xF4000..0xFBFFF), so their bounds differ. Compared with the window's constants masked: everything else the same
WINDOW_2C = {"fl_erase4k_ram", "fl_prog_ram"}
WINDOW_CONST = re.compile(r"\b0x[0-9A-F]{5}\b|(?<=> )\d+\)")
# gate 2b: update-path functions the reference link drops (--gc-sections: the core never calls them as functions)
# and the Bluetooth link keeps because the BLE unit calls them; libc.c is unchanged (gate 2a)
NEW_2B = {"memset", "memcmp", "memcpy"}


def callees(body):
    return set(re.findall(r"call <([\w.$]+)>", "\n".join(body or [])))


def compare_builds(plain_text, ble_text):
    """gate 2c -> (errors, how many functions are identical, the KNOWN_2C functions that differ)"""
    a, b = selected(functions(plain_text)), selected(functions(ble_text))
    errors, known, same = [], [], 0
    for n in sorted(set(a) | set(b)):
        if a.get(n) == b.get(n):
            same += 1
        elif n in WINDOW_2C and n in a and n in b:
            if [WINDOW_CONST.sub("K", x) for x in a[n]] != [WINDOW_CONST.sub("K", x) for x in b[n]]:
                errors.append(f"{n} differs beyond FL_RANGE_OK's window constants between the plain and the "
                              f"Bluetooth compile")
            known.append(n)
        elif n in KNOWN_2C:
            _, more, fewer = KNOWN_2C[n]
            new, gone = callees(b.get(n)) - callees(a.get(n)), callees(a.get(n)) - callees(b.get(n))
            if new - more:
                errors.append(f"{n} calls {', '.join(sorted(new - more))} in the Bluetooth compile only (not listed)")
            if gone - fewer:
                errors.append(f"{n} no longer calls {', '.join(sorted(gone - fewer))} in the Bluetooth compile")
            known.append(n)
        else:
            how = "only in the Bluetooth compile" if n not in a else "only in the plain compile" if n not in b else \
                "differs between the plain and the Bluetooth compile"
            errors.append(f"{n} {how} (not listed in KNOWN_2C)")
    errors += [f"{n} is listed in KNOWN_2C but is the same in both compiles (remove it)" for n in KNOWN_2C
               if n in a and n in b and a[n] == b[n]]
    return errors, same, known


def selftest():
    two = ("usb_poll:\n  2000000:    00 49    \tr0 = 1\n  2000002:    80 00    \trts\n"
           "platform_poll:\n  2000010:    00 49    \tr0 = 1\n  2000012:    80 00    \trts\n")
    real_known = dict(KNOWN_2C)
    try:
        KNOWN_2C.clear()
        assert compare_builds(two, two) == ([], 2, []), "gate 2c: identical compiles flagged"
        e, same, known = compare_builds(two, two.replace("usb_poll:\n  2000000:    00 49    \tr0 = 1", "usb_poll:\n  2000000:    00 49    \tr0 = 2"))
        assert len(e) == 1 and "usb_poll" in e[0] and same == 1, "gate 2c: a changed update-path function not caught"
        KNOWN_2C["platform_poll"] = ("test", {"ble_console"}, set())
        changed = two.replace("platform_poll:\n  2000010:    00 49    \tr0 = 1", "platform_poll:\n  2000010:    00 49    \tr0 = 3")
        assert compare_builds(two, changed) == ([], 1, ["platform_poll"]), "gate 2c: a listed function flagged"
        ok = changed.replace("\tr0 = 3\n", "\tr0 = 3\n  2000011:    00 49    \tcall -4 <ble_console : 2000030 >\n")
        assert compare_builds(two, ok) == ([], 1, ["platform_poll"]), "gate 2c: a listed function's listed new callee flagged"
        bad = changed.replace("\tr0 = 3\n", "\tr0 = 3\n  2000011:    00 49    \tcall -4 <ble_user_start : 2000030 >\n")
        e, _, _ = compare_builds(two, bad)
        assert len(e) == 1 and "ble_user_start" in e[0], "gate 2c: a listed function's unlisted new callee not caught"
        withcall = two.replace("platform_poll:\n  2000010:    00 49    \tr0 = 1\n", "platform_poll:\n  2000010:    00 49    \tr0 = 1\n  2000011:    00 49    \tcall -4 <ota_prog : 2000030 >\n")
        e, _, _ = compare_builds(withcall, changed)
        assert len(e) == 1 and "no longer calls ota_prog" in e[0], "gate 2c: a listed function's lost callee not caught"
        e, _, _ = compare_builds(two, two)
        assert len(e) == 1 and "remove it" in e[0], "gate 2c: a stale KNOWN_2C entry not caught"
        e, _, _ = compare_builds(two, changed + "usb_new:\n  2000020:    80 00    \trts\n")
        assert len(e) == 1 and "usb_new only in the Bluetooth compile" in e[0], "gate 2c: a new function not caught"
    finally:
        KNOWN_2C.clear()
        KNOWN_2C.update(real_known)
    dis = ("usb_poll:\n  2000000:    00 49    \tr0 = 1\n  2000002:    80 00    \trts\n"
           "other:\n  2000010:    80 00    \trts\n")
    ref = render(selected(functions(dis)))
    assert compare(dis, ref) == [], "identical listing flagged"
    assert compare(dis.replace("r0 = 1", "r0 = 2"), ref), "changed instruction not caught"
    assert compare(dis.replace("usb_poll:", "usb_poll2:"), ref), "missing function not caught"
    moved = dis.replace("2000000:", "2000100:")
    assert compare(moved, ref) == [], "moved code flagged"
    # objdump's real forms: upper-case offsets, relative call / branch numbers that change with layout, constants
    # annotated with a nonsense nearest symbol (keep the constant), address constants (normalise to the symbol)
    a = ("usb_x:\n  2000000:    00 49    \tif (r1 == 0) goto 38 <usb_x+0x2A : 2002d6c >\n"
         "  2000002:    00 49    \tcall -40632 <memset : 2001fae >\n"
         "  2000004:    00 49    \tr3 = -20000 <_data_load+0xFFFFFFFFFDFEADC8 : ffffffffffffb1e0 >\n"
         "  2000006:    00 49    \tr0 = 33720110 <usb_x.D1 : 202872e >\n")
    b = (a.replace("2002d6c", "2003faa").replace("-40632", "-50000").replace("2001fae", "2003fae")
          .replace("FFFFFFFFFDFEADC8", "FFFFFFFFFDF84D5C").replace("33720110", "33800000").replace("202872e", "2035f00"))
    ref2 = render(selected(functions(a)))
    assert compare(b, ref2) == [], "layout-only changes flagged"
    assert compare(b.replace("-20000", "-20004"), ref2), "a changed constant not caught"
    assert compare(b.replace("<memset", "<memcpy"), ref2), "a changed call target not caught"
    c1 = "usb_y:\n  2000000:    00 49    \tr1 = -12711  # <_data_load+0xFFFFFFFFFDFECA40 : ffffffffffffce58 >\n"
    ref3 = render(selected(functions(c1)))
    assert compare(c1.replace("FDFECA40", "FDF869D4"), ref3) == [], "a comment annotation flagged"
    assert compare(c1.replace("-12711", "-12712"), ref3), "the constant before a comment not caught"
    # the same address may carry several names (an empty pool after _bss_end): addresses compare by role, branches
    # inside the function by offset
    d1 = ("usb_z:\n  2000000:    00 49    \tr3 = 29410364 <_bss_end : 1c0c43c >\n"
          "  2000002:    00 49    \tgoto 10 <usb_z+0x2A : 200002a >\n")
    ref4 = render(selected(functions(d1)))
    assert compare(d1.replace("_bss_end", "tws_bulk_pool_end"), ref4) == [], "an address alias flagged"
    assert compare(d1.replace("usb_z+0x2A", "usb_z+0x2C"), ref4), "a moved branch target not caught"
    # alignment padding after a function's last instruction (objdump puts it in the function) is not code: what
    # follows a function in its section decides it, so it comes and goes with the layout; a nop inside is code
    e1 = ("usb_w:\n  2000000:    00 49    \tr0 = 1\n  2000002:    80 00    \trts\n  2000004:    00 00    \tnop\n"
          "other:\n  2000010:    80 00    \trts\n")
    ref5 = render(selected(functions(e1)))
    assert compare(e1.replace("  2000004:    00 00    \tnop\n", ""), ref5) == [], "trailing padding flagged"
    e2 = e1.replace("\tr0 = 1\n", "\tr0 = 1\n  2000001:    00 00    \tnop\n")
    assert compare(e2, ref5), "a nop inside the function not caught"
    # the main loop holds the inlined update session, the UBOOT key and the SysEx UBOOT request (review #1)
    f1 = ("platform_poll:\n  2000000:    00 49    \tcall -40 <ota_prog : 2001fae >\n  2000002:    80 00    \trts\n")
    ref6 = render(selected(functions(f1)))
    assert compare(f1.replace("<ota_prog", "<ota_show"), ref6), "a changed call in platform_poll not caught"
    for fn in ("platform_poll", "fm1_enter_uboot", "fm1_wdt_feed"):
        assert selected({fn: ["rts"]}), f"{fn} not compared"
    # a jump table's local labels (.GJTIS…) are inside the function: a change after them is caught
    g1 = ("usb_j:\n  2000000:    00 49    \tr0 = 1\n.GJTIS7_0_0_:\n  2000002:    00 49    \tr1 = 2\n"
          "  2000004:    80 00    \trts\n")
    ref7 = render(selected(functions(g1)))
    assert compare(g1.replace("r1 = 2", "r1 = 3"), ref7), "a change after a jump table's label not caught"
    assert compare(g1.replace(".GJTIS7_0_0_", ".GJTIS9_0_0_"), ref7) == [], "a renumbered local label flagged"
    # a bare constant in the RAM / XIP range (objdump leaves some address loads unannotated, e.g. inside `if (...)
    # {` forms) is an address: it moves with the layout; a constant outside the range is compared
    h1 = "usb_k:\n  2000000:    00 49    \tif (r0 != 7) {\n  2000002:    00 49    \tr1 = 33848701\n"
    ref8 = render(selected(functions(h1)))
    assert compare(h1.replace("33848701", "33884012"), ref8) == [], "a moved unannotated address flagged"
    h2 = "usb_k:\n  2000000:    00 49    \tr1 = 4096\n"
    assert compare(h2.replace("4096", "4097"), render(selected(functions(h2)))), "a changed small constant not caught"
    # a relative call objdump did not annotate: resolved by address, so the same callee at another distance compares
    # equal and another callee does not (4 encoding bytes: the callee is at the instruction's end + N)
    r1 = ("usb_a:\n  2000000:    00 49 00 00    \tcall 12\n  2000004:    80 00    \trts\n"
          "fl_one:\n  2000010:    80 00    \trts\nfl_two:\n  2000020:    80 00    \trts\n")
    assert functions(r1)["usb_a"][0] == "call <fl_one>", "an unannotated relative call not resolved"
    r2 = r1.replace("2000010:", "2000018:").replace("call 12", "call 20")
    assert compare(r2, render(selected(functions(r1)))) == [], "the same callee at another distance flagged"
    assert compare(r1.replace("call 12", "call 28"), render(selected(functions(r1)))), "another callee not caught"
    real, real_read = globals()["git"], globals()["read_now"]   # gate 2a's rule for added files, with a fake git
    base_pf = "a\n#if X\nb\n#endif\nc\n"
    try:
        globals()["read_now"] = lambda path: base_pf
        globals()["git"] = lambda *a: base_pf if a[0] == "show" else "A\tfirmware/hal/fm1_ble_hal.h\n"
        assert source_errors("x") == [], "an allowed new HAL file flagged"
        globals()["git"] = lambda *a: base_pf if a[0] == "show" else "M\tfirmware/src/ota.c\nA\tfirmware/hal/fm1_new.h\n"
        assert len(source_errors("x")) == 2, "a changed source / an unlisted new file not caught"
        globals()["git"] = lambda *a: base_pf if a[0] == "show" else ""
        globals()["read_now"] = lambda path: "a\n#if FELUCCA_BLE\nnew\n#if Y\nz\n#endif\n#else\n#if X\nb\n#endif\n" \
                                             "#endif\nc\n#if FELUCCA_BLE\nmore\n#endif\n"
        assert source_errors("x") == [], "platform.c with FELUCCA_BLE blocks (plain view = base) flagged"
        globals()["read_now"] = lambda path: "a\n#if X\nb2\n#endif\nc\n"
        assert len(source_errors("x")) == 1, "platform.c changed outside FELUCCA_BLE blocks not caught"
        globals()["read_now"] = lambda path: "a\n#if X\nb\n#endif\nc\n#if !FELUCCA_BLE\nd\n#endif\n"
        assert len(source_errors("x")) == 1, "a plain-only block added to platform.c not caught"
    finally:
        globals()["git"], globals()["read_now"] = real, real_read
    w1 = "fl_erase4k_ram:\n  2000000:    00 49    \tr2 = r0 - 0xF4000\n  2000002:    00 49    \tif (r2 > 32768) goto 4 <fl_erase4k_ram+0x8 : 2000008 >\n"
    w2 = w1.replace("0xF4000", "0xB9000").replace("32768", "8192")
    assert compare_builds(w1, w2) == ([], 0, ["fl_erase4k_ram"]), "gate 2c: the driver's window constants flagged"
    e, _, _ = compare_builds(w1, w2.replace("r2 = r0", "r3 = r0"))
    assert len(e) == 1 and "fl_erase4k_ram" in e[0], "gate 2c: a driver change beyond the window not caught"
    print("check_update_path: selftest ok")


# gate 2a: the update path's sources are the base upstream release's. Modified or deleted files fail; added files
# only as listed (the Bluetooth build's new HAL header and context switch)
SOURCES = ["firmware/src/usb.c", "firmware/src/ota.c", "firmware/src/libc.c", "firmware/src/recovery.c",
           "firmware/src/midi_uart.c", "firmware/src/bootguard.h", "firmware/hal", "firmware/loader",
           # the start-up and the memory map: gate 2b links the same core object twice, so a change here would
           # otherwise reach a Bluetooth device build unseen
           "firmware/crt0.S", "firmware/app.ld"]
NEW_OK = {"firmware/hal/fm1_ble_hal.h", "firmware/hal/fm1_ctx.h", "firmware/hal/fm1_ctx.S"}
# the update path's sources that carry #if FELUCCA_BLE blocks: their plain view must be the base's
GUARDED = ["firmware/src/platform.c"]


def plain_view(text):
    """the file as the plain build (FELUCCA_BLE 0) sees it, textually: each `#if FELUCCA_BLE` block's first branch
    dropped and its `#else` branch kept, the block's directives dropped (nested #if inside are followed). A plain-only
    block (`#if !FELUCCA_BLE`) is not a FELUCCA_BLE block: it stays as text (and so differs from the base)"""
    out, stack = [], []                 # per open #if: (is a FELUCCA_BLE block, its branch is the plain one)
    for ln in text.splitlines(keepends=True):
        d = ln.strip()
        live = all(keep for _, keep in stack)
        if re.match(r"#\s*if(def|ndef)?\b", d):
            ble = bool(re.match(r"#\s*if\s+FELUCCA_BLE\s*(/[*/].*)?$", d))
            stack.append((ble, not ble))
            if not ble and live:
                out.append(ln)
            continue
        if stack and re.match(r"#\s*(else|elif)\b", d):
            ble, keep = stack[-1]
            if ble:
                stack[-1] = (True, True)
                continue
            if all(k for _, k in stack[:-1]):
                out.append(ln)
            continue
        if stack and re.match(r"#\s*endif\b", d):
            ble, _ = stack.pop()
            if not ble and all(k for _, k in stack):
                out.append(ln)
            continue
        if live:
            out.append(ln)
    return "".join(out)


def read_now(path):
    from pathlib import Path
    return (Path(__file__).resolve().parents[1] / path).read_text()


def git(*a):
    import subprocess
    from pathlib import Path
    r = subprocess.run(["git", *a], cwd=Path(__file__).resolve().parents[1], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(f"check_update_path: git {' '.join(a)}: {r.stderr.strip()}")
    return r.stdout


def last_release():
    """the upstream release this tree's update path is taken from: UPSTREAM in tools/ble_base.txt (a merge of a
    later upstream release moves it, with the loader's hash)"""
    import os
    from pathlib import Path
    f = Path(os.environ.get("BLE_BASE_FILE") or Path(__file__).resolve().parent / "ble_base.txt")
    for ln in f.read_text().splitlines():
        if ln.startswith("UPSTREAM "):
            return ln.split()[1]
    raise SystemExit(f"check_update_path: {f} has no UPSTREAM")


def source_errors(base):
    errors = []
    for ln in git("diff", "--name-status", base, "--", *SOURCES).splitlines():
        status, path = ln.split("\t", 1)
        if not (status == "A" and path in NEW_OK):
            errors.append(f"update path source {path} differs from {base} ({status})")
    for path in GUARDED:
        if plain_view(read_now(path)) != git("show", f"{base}:{path}"):
            errors.append(f"update path source {path}: its plain view (without its FELUCCA_BLE blocks) differs "
                          f"from {base}")
    return errors


def main(argv):
    if argv[1:2] == ["--sources"]:
        base = argv[2] if len(argv) > 2 else last_release()
        errors = source_errors(base)
        for e in errors:
            print("gate 2a:", e)
        if not errors:
            print(f"update path sources: identical to {base}")
        return 1 if errors else 0
    if argv[1:] == ["--selftest"]:
        selftest()
        return 0
    errors = compare(open(argv[1]).read(), render(selected(functions(open(argv[2]).read()))))
    for e in errors:
        print("update path:", e)
    print("update path: unchanged" if not errors else f"update path: {len(errors)} difference(s)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
