/* SPDX-License-Identifier: GPL-3.0-only
 * BLE MIDI: 2026 DEADACTIVE (Felucca's tests/bt_page_test.c and bt_page_render.c on OberVave's harness) */
/* The Bluetooth page on the host (firmware/src/bt_page.c): GLO's fourth page (GLOBAL, VOICE, OUTPUT, BLUETOOTH, and
 * any other page closes it), the cursor (PRESETS, wrapping), BLUETOOTH ON / OFF (the only start: ble_user_start
 * once), AUTOSTART (after a clean boot only, 3 s on; never with it OFF), ROLE (DEVICE: no device lines), MODE and
 * RECONNECT (stop at the ends, OCT+ wraps), + ADD A DEVICE (the 8 s scan, BLE-MIDI first, the saved left out, a pick
 * saved first; NOTHING FOUND: OCT+ again), a device line (OCT+ uses it, a knob left asks FORGET, OCT- keeps, OCT+
 * forgets), the state and dim lines, the header's BT (none, struck, lit); and the page drawn in every state as
 * screens (OUT-*.ppm: tests/run_tests.sh makes PNGs of them) with nothing drawn outside its lines. */
#include "bt_test.h"

static const char *out_dir = "build/host";
static void shot(const char *name)                   /* the page (and the header) now -> OUT/bt-<name>.ppm */
{
    char path[256];
    btp.redraw = 1;
    draw_header();
    bt_page_draw();
    snprintf(path, sizeof path, "%s/bt-%s.ppm", out_dir, name);
    write_ppm_as(path);
}
static void draw(void) { btp.redraw = 1; bt_page_draw(); }   /* (the page draws at most every 100 ms) */
static uint32_t press(uint32_t btn) { return bt_page_input(1u << btn); }   /* a button's edge through the page */
static void turn(uint32_t enc, int32_t n) { host_enc[enc] += n; (void)bt_page_input(0); }
static bt_dev_t mk(uint8_t a0, uint32_t kind, const char *name)
{
    bt_dev_t d;
    memset(&d, 0, sizeof d);
    d.addr[0] = a0;
    d.addr[1] = 0x3F;
    d.addr[5] = 0xC2;
    d.kind = (uint8_t)kind;
    bt_cpy(d.name, name, sizeof d.name);
    return d;
}
static void scan_dev(uint32_t i, uint8_t a0, const char *name, int midi, int rssi)
{
    memset(&bs.dev[i], 0, sizeof bs.dev[i]);
    bs.dev[i].addr[0] = a0;
    bs.dev[i].addr[5] = 0x55;
    bs.dev[i].midi = (uint8_t)midi;
    bs.dev[i].rssi = (int8_t)rssi;
    bs.dev[i].seen_ms = fm1_ms;
    bt_cpy(bs.dev[i].name, name, sizeof bs.dev[i].name);
    if (bs.ndev <= i)
        bs.ndev = i + 1u;
}
static int outside_lines(void)                       /* a pixel lit in the page's margins (x < 2, x > 237) */
{
    uint32_t x, y, bad = 0;
    for (y = BT_Y0; y < 240u; y++)
        for (x = 0; x < 240u; x++)
            if ((x < 2u || x > 237u) && fb[y * 240u + x] != swap16(C_PANEL) && fb[y * 240u + x] != C_PANEL)
                bad++;
    return (int)bad;
}

