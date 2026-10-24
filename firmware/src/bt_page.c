/* SPDX-License-Identifier: GPL-3.0-only
 * BLE MIDI: 2026 DEADACTIVE (from Felucca's ble-kit bt_page.c, after Hortator's)
 * FM-Link: 2026 DEADACTIVE */
/* FELUCCA_BLE (obervave.c includes it after bt_devs.c): the Bluetooth page, OberVave's fourth GLO page (GLO tapped:
 * GLOBAL, VOICE, OUTPUT, BLUETOOTH), in OberVave's look (FONT_S on the blue-black panel, the light blue accent, the
 * selection as the PATTERNS view's) with Felucca's rows and behaviour (Felucca's
 * docs/superpowers/specs/2026-10-22-bt-devices-design.md 5.1, mockup A). It takes the screen under the header: a
 * state line (FORGET <name>? in red), a dim line (the last error; else the audio's longest gap between two halves
 * and the halves rendered late since power-on: the device test's numbers, OberVave has no serial console), the rows
 * BLUETOOTH, AUTOSTART, ROLE (HOST, DEVICE), MODE (AUTO, SMC DAW, MIDI, FM-LINK) and RECONNECT (AUTO, FAST, SLOW:
 * bt_set.c bt_set_slow), then (HOST) the saved devices, a line each (a dot connected / a ring, its name, CTRL or
 * FM-1, ON / NEAR / AWAY), three in view, and + ADD A DEVICE, and the key line. On a device line a knob left asks
 * FORGET <name>? (OCT+ yes, OCT- no) and OCT+ uses it now (the first of its kind); + ADD A DEVICE opens the scan list
 * of nearby devices not saved (BLE-MIDI ones first), found in BT_ADD_MS, then kept as they are: OCT+ saves and
 * connects one, OCT- goes back (NOTHING FOUND: OCT+ scans again). ROLE DEVICE: the FM-1 is the BLE-MIDI device a
 * host connects to; no device lines then. PRESETS moves the cursor (it wraps), any of KNOB 1..4 changes the row,
 * OCT+ acts on it; GLO tapped, or any other page, closes it. The keys still play.
 * BLUETOOTH ON is the only call of ble_user_start in the core (gate 3: tools/check_ble_boot.py TRIGGERS has
 * bt_page_on); Bluetooth is off at every power-on and ON is not saved, unless AUTOSTART is ON: then bt_page_auto
 * turns it on once, BT_AUTO_MS after the main loop began, after a clean boot only. AUTOSTART, ROLE, MODE, RECONNECT,
 * the saved devices are saved (bt_store.c). Main loop only. */
#define BT_PAGE_C 1
#define BT_SET_ROWS 5u                              /* BLUETOOTH AUTOSTART ROLE MODE RECONNECT, then (HOST) the
                                                     * devices, ADD */
#define BT_SHOW 6u                                  /* the ADD list's lines (the scan keeps 12) */
#define BT_ADD_MS 8000u                             /* the ADD list's scan, then the list stays (the user: 8 s) */
#define BT_VIS 3u                                   /* device lines in view (the cursor scrolls them) */
#define BT_Y0 46                                    /* under the header */
#define BT_LH 17                                    /* a line: FONT_S (16) and 1 */
#define BT_SY BT_Y0                                 /* the state line */
#define BT_EY (BT_Y0 + BT_LH)                       /* the dim line */
#define BT_RY (BT_Y0 + 2 * BT_LH + 2)               /* the five rows */
#define BT_DY (BT_RY + 5 * BT_LH + 2)               /* the device lines */
#define BT_KY (240 - BT_LH)                         /* the key line */
_Static_assert(BT_DY + 3 * BT_LH <= BT_KY - 2, "the Bluetooth page: the device lines over the key line");
#define BT_SPLIT 142                                /* the two strips the canvas draws (CV_MAX: 120 rows) */
static const char *const BT_MODE_N[4] = {"AUTO", "SMC DAW", "MIDI", "FM-LINK"};
static const char *const BT_RATE_N[3] = {"AUTO", "FAST", "SLOW"};
static uint32_t bt_role(void) { bt_set_t s; bt_set_get(&s); return s.role; }   /* 1: ROLE DEVICE */
static struct {
    uint8_t on;                                     /* BLUETOOTH ON (not saved) */
    uint8_t open, redraw, restore;                  /* the page is shown; all of it to draw; closed: the rest again */
    uint8_t sel, top;                               /* the cursor (a row), the first device line in view */
    uint8_t confirm;                                /* FORGET asked for saved device confirm - 1 (0: none) */
    uint8_t add, asel, an;                          /* the ADD list open, its cursor, its devices */
    uint8_t head;                                   /* the header's BT as drawn (bt_head_icon) */
    uint32_t add_t0;                                /* .. its scan's start (BT_ADD_MS: then it stays) */
    uint32_t sig, t_draw;                           /* what was drawn, when */
    ble_ui_dev_t adev[BT_SHOW];                     /* the ADD list as drawn: a pick takes these */
} btp __attribute__((section(".pool")));            /* (as Felucca's: the RAM region keeps its 4 KB spare) */