int main(int argc, char **argv)
{
    uint32_t pg, r, b;
    char st[40];
    if (argc > 1)
        out_dir = argv[1];
    bt_start();
    bt_fresh(0);
    lcd_fill(0, 0, 240, 240, C_PANEL);

    /* GLO's pages */
    ok("GLO: GLOBAL -> VOICE -> OUTPUT as before", bt_glo_tap(PG_GLOBAL) == PG_VOICE && bt_glo_tap(PG_VOICE) == PG_OUTPUT);
    pg = bt_glo_tap(PG_OUTPUT);
    ok("GLO on OUTPUT: the Bluetooth page (OUTPUT stays the page under it)", pg == PG_OUTPUT && bt_page_shown());
    ok("the page open (HOST): scanning while it is open (ON / NEAR / AWAY)", bt_dv.page == 1u);
    ok("BLUETOOTH is off at power-on, nothing started", !btp.on && bs.starts == 0u && bt_head_icon() == 0u);
    bt_state_text(st);
    ok("state line: OFF", str_eq(st, "OFF"));
    shot("off");
    ok("drawn within its lines", outside_lines() == 0);

    /* the cursor and the rows */
    r = bt_page_rows();
    ok("rows: five settings, + ADD A DEVICE (no device saved)", r == 6u);
    turn(ENC_PRESETS, -1);
    ok("PRESETS back from the first row wraps to the last", btp.sel == r - 1u);
    turn(ENC_PRESETS, 1);
    ok(".. and on wraps to the first", btp.sel == 0u);
    b = press(BTN_OCT_UP);
    ok("OCT+ on BLUETOOTH: ON, the stack started once, links wanted", btp.on && bs.starts == 1u && bs.want == 1);
    ok("OCT taken by the page (no octave change)", !(b & (3u << BTN_OCT_DOWN)));
    ok("the header's BT: struck (on, nothing linked)", bt_head_icon() == 1u);
    bs.state = BLE_ST_IDLE;
    turn(ENC_KNOB2, 1);
    ok("a knob right on BLUETOOTH ON: no second start", bs.starts == 1u);
    turn(ENC_KNOB1, -1);
    ok("a knob left: OFF, links dropped (the stack stays up)", !btp.on && bs.want == 0 && bt_head_icon() == 0u);
    turn(ENC_KNOB4, 1);
    ok("a knob right: ON again, still one start", btp.on && bs.starts == 1u && bs.want == 1);
    bt_state_text(st);
    ok("state line: ON, NO DEVICE SAVED", str_eq(st, "ON, NO DEVICE SAVED"));
    shot("on");

    turn(ENC_PRESETS, 1);                            /* AUTOSTART */
    turn(ENC_KNOB1, 1);
    {
        bt_set_t s;
        bt_set_get(&s);
        ok("AUTOSTART: a knob right -> ON, saved (the store)", s.autostart && bts_dirty);
    }
    turn(ENC_PRESETS, 1);                            /* ROLE */
    press(BTN_OCT_UP);
    ok("ROLE: OCT+ -> DEVICE, handed to the BLE unit, no device lines", bt_role() == 1u && bs_role == 1 &&
       bt_page_rows() == 5u);
    bs.state = BLE_ST_ADVERTISING;
    bt_state_text(st);
    ok("ROLE DEVICE: WAITING FOR A HOST", str_eq(st, "WAITING FOR A HOST"));
    shot("device");
    bs.state = BLE_ST_SUBSCRIBED;
    bt_cpy(conn_name, "iPad", sizeof conn_name);
    conn_up = 1;
    bt_state_text(st);
    ok("ROLE DEVICE: CONNECTED and the host's name; the header's BT lit", str_eq(st, "CONNECTED iPad") &&
       bt_head_icon() == 2u);
    conn_up = 0;
    bs.state = BLE_ST_IDLE;
    turn(ENC_KNOB3, -1);
    ok("ROLE: a knob left -> HOST", bt_role() == 0u && bs_role == 0);
    turn(ENC_PRESETS, 1);                            /* MODE */
    turn(ENC_KNOB1, 1);
    turn(ENC_KNOB1, 1);
    turn(ENC_KNOB1, 1);
    turn(ENC_KNOB1, 1);
    {
        bt_set_t s;
        bt_set_get(&s);
        ok("MODE: a knob right steps and stops at FM-LINK", s.mode == BT_MODE_LINK);
        press(BTN_OCT_UP);
        bt_set_get(&s);
        ok("MODE: OCT+ wraps to AUTO", s.mode == BT_MODE_AUTO);
    }
    turn(ENC_PRESETS, 1);                            /* RECONNECT */
    turn(ENC_KNOB2, -1);
    {
        bt_set_t s;
        bt_set_get(&s);
        ok("RECONNECT: a knob left stops at AUTO", s.rate == BT_RATE_AUTO);
        turn(ENC_KNOB2, 2);
        bt_set_get(&s);
        ok("RECONNECT: right -> FAST (one step a turn)", s.rate == BT_RATE_FAST);
        press(BTN_OCT_UP);
        press(BTN_OCT_UP);
        bt_set_get(&s);
        ok("RECONNECT: OCT+ twice wraps to AUTO", s.rate == BT_RATE_AUTO);
    }

    /* + ADD A DEVICE */
    turn(ENC_PRESETS, 1);
    ok("the cursor on + ADD A DEVICE", btp.sel == BT_SET_ROWS);
    press(BTN_OCT_UP);
    ok("OCT+: the ADD list, its scan", btp.add);
    scan_dev(0, 0x21, "SMC-Mixer", 1, -48);
    scan_dev(1, 0x22, "Felucca FM-1", 0, -40);
    scan_dev(2, 0x23, "Xkey Air 25", 1, -70);
    draw();
    ok("the list: BLE-MIDI devices first, each part by signal", btp.an == 3u && btp.adev[0].addr[0] == 0x21 &&
       btp.adev[1].addr[0] == 0x23 && btp.adev[2].addr[0] == 0x22);
    bt_state_text(st);
    ok("state line: SCANNING and its seconds", st[0] == 'S' && st[1] == 'C');
    shot("add");
    turn(ENC_PRESETS, 1);
    turn(ENC_PRESETS, 1);
    press(BTN_OCT_UP);
    ok("a pick: saved first (an FM-1 by its name), the list closed, the cursor on its line", !btp.add &&
       bt_devs_n() == 1u && btp.sel == BT_SET_ROWS);
    {
        bt_dev_t d;
        ok(".. Felucca FM-1 is an FM-1", bt_devs_get(0, &d) && d.kind == BT_KIND_FM1);
    }
    turn(ENC_PRESETS, 1);
    press(BTN_OCT_UP);
    draw();
    fm1_ms += BT_ADD_MS + 10u;
    draw();
    ok("the scan's 8 s over: the list stays as it is (the saved one left out)", btp.an == 2u &&
       btp.adev[0].addr[0] == 0x21);
    press(BTN_OCT_UP);                               /* SMC-Mixer */
    ok("a second pick: two saved, the last first", bt_devs_n() == 2u);
    turn(ENC_PRESETS, 1);                            /* (on ADD again) */
    turn(ENC_PRESETS, 1);
    press(BTN_OCT_UP);
    bs.ndev = 0;
    draw();
    fm1_ms += BT_ADD_MS + 10u;
    draw();
    bt_state_text(st);
    ok("nothing new around: NOTHING FOUND", str_eq(st, "NOTHING FOUND") && btp.an == 0u);
    shot("nothing");
    press(BTN_OCT_UP);
    ok(".. OCT+ scans again", btp.add && fm1_ms - btp.add_t0 < BT_ADD_MS);
    press(BTN_OCT_DOWN);
    ok("OCT-: back to the rows", !btp.add && btp.open);

    /* the device lines: use, forget */
    scan_dev(0, 0x21, "SMC-Mixer", 1, -48);
    memset(&bh.slot[BLE_SLOT_FM1], 0, sizeof bh.slot[0]);
    bh.slot[BLE_SLOT_FM1].st = BLE_LS_UP;
    {
        bt_dev_t d;
        bt_devs_get(1, &d);                          /* the FM-1 */
        memcpy(bh.slot[BLE_SLOT_FM1].addr, d.addr, 6);
    }
    btp.sel = BT_SET_ROWS;
    draw();
    bt_state_text(st);
    ok("state line: ON, 1 CONNECTED; the header's BT lit", str_eq(st, "ON, 1 CONNECTED") && bt_head_icon() == 2u);
    ok("device lines: the SMC-Mixer NEAR, the FM-1 ON", bt_devs_state(0) == 1 && bt_devs_state(1) == 2 &&
       str_eq(bt_dev_value(0), "NEAR") && str_eq(bt_dev_value(1), "ON"));
    shot("devices");
    btp.sel = BT_SET_ROWS + 1u;                      /* the FM-1, connected */
    press(BTN_OCT_UP);
    ok("OCT+ on a connected device: says CONNECTED, the order kept", toast && str_eq(toast, "CONNECTED"));
    btp.sel = BT_SET_ROWS;
    turn(ENC_KNOB1, -1);
    ok("a knob left on a device: FORGET <name>?", btp.confirm == 1u);
    bt_state_text(st);
    ok("state line: FORGET SMC-Mixer?", str_eq(st, "FORGET SMC-Mixer?"));
    shot("forget");
    press(BTN_OCT_DOWN);
    ok("OCT-: kept", btp.confirm == 0u && bt_devs_n() == 2u);
    turn(ENC_KNOB1, -1);
    press(BTN_OCT_UP);
    ok("OCT+: forgotten", bt_devs_n() == 1u);
    {
        bt_dev_t d;
        char lb[24];
        bt_devs_get(0, &d);
        bt_dev_label(&d, lb);
        ok("an FM-1's line: FM-1 and its address' last four hex digits", str_eq(lb, "FM-1 0022"));
    }
    bs.state = BLE_ST_FAILED;
    bs.err = "NO RF TRIM IN FLASH";
    bt_state_text(st);
    ok("a failed start: FAILED and the reason", str_eq(st, "FAILED NO RF TRIM IN FLASH"));
    shot("failed");
    bs.state = BLE_ST_IDLE;
    bs.err = 0;
    audio_gap_page = 12100u * FM1_TICKS_PER_US;
    audio_late = 3;
    {
        char dg[40];
        bt_diag_text(dg);
        ok("the dim line: the audio's longest gap and the late halves (the device test's numbers)",
           str_eq(dg, "AUDIO GAP 12.1MS LATE 3"));
    }
    audio_gap_page = 0;
    audio_late = 0;

    /* closing */
    pg = bt_glo_tap(PG_OUTPUT);
    ok("GLO again: closed, GLOBAL, the rest of the screen drawn again", pg == PG_GLOBAL && !bt_page_shown() &&
       bt_page_restore() && !bt_page_restore());
    ok("closed: no page scan", bt_dv.page == 0u);
    bt_glo_tap(PG_OUTPUT);
    bt_page_close();                                 /* (app_main: another page) */
    ok("another page closes it too", !bt_page_shown());

    /* AUTOSTART */
    {
        bt_set_t s;
        bt_set_get(&s);
        s.autostart = 1;
        bt_set_put(&s);
        fm1_ms += 2000u;
        bt_store_frame();                            /* (saved: stopped and quiet) */
    }
    bt_fresh(1);                                     /* (the store as saved: AUTOSTART ON) */
    bt_boot_clean = 1;
    bt_page_auto();
    fm1_ms += 2900u;
    bt_page_auto();
    ok("AUTOSTART: not in the first 3 s", bs.starts == 0u);
    fm1_ms += 200u;
    bt_page_auto();
    ok("AUTOSTART ON, a clean boot: BLUETOOTH ON once, 3 s on", btp.on && bs.starts == 1u);
    fm1_ms += 10000u;
    bt_page_auto();
    ok(".. once only", bs.starts == 1u);
    bt_fresh(1);
    bt_boot_clean = 0;
    bt_page_auto();
    fm1_ms += 3100u;
    bt_page_auto();
    ok("AUTOSTART after a crash or a watchdog reset: skipped, said", !btp.on && bs.starts == 0u && toast &&
       str_eq(toast, "BT AUTOSTART SKIPPED"));
    {
        bt_set_t s;
        bt_fresh(0);
        bt_boot_clean = 1;
        bt_set_get(&s);
        bt_page_auto();
        fm1_ms += 5000u;
        bt_page_auto();
        ok("AUTOSTART OFF: Bluetooth stays off", !s.autostart && !btp.on && bs.starts == 0u);
    }

    /* the header's BT, drawn */
    bt_fresh(0);
    btp.on = 1;
    lcd_fill(0, 0, 240, 240, C_PANEL);
    draw_header();
    snprintf(st, sizeof st, "%s/bt-head-struck.ppm", out_dir);
    write_ppm_as(st);
    bh.slot[0].st = BLE_LS_UP;
    draw_header();
    snprintf(st, sizeof st, "%s/bt-head-lit.ppm", out_dir);
    write_ppm_as(st);
    {
        uint32_t x, y, lit = 0;
        for (y = 22; y < 38u; y++)
            for (x = (uint32_t)bt_head_at; x < (uint32_t)bt_head_at + 16u; x++)
                lit += fb[y * 240u + x] != fb[1 * 240u + 239u];
        ok("the header's BT: a tag after the others, drawn", bt_head_at > 54 && bt_head_at + 16 <= 238 && lit > 20u);
    }
    printf(fails ? "bt_page: %d FAILED\n" : "bt_page: all checks passed\n", fails);
    return fails != 0;
}