/* the header's BT, a tag after the others (obervave.c draw_header, obv_ui.h ui_header; Felucca's top-bar icon, design
 * 5.6): 0 none (Bluetooth off), 1 struck (on, nothing linked: searching, reconnecting, the device role waiting), 2 lit
 * (a link up) */
static uint32_t bt_head_icon(void)
{
    ble_ui_slot_t x;
    uint32_t i, st = ble_ui_state();
    if (!btp.on)
        return 0;
    if (st == BLE_ST_SUBSCRIBED || st == BLE_ST_RECEIVING)
        return 2;
    for (i = 0; i < 2u; i++)
        if (ble_ui_slot(i, &x) && x.st == BLE_LS_UP)
            return 2;
    return 1;
}
static void bt_head_draw(int32_t x, int32_t y)     /* (inside ui_header's canvas) */
{
    uint32_t s = bt_head_icon();
    if (!s)
        return;
    cv_text(x, y, &FONT_S, "BT", s == 2u ? C_AMBER : C_DIMT);
    if (s == 1u)
        cv_line(x - 1, y + 13, x + 16, y + 2, C_CREAM);   /* struck: on, nothing linked */
}

static uint32_t bt_page_rows(void) { return bt_role() ? BT_SET_ROWS : BT_SET_ROWS + bt_devs_n() + 1u; }

static void bt_cpy(char *o, const char *s, uint32_t n)   /* a C string into o[n] (cut) */
{
    uint32_t i;
    for (i = 0; i + 1u < n && s[i]; i++)
        o[i] = s[i];
    o[i] = 0;
}
/* a name as the screen can show it: the font's characters only (a device's name is any bytes) */
static void bt_name(char *o, const char *s, uint32_t n)
{
    uint32_t i;
    for (i = 0; i + 1u < n && s[i]; i++)
        o[i] = s[i] >= 0x20 && s[i] < 0x7F ? s[i] : '?';
    o[i] = 0;
}
static char *bt_hex2(char *o, uint8_t v)
{
    static const char H[] = "0123456789ABCDEF";
    *o++ = H[v >> 4];
    *o++ = H[v & 15u];
    return o;
}
/* a saved device's line text (24 B): its name; an FM-1 (they all say Felucca FM-1) FM-1 and its address' last four
 * hex digits, most significant first */
static void bt_dev_label(const bt_dev_t *d, char *o)
{
    if (d->kind == BT_KIND_FM1) {
        bt_cpy(o, "FM-1 ", 24);
        *bt_hex2(bt_hex2(o + 5, d->addr[1]), d->addr[0]) = 0;
    } else {
        bt_name(o, d->name[0] ? d->name : "?", 24);
    }
}
static const char *bt_dev_value(uint32_t i)        /* a device line's value: ON (linked), NEAR (around), AWAY */
{
    int st = bt_devs_state(i);
    return st == 2 ? "ON" : st ? "NEAR" : "AWAY";
}

static void bt_state_text(char *o)                  /* the state line (40 B) */
{
    uint32_t st = ble_ui_state(), code, n, i;
    const char *e = ble_ui_err(&code);
    char t[24];
    bt_dev_t d;
    if (btp.confirm && bt_devs_get(btp.confirm - 1u, &d)) {
        bt_dev_label(&d, t);
        bt_cpy(o, "FORGET ", 40);
        scat(o, t);
        scat(o, "?");
        return;
    }
    if (st == BLE_ST_FAILED) {
        bt_cpy(o, "FAILED ", 40);
        bt_cpy(o + 7, e, 33);
    } else if (st == BLE_ST_STARTING) {
        bt_cpy(o, "STARTING", 40);
    } else if (!btp.on || st == BLE_ST_OFF) {
        bt_cpy(o, "OFF", 40);
    } else if (st == BLE_ST_ADVERTISING) {
        bt_cpy(o, "WAITING FOR A HOST", 40);
    } else if (st == BLE_ST_SUBSCRIBED || st == BLE_ST_RECEIVING) {   /* the device role: its host */
        bt_cpy(o, "CONNECTED ", 40);
        if (ble_ui_conn_name(t))
            t[23] = 0, bt_name(o + 10, t, 30);
    } else if (btp.add) {                           /* its scan's seconds left, then what it found */
        n = BT_ADD_MS - (fm1_ms - btp.add_t0);
        if ((int32_t)n > 0) {
            bt_cpy(o, "SCANNING ", 40);
            fmt_u(o + 9, (n + 999u) / 1000u);
            scat(o, " S");
        } else {
            bt_cpy(o, btp.an ? "ADD: PICK A DEVICE" : "NOTHING FOUND", 40);
        }
    } else if (!(n = bt_devs_n())) {
        bt_cpy(o, "ON, NO DEVICE SAVED", 40);
    } else {
        for (i = 0, n = 0; i < 2u; i++) {
            ble_ui_slot_t x;
            n += ble_ui_slot(i, &x) && x.st == BLE_LS_UP;
        }
        if (n) {
            bt_cpy(o, "ON, ", 40);
            fmt_u(o + 4, n);
            scat(o, " CONNECTED");
        } else {
            bt_cpy(o, "ON, SEARCHING", 40);
        }
    }
}

/* the dim line (40 B): the last error, else the audio's timing: the longest gap between two audio halves since the
 * page opened (5.8 ms when nothing held the audio interrupt off) and the halves rendered late since power-on */
static void bt_diag_text(char *o)
{
    uint32_t code, g;
    const char *e = ble_ui_state() == BLE_ST_FAILED ? "" : ble_ui_err(&code);
    if (e[0]) {
        bt_cpy(o, e, 40);
        return;
    }
    g = (audio_gap_page / FM1_TICKS_PER_US + 50u) / 100u;   /* 0.1 ms */
    bt_cpy(o, "AUDIO GAP ", 40);
    fmt_u(o + slen(o), g / 10u);
    scat(o, ".");
    fmt_u(o + slen(o), g % 10u);
    scat(o, "MS LATE ");
    fmt_u(o + slen(o), audio_late);
}

/* the ADD list: the BLE unit's scan list without the saved devices, BLE-MIDI devices first, each part by signal, the
 * first BT_SHOW; the cursor kept on the device it was on (the list re-sorts as signals change) */
static int bt_saved(const uint8_t a[6])
{
    bt_dev_t d;
    uint32_t i;
    for (i = 0; bt_devs_get(i, &d); i++)
        if (!memcmp(d.addr, a, 6))
            return 1;
    return 0;
}
static void bt_add_list(void)
{
    ble_ui_dev_t d;
    uint8_t was[6];
    uint32_t i, k, pass, n = ble_ui_devs(), had = btp.asel < btp.an;
    if (fm1_ms - btp.add_t0 >= BT_ADD_MS)
        return;                                     /* (its scan is over: the list stays as it is) */
    if (had)
        memcpy(was, btp.adev[btp.asel].addr, 6);
    btp.an = 0;
    for (pass = 0; pass < 2u; pass++)
        for (i = 0; i < n; i++) {
            if (!ble_ui_dev(i, &d) || (d.midi != 0) != (pass == 0u) || bt_saved(d.addr) || btp.an >= BT_SHOW)
                continue;
            d.name[sizeof d.name - 1u] = 0;
            btp.adev[btp.an++] = d;
        }
    for (k = 0; had && k < btp.an; k++)
        if (!memcmp(btp.adev[k].addr, was, 6))
            btp.asel = (uint8_t)k;
    if (btp.asel >= btp.an)
        btp.asel = btp.an ? (uint8_t)(btp.an - 1u) : 0u;
}
static void bt_add_close(void) { btp.add = 0; }

/* BLUETOOTH ON: links wanted, the role handed over, the stack started if it is not (the user's trigger: the only
 * call of ble_user_start in the core, gate 3); the saved devices are the targets (bt_devs_frame) */
__attribute__((noinline)) static void bt_page_on(void)
{
    if (ble_ui_state() == BLE_ST_FAILED) {          /* the stack cannot stop: only a power cycle starts it again */
        say("POWER OFF AND ON TO RETRY");
        return;
    }
    if (btp.on)
        return;
    btp.on = 1;
    ble_ui_role((int)bt_role());                    /* (applied once the stack is up and idle) */
    ble_ui_want(1);
    if (ble_ui_state() == BLE_ST_OFF)
        ble_user_start();
}

/* AUTOSTART (bt_service, every main-loop pass): once per power-on, BT_AUTO_MS after the main loop first served
 * Bluetooth, BLUETOOTH ON as from the page, when AUTOSTART is ON and the boot was clean (bootguard: no crash, no
 * watchdog hang and no failed boot before this one, bt_boot_clean). So a Bluetooth start that crashes or hangs the
 * FM-1 is not tried again on the next power-on: the way to UBOOT and the updater stays clear. Gate 3
 * (tools/check_ble_boot.py) allows it only from the main loop's service */
#define BT_AUTO_MS 3000u
static struct { uint8_t armed, done; uint32_t t0; } bt_auto;
__attribute__((noinline)) static void bt_page_auto(void)
{
    bt_set_t s;
    if (bt_auto.done)
        return;
    if (!bt_auto.armed) {
        bt_auto.armed = 1;
        bt_auto.t0 = fm1_ms;
        return;
    }
    if (fm1_ms - bt_auto.t0 < BT_AUTO_MS)
        return;
    bt_auto.done = 1;
    bt_set_get(&s);
    if (!s.autostart)
        return;
    if (!bt_boot_clean) {
        say("BT AUTOSTART SKIPPED");                 /* (after a crash, a hang or a failed boot) */
        return;
    }
    bt_page_on();
}

static void bt_page_open(void)                      /* GLO tapped on OUTPUT */
{
    btp.open = btp.redraw = 1;
    btp.sel = btp.top = btp.confirm = 0;
    bt_add_close();
    audio_gap_page = 0;                             /* (the dim line: the longest gap since the page opened) */
    bt_devs_page(!bt_role());                       /* (HOST: scan while the page is open: its ON / NEAR / AWAY) */
}

static void bt_page_close(void)                     /* GLO tapped again, or another page: the page's scan and list end */
{
    if (!btp.open)
        return;
    btp.open = 0;
    btp.confirm = 0;
    bt_add_close();
    bt_devs_page(0);
    btp.restore = 1;                                /* (app_main: the screen under the header drawn again) */
}
static int bt_page_shown(void) { return btp.open; }
static int bt_page_restore(void)                    /* 1 once after a close */
{
    int r = btp.restore;
    btp.restore = 0;
    return r;
}

/* GLO tapped (app_main): GLOBAL, VOICE, OUTPUT, BLUETOOTH, GLOBAL; the page to show */
static uint32_t bt_glo_tap(uint32_t pg)
{
    if (btp.open) {
        bt_page_close();
        return PG_GLOBAL;
    }
    if (pg == PG_OUTPUT) {
        bt_page_open();
        return pg;
    }
    return pg == PG_GLOBAL ? PG_VOICE : pg == PG_VOICE ? PG_OUTPUT : PG_GLOBAL;
}

/* the page's input (app_main, right after the button edges, while the page is open): PRESETS moves the cursor, KNOB
 * 1..4 change the row, OCT- / OCT+ (their bits taken out of b: no octave change meanwhile); the rest of the panel as
 * ever */
__attribute__((noinline)) static uint32_t bt_page_input(uint32_t b)
{
    uint32_t oct = (b >> BTN_OCT_DOWN & 1u) | (b >> BTN_OCT_UP & 1u) << 1, ok = (oct >> 1) & 1u, rows = bt_page_rows();
    uint32_t st = ble_ui_state(), k, n = bt_devs_n();
    int32_t s, e;
    bt_set_t set1;
    if (!btp.open)
        return b;
    b &= ~(3u << BTN_OCT_DOWN);
    e = fm1_enc_take(ENC_PRESETS);
    for (s = 0, k = 0; k < 4u; k++)                 /* KNOB 1..4, any of them */
        s += fm1_enc_take(ENC_KNOB1 + k);
    if (btp.add) {                                  /* the ADD list: PRESETS moves, OCT+ adds, OCT- back */
        if (e && btp.an)
            btp.asel = (uint8_t)((btp.asel + (e > 0 ? 1u : btp.an - 1u)) % btp.an);
        if (oct & 1u) {
            bt_add_close();
        } else if (ok && !btp.an) {
            btp.add_t0 = fm1_ms;                    /* (nothing found: OCT+ scans again) */
        } else if (ok && btp.asel < btp.an) {
            const ble_ui_dev_t *a = &btp.adev[btp.asel];
            bt_dev_t d;
            memset(&d, 0, sizeof d);
            memcpy(d.addr, a->addr, 6);
            d.type = a->type & 1u;
            d.kind = (uint8_t)bt_dev_kind(a->name);
            bt_cpy(d.name, a->name, sizeof d.name);
            bt_devs_add(&d);                        /* (first of its kind: bt_devs_frame connects it) */
            bt_add_close();
            btp.sel = BT_SET_ROWS;                  /* on its line */
        }
        return b;
    }
    if (btp.confirm) {                              /* FORGET <name>?: OCT+ yes, OCT- no; anything else no */
        if (!oct && !s && !e)
            return b;                               /* (called every pass: waits for an answer) */
        k = btp.confirm - 1u;
        btp.confirm = 0;
        if (ok)
            bt_devs_forget(k);
        if (btp.sel >= bt_page_rows())
            btp.sel = (uint8_t)(bt_page_rows() - 1u);
        return b;
    }
    if (e)                                          /* one row per detent, wrapping */
        btp.sel = (uint8_t)((btp.sel + (e > 0 ? 1u : rows - 1u)) % rows);
    switch (btp.sel) {
    case 0:                                         /* BLUETOOTH: a knob right ON, left OFF; OCT+ toggles */
        if (s > 0 || (ok && !btp.on)) {
            bt_page_on();
        } else if ((s < 0 || ok) && btp.on) {
            btp.on = 0;
            ble_ui_want(0);
        }
        break;
    case 1:                                         /* AUTOSTART: a knob right ON, left OFF; OCT+ toggles; saved */
        bt_set_get(&set1);
        k = s > 0 ? 1u : s < 0 ? 0u : ok ? !set1.autostart : set1.autostart;
        if (k != set1.autostart) {
            set1.autostart = (uint8_t)k;
            bt_set_put(&set1);
        }
        break;
    case 2:                                         /* ROLE: a knob right DEVICE, left HOST; OCT+ toggles; saved */
        bt_set_get(&set1);
        k = s > 0 ? 1u : s < 0 ? 0u : ok ? !set1.role : set1.role;
        if (k != set1.role) {
            set1.role = (uint8_t)k;
            set1.link = 0;
            bt_set_put(&set1);
            ble_ui_role((int)k);                    /* (the BLE unit ends its links, a scan or advertising, then switches) */
            bt_devs_page(!k);
            btp.top = 0;
        }
        break;
    case 3:                                         /* MODE, RECONNECT: a knob steps and stops at the ends; OCT+ */
    case 4: {                                       /* wraps; saved */
        uint8_t *v;
        uint32_t m;
        bt_set_get(&set1);
        v = btp.sel == 3u ? &set1.mode : &set1.rate;
        m = btp.sel == 3u ? BT_MODE_LINK : BT_RATE_SLOW;
        k = s > 0 ? (*v < m ? *v + 1u : m) : s < 0 ? (*v ? *v - 1u : 0u) : ok ? (*v < m ? *v + 1u : 0u) : *v;
        if (k != *v) {
            *v = (uint8_t)k;
            bt_set_put(&set1);
        }
        break;
    }
    default:
        k = btp.sel - BT_SET_ROWS;
        if (k == n) {                               /* + ADD A DEVICE */
            if (ok && st == BLE_ST_FAILED)
                say("POWER OFF AND ON TO RETRY");
            else if (ok && !btp.on)
                say("BLUETOOTH IS OFF");
            else if (ok) {
                btp.add = 1;
                btp.asel = btp.an = 0;
                btp.add_t0 = fm1_ms;
            }
        } else if (s < 0) {                         /* a device: a knob left asks FORGET */
            btp.confirm = (uint8_t)(k + 1u);
        } else if (ok) {                            /* OCT+: use it now (around, not linked) */
            int at = bt_devs_state(k);
            if (at == 1)
                bt_devs_use(k);
            else
                say(at ? "CONNECTED" : "NOT AROUND");
        }
        break;
    }
    return b;
}

static uint32_t bt_hash(uint32_t h, const void *p, uint32_t n)
{
    const uint8_t *q = p;
    while (n--)
        h = (h ^ *q++) * 16777619u;
    return h;
}

/* one line at y: its label (and a tag), its value at the right; the cursor's line on the selection's colour (the
 * PATTERNS view's); dot: 2 a connected device's dot, 1 a saved one's ring, 0 none */
static void bt_line(int32_t y, int sel, const char *label, const char *tag, const char *value, int lit, int dot)
{
    char l[24];
    int32_t lx = 8;
    if (sel) {
        cv_rect(2, y - 1, 236, BT_LH, C_RULE);
        cv_rect(2, y - 1, 3, BT_LH, C_RED);
    }
    if (dot) {
        cv_disc(lx + 4, y + 8, 4, dot == 1 ? 1 : 0, dot == 2 ? C_AMBER : C_DIMT);
        lx += 13;
    }
    bt_cpy(l, label, (uint32_t)((tag ? 140 : 236 - (value[0] ? 8 * (int32_t)slen(value) + 8 : 0)) - lx) / 8u + 1u);
    cv_text(lx, y, &FONT_S, l, sel ? C_CREAM : C_DIMT);
    if (tag)
        cv_text(148, y, &FONT_S, tag, C_DIMT);
    if (value[0])
        cv_text(236 - text_w(&FONT_S, value), y, &FONT_S, value, lit ? C_AMBER : sel ? C_CREAM : C_DIMT);
}

/* the page; drawn again when its signature changes (checked every 100 ms): the state and dim lines, the rows, the
 * saved devices' lines and states, the ADD list, the cursor */
static void bt_page_draw(void)
{
    char st[40], dg[40], lb[24];
    uint32_t sig, pass, v, n, rows;
    int32_t y;
    bt_set_t set1;
    bt_dev_t d;
    uint8_t ds[BT_NDEV];
    const char *keys;
    if (!btp.redraw && fm1_ms - btp.t_draw < 100u)
        return;
    btp.t_draw = fm1_ms;
    if (btp.add)
        bt_add_list();
    bt_set_get(&set1);
    n = set1.role ? 0u : bt_devs_n();
    rows = bt_page_rows();
    if (btp.sel >= rows)
        btp.sel = (uint8_t)(rows - 1u);
    if (btp.sel < BT_SET_ROWS)                      /* the device line under the cursor in view */
        btp.top = 0;
    else if (btp.sel - BT_SET_ROWS < btp.top)
        btp.top = (uint8_t)(btp.sel - BT_SET_ROWS);
    else if (btp.sel - BT_SET_ROWS >= btp.top + BT_VIS)
        btp.top = (uint8_t)(btp.sel - BT_SET_ROWS - BT_VIS + 1u);
    for (v = 0; v < n; v++)
        ds[v] = (uint8_t)bt_devs_state(v);
    bt_state_text(st);
    bt_diag_text(dg);
    sig = bt_hash(2166136261u, st, slen(st));
    sig = bt_hash(sig, dg, slen(dg));
    sig = bt_hash(sig, &btp.sel, 6);                /* sel top confirm add asel an */
    sig = bt_hash(sig, btp.adev, sizeof btp.adev);
    sig = bt_hash(sig, &set1, sizeof set1);
    sig = bt_hash(sig, ds, n);
    sig = bt_hash(sig, bts_get()->dev, sizeof bts.dev);
    sig = (sig ^ btp.on) * 16777619u;
    if (!btp.redraw && sig == btp.sig)
        return;
    btp.redraw = 0;
    btp.sig = sig;
    if (btp.add)
        keys = "PRESETS  OCT+ ADD  OCT- BACK";
    else if (btp.confirm)
        keys = "OCT+ FORGET  OCT- KEEP";
    else if (btp.sel >= BT_SET_ROWS && btp.sel - BT_SET_ROWS < n)
        keys = "KNOB LEFT FORGET  OCT+ USE";
    else
        keys = "PRESETS ROW  KNOB  OCT+ OK";
    for (pass = 0; pass < 2u; pass++) {
        int32_t top_y = pass ? BT_SPLIT : BT_Y0;
        cv_begin(240, (uint32_t)(pass ? 240 - BT_SPLIT : BT_SPLIT - BT_Y0), C_PANEL);
        cv_oy = -top_y;                             /* drawn in screen rows */
        cv_text(8, BT_SY, &FONT_S, st, btp.confirm ? C_RED : C_AMBER);
        cv_text(8, BT_EY, &FONT_S, dg, C_DIMT);
        cv_rect(6, BT_RY - 3, 228, 1, C_RULE);
        if (btp.add) {                              /* the ADD list over the rows: a device a line, BLE-MIDI tagged */
            for (v = 0; v < btp.an; v++) {
                char r[8];
                const ble_ui_dev_t *a = &btp.adev[v];
                y = BT_RY + (int32_t)v * BT_LH;
                if (a->name[0])
                    bt_name(lb, a->name, sizeof lb);
                else
                    *bt_hex2(bt_hex2(bt_hex2(lb, a->addr[5]), a->addr[4]), a->addr[3]) = 0;
                r[0] = 0;
                if (a->rssi < 0) {
                    scat(r, "-");
                    fmt_u(r + 1, (uint32_t)-a->rssi);
                } else
                    fmt_u(r, (uint32_t)a->rssi);
                bt_line(y, btp.asel == v, lb, a->midi ? "MIDI" : 0, r, 0, 0);
            }
            if (!btp.an)
                cv_text(8, BT_RY + 2 * BT_LH, &FONT_S, fm1_ms - btp.add_t0 < BT_ADD_MS ? "LOOKING FOR DEVICES..." :
                        "OCT+: SCAN AGAIN", C_DIMT);
        } else {
            bt_line(BT_RY, btp.sel == 0u, "BLUETOOTH", 0, btp.on ? "ON" : "OFF", btp.on, 0);
            bt_line(BT_RY + BT_LH, btp.sel == 1u, "AUTOSTART", 0, set1.autostart ? "ON" : "OFF", 0, 0);
            bt_line(BT_RY + 2 * BT_LH, btp.sel == 2u, "ROLE", 0, set1.role ? "DEVICE" : "HOST", 0, 0);
            bt_line(BT_RY + 3 * BT_LH, btp.sel == 3u, "MODE", 0, BT_MODE_N[set1.mode & 3u], 0, 0);
            bt_line(BT_RY + 4 * BT_LH, btp.sel == 4u, "RECONNECT", 0, BT_RATE_N[set1.rate % 3u], 0, 0);
            if (!set1.role) {                       /* the saved devices and ADD, BT_VIS lines in view */
                uint32_t lines = n + 1u - btp.top < BT_VIS ? n + 1u - btp.top : BT_VIS;
                cv_rect(6, BT_DY - 3, 228, 1, C_RULE);
                for (v = 0; v < lines; v++) {
                    uint32_t k = btp.top + v;
                    int sel = btp.sel == BT_SET_ROWS + k;
                    y = BT_DY + (int32_t)v * BT_LH;
                    if (k < n && bt_devs_get(k, &d)) {
                        bt_dev_label(&d, lb);
                        bt_line(y, sel, lb, d.kind == BT_KIND_FM1 ? "FM-1" : "CTRL", bt_dev_value(k), ds[k] == 2,
                                ds[k] == 2 ? 2 : 1);
                    } else {
                        bt_line(y, sel, "+ ADD A DEVICE", 0, "SCAN", btp.on, 0);
                    }
                }
            }
        }
        cv_rect(6, BT_KY - 3, 228, 1, C_RULE);
        cv_text((240 - text_w(&FONT_S, keys)) / 2, BT_KY, &FONT_S, keys, C_DIMT);
        cv_oy = 0;
        cv_blit(0, (uint32_t)top_y);
    }
}

/* the header's BT changed (bt_service): the header drawn again */
static void bt_head_frame(void)
{
    uint32_t h = bt_head_icon();
    if (h != btp.head) {
        btp.head = (uint8_t)h;
        dirty_hdr = 1;
    }
}

/* the screen while the page is open (app_main): the header as ever, the page under it */
static void bt_page_screen(uint32_t *t_hdr)
{
    if ((dirty_hdr || dirty_tags) && fm1_ms - *t_hdr >= 30u) {
        dirty_hdr = dirty_tags = 0;
        *t_hdr = fm1_ms;
        draw_header();
    }
    dirty_page = dirty_steps = 0;
    bt_page_draw();
}
