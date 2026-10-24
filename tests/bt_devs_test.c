/* SPDX-License-Identifier: GPL-3.0-only
 * BLE MIDI: 2026 DEADACTIVE (Felucca's tests/bt_devs_test.c on OberVave's harness) */
/* The Bluetooth store and the saved devices on the host (firmware/src/bt_store.c, bt_devs.c, bt_set.c; Felucca's
 * docs/superpowers/specs/2026-10-22-bt-devices-design.md 5.2, 5.3): the record in its two sectors (the newer copy,
 * a save into the other, a cut save, a damaged copy, saved only stopped and quiet), OberVave's own settings untouched
 * (their size, SYNC's byte), the list (order, the same address once, the full list, forgetting, a damaged entry), the
 * kind by name, each link's device by the user's pick order with the 30 s grace, this FM-1's address, the radio
 * calibration's copy. The BLE unit is tests/bt_host.h's. */
#include "bt_test.h"

static bt_dev_t mk(uint8_t a0, uint32_t kind, const char *name)
{
    bt_dev_t d;
    memset(&d, 0, sizeof d);
    d.addr[0] = a0;
    d.addr[1] = 0x11;
    d.addr[5] = 0x42;
    d.kind = (uint8_t)kind;
    bt_cpy(d.name, name, sizeof d.name);
    return d;
}
static void seen(uint32_t i, const bt_dev_t *d, uint32_t at_ms)   /* the scan list's entry i: d, seen at at_ms */
{
    memset(&bs.dev[i], 0, sizeof bs.dev[i]);
    memcpy(bs.dev[i].addr, d->addr, 6);
    bt_cpy(bs.dev[i].name, d->name, sizeof bs.dev[i].name);
    bs.dev[i].seen_ms = at_ms;
    if (bs.ndev <= i)
        bs.ndev = i + 1u;
}
static void link_slot(uint32_t slot, const bt_dev_t *d)   /* the slot up on d (0: down) */
{
    memset(&bh.slot[slot], 0, sizeof bh.slot[slot]);
    if (d) {
        bh.slot[slot].st = BLE_LS_UP;
        memcpy(bh.slot[slot].addr, d->addr, 6);
    }
}
static int idx_of(uint8_t a0)
{
    bt_dev_t d;
    uint32_t i;
    for (i = 0; bt_devs_get(i, &d); i++)
        if (d.addr[0] == a0)
            return (int)i;
    return -1;
}
static void flush(void)                             /* the main loop's store pass, a second after the change */
{
    fm1_ms += 1500u;
    bt_store_frame();
}

int main(void)
{
    bt_dev_t a = mk(0xA1, BT_KIND_CTRL, "SMC-Mixer"), b = mk(0xB2, BT_KIND_CTRL, "Xkey Air 25"),
             c = mk(0xC3, BT_KIND_CTRL, "nanoKEY"), f = mk(0xF1, BT_KIND_FM1, "Felucca FM-1"), d;
    uint8_t around[BT_NDEV];
    uint32_t i, saves;
    int k;

    bt_start();

    /* ---- OberVave's own settings: never grown, the plain build reads them as before */
    ok("settings: OberVave's record is the plain build's size (67 B: sector_load reads by exact size)", sizeof set == 67u);
    ok("store: the Bluetooth record is 300 B, a copy per sector", sizeof(bts_t) == 300u);
    {
        uint8_t before[sizeof set];
        set.sync_int = 0;
        memcpy(before, &set, sizeof set);
        bt_fresh(0);
        bt_sync_set(BT_SYNC_BLE);
        ok("sync: BLE keeps INTERNAL in OberVave's byte (the plain build ignores the clocks), BLE in the record",
           set.sync_int == 1u && bts.sync == BT_SYNC_BLE && bt_sync() == BT_SYNC_BLE);
        bt_sync_set(BT_SYNC_LINK);
        ok("sync: LINK as well", set.sync_int == 1u && bt_sync() == BT_SYNC_LINK);
        bt_sync_set(BT_SYNC_AUTO);
        ok("sync: AUTO back: OberVave's byte as before, nothing else of it touched",
           set.sync_int == 0u && bt_sync() == BT_SYNC_AUTO && !memcmp(before, &set, sizeof set));
        bt_sync_set(BT_SYNC_INT);
        ok("sync: INTERNAL is OberVave's own (the record keeps none)", set.sync_int == 1u && bts.sync == 0u);
        set.sync_int = 0;
    }

    /* ---- the store: two copies, the newer read, a save into the other */
    bt_fresh(0);
    ok("store: an empty flash -> no record, defaults (MODE AUTO, no device)", bt_devs_n() == 0u && !bt_set_slow(0));
    bt_devs_add(&a);
    ok("store: a change waits (saved once stopped and quiet, a second on)", host_bts_saves == 0u && bts_dirty);
    saves = host_bts_saves;
    ctl_seq_run = 1;
    flush();
    ok("store: not while the sequencer runs (the erase stalls the sound)", host_bts_saves == saves);
    ctl_seq_run = 0;
    flush();
    ok("store: stopped -> saved into copy B (A was empty)", host_bts_saves == saves + 1u && host_bts[1].ok &&
       !host_bts[0].ok);
    bt_devs_add(&b);
    flush();
    ok("store: the next change -> copy A (the other)", host_bts[0].ok && host_bts_saves == saves + 2u);
    bt_fresh(1);
    ok("store: a power-on reads the newer copy (A: both devices)", bt_devs_n() == 2u && idx_of(0xB2) == 0);
    host_bts[0].b[40] ^= 0x10u;                     /* copy A damaged (its CRC fails) */
    bt_fresh(1);
    ok("store: the newer copy damaged -> the older one (B: one device)", bt_devs_n() == 1u && idx_of(0xA1) == 0);
    host_bts[1].ok = 0;                             /* B gone too (a save cut before its header) */
    bt_fresh(1);
    ok("store: both copies bad -> defaults", bt_devs_n() == 0u);
    host_bts[0].b[40] ^= 0x10u;
    host_bts[1].ok = 1;
    bt_fresh(1);
    bt_devs_add(&c);
    flush();
    ok("store: a save after a power-on goes to the copy not read (A read -> B written)", host_bts[1].ok);
    bt_fresh(1);
    ok("store: .. and is read back", bt_devs_n() == 3u && idx_of(0xC3) == 0);
    {
        bts_t r;
        memcpy(&r, host_bts[0].b, sizeof r);
        r.ver = 2u;                                 /* a later version's record: not this one's (defaults) */
        r.crc = crc32((const uint8_t *)&r, sizeof r - 4u);
        memcpy(host_bts[0].b, &r, sizeof r);
        memcpy(host_bts[1].b, &r, sizeof r);
        bt_fresh(1);
        ok("store: another version's record -> defaults, not misread", bt_devs_n() == 0u);
    }

    bt_fresh(0);
    ok("load_zero: no record -> no saved device", bt_devs_n() == 0u && !bt_devs_get(0, &d));
    bt_devs_add(&a);
    bt_devs_add(&b);
    bt_devs_add(&c);
    ok("add_order: the last added first", bt_devs_n() == 3u && idx_of(0xC3) == 0 && idx_of(0xB2) == 1 && idx_of(0xA1) == 2);
    bt_devs_add(&a);
    ok("add_order: the same address again moves to the top, once", bt_devs_n() == 3u && idx_of(0xA1) == 0 &&
       idx_of(0xC3) == 1 && idx_of(0xB2) == 2);
    ok("add_order: the name and kind kept", bt_devs_get(0, &d) && str_eq(d.name, "SMC-Mixer") && d.kind == BT_KIND_CTRL);
    bt_dev_rec(1)[7] ^= 0x55u;                     /* entry 1's check byte broken */
    ok("load_bad_check: a damaged entry is skipped, the others kept", bt_devs_n() == 2u && idx_of(0xC3) < 0 &&
       idx_of(0xA1) == 0 && idx_of(0xB2) == 1);

    bt_fresh(0);
    for (i = 0; i < 8u; i++) {
        bt_dev_t x = mk((uint8_t)(0x10u + i), BT_KIND_CTRL, "CTRL");
        bt_devs_add(&x);
    }
    {
        bt_dev_t x = mk(0x20, BT_KIND_CTRL, "NINTH");
        bt_devs_add(&x);
    }
    ok("evict: a ninth controller -> the least recently picked controller goes", bt_devs_n() == 8u &&
       idx_of(0x20) == 0 && idx_of(0x10) < 0 && idx_of(0x11) == 7);
    bt_fresh(0);
    bt_devs_add(&f);
    for (i = 0; i < 7u; i++) {
        bt_dev_t x = mk((uint8_t)(0x10u + i), BT_KIND_CTRL, "CTRL");
        bt_devs_add(&x);
    }
    {
        bt_dev_t g = mk(0xF2, BT_KIND_FM1, "Felucca FM-1");
        bt_devs_add(&g);
    }
    ok("evict: full, a new FM-1 -> the old FM-1 goes, the controllers stay", bt_devs_n() == 8u && idx_of(0xF2) == 0 &&
       idx_of(0xF1) < 0 && idx_of(0x10) >= 0);

    bt_fresh(0);
    bt_devs_add(&a);
    bt_devs_add(&b);
    bt_devs_add(&c);
    bt_devs_forget(1);                              /* b */
    ok("forget: the rest close up", bt_devs_n() == 2u && idx_of(0xC3) == 0 && idx_of(0xA1) == 1 && idx_of(0xB2) < 0);
    ok("forget: the row it left is cleared", !bt_dev_rec(2)[0] && !bt_dev_rec(2)[7]);
    bt_devs_use(1);                                 /* a */
    ok("use: to the top", idx_of(0xA1) == 0 && idx_of(0xC3) == 1);

    ok("kind: Felucca FM-1 is an FM-1", bt_dev_kind("Felucca FM-1") == BT_KIND_FM1);
    ok("kind: anything else is a controller", bt_dev_kind("SMC-Mixer") == BT_KIND_CTRL && bt_dev_kind("") == BT_KIND_CTRL &&
       bt_dev_kind("Felucca") == BT_KIND_CTRL);

    bt_fresh(0);
    {
        bt_dev_t x = mk(0x77, BT_KIND_CTRL, "W-0123456789-ABCDEFGHIJ");   /* 23 characters */
        bt_devs_add(&x);
        ok("name_cut: a long name kept to 16", bt_devs_get(0, &d) && slen(d.name) == 16u &&
           str_eq(d.name, "W-0123456789-ABC"));
    }

    /* the choice: the user's pick order, a lost device's 30 s grace, a higher one taking the slot back */
    bt_fresh(0);
    bt_devs_add(&b);                                /* order: a (first), b */
    bt_devs_add(&a);
    memset(around, 0, sizeof around);
    around[0] = around[1] = 1;
    ok("choose_order: both around -> the first picked", bt_devs_choose(BT_KIND_CTRL, around, -1, fm1_ms) == 0);
    around[0] = 0;
    ok("choose_order: the first away -> the next", bt_devs_choose(BT_KIND_CTRL, around, -1, fm1_ms) == 1);
    ok("choose_order: an FM-1 is not a controller", bt_devs_choose(BT_KIND_FM1, around, -1, fm1_ms) == -1);

    bt_fresh(0);
    bt_devs_add(&b);
    bt_devs_add(&a);                                /* a first */
    seen(0, &a, fm1_ms);
    seen(1, &b, fm1_ms);
    link_slot(BLE_SLOT_CTRL, &a);
    bt_devs_frame();                                /* a linked */
    link_slot(BLE_SLOT_CTRL, 0);                    /* a drops at t; it is not seen any more, b is */
    {
        uint32_t t = fm1_ms;
        seen(0, &a, t - 60000u);
        seen(1, &b, t);
        bt_devs_frame();
        ok("choose_grace: a dropped -> it stays the target (no stand-in)", !memcmp(bh.tgt[BLE_SLOT_CTRL], a.addr, 6));
        fm1_ms = t + 29000u;
        seen(1, &b, fm1_ms);
        bt_devs_frame();
        ok("choose_grace: 29 s on -> still waiting for it", !memcmp(bh.tgt[BLE_SLOT_CTRL], a.addr, 6));
        fm1_ms = t + 31000u;
        seen(1, &b, fm1_ms);
        bt_devs_frame();
        ok("choose_grace: 31 s on -> the next one around", !memcmp(bh.tgt[BLE_SLOT_CTRL], b.addr, 6));
        link_slot(BLE_SLOT_CTRL, &b);
        fm1_ms = t + 40000u;
        seen(0, &a, fm1_ms);                        /* a back */
        bt_devs_frame();
        ok("choose_grace: the first back -> it takes the slot again", !memcmp(bh.tgt[BLE_SLOT_CTRL], a.addr, 6));
        ok("choose_grace: the order is the user's: a still first", idx_of(0xA1) == 0 && idx_of(0xB2) == 1);
    }

    bt_fresh(0);
    bt_devs_add(&b);
    bt_devs_add(&a);
    seen(0, &a, fm1_ms);
    seen(1, &b, fm1_ms);
    link_slot(BLE_SLOT_CTRL, &a);
    bt_devs_frame();
    bt_devs_forget(0);                              /* the linked one */
    bt_devs_frame();
    ok("forget_connected: the next around at once (no grace)", !memcmp(bh.tgt[BLE_SLOT_CTRL], b.addr, 6));

    bt_fresh(0);
    bt_devs_add(&a);
    bt_devs_add(&f);
    seen(0, &a, fm1_ms);
    seen(1, &f, fm1_ms - 5000u);
    bt_devs_frame();
    ok("targets: the controller's slot -> the controller", !memcmp(bh.tgt[BLE_SLOT_CTRL], a.addr, 6));
    ok("targets: the FM-1's slot -> the FM-1", !memcmp(bh.tgt[BLE_SLOT_FM1], f.addr, 6));
    bt_devs_forget((uint32_t)idx_of(0xF1));
    bt_devs_frame();
    k = !bh.tgt[BLE_SLOT_FM1][0] && !bh.tgt[BLE_SLOT_FM1][1] && !bh.tgt[BLE_SLOT_FM1][5];
    ok("targets: no FM-1 saved -> no target", k);

    bt_fresh(0);
    bt_devs_add(&b);
    bt_devs_add(&a);
    seen(1, &b, fm1_ms);
    link_slot(BLE_SLOT_CTRL, &b);                   /* on the second; the first is not around */
    bt_devs_frame();
    ok("scan_want: linked to a lower one -> a background scan for the first (windowed: 1)", bh.scan_want == 1);
    link_slot(BLE_SLOT_CTRL, &a);
    bt_devs_frame();
    ok("scan_want: on the first, the page closed -> none", bh.scan_want == 0);
    bt_devs_page(1);
    bt_devs_frame();
    ok("scan_want: the page open -> a continuous scan (2)", bh.scan_want == 2);
    bt_devs_page(0);

    bt_fresh(0);
    bt_devs_add(&a);
    bt_devs_add(&b);                                /* b first, a */
    seen(0, &a, fm1_ms - 21000u);
    link_slot(BLE_SLOT_CTRL, &b);
    ok("state: linked -> 2, seen lately -> 1, not for 21 s -> 0",
       bt_devs_state((uint32_t)idx_of(0xB2)) == 2 && bt_devs_state((uint32_t)idx_of(0xA1)) == 0);
    seen(0, &a, fm1_ms - 1000u);
    ok("state: seen a second ago -> around", bt_devs_state((uint32_t)idx_of(0xA1)) == 1);

    bt_fresh(0);
    bt_devs_add(&b);
    bt_devs_add(&a);
    bt_devs_add(&f);
    bt_devs_frame();
    ok("targets_when_none_around: no scan sighting -> the first controller is the target (scan for it)",
       !memcmp(bh.tgt[BLE_SLOT_CTRL], a.addr, 6));
    ok("targets_when_none_around: .. and the first FM-1", !memcmp(bh.tgt[BLE_SLOT_FM1], f.addr, 6));
    fm1_ms += 120000u;                              /* long after any grace: still */
    bt_devs_frame();
    ok("targets_when_none_around: minutes later still (reconnect indefinitely)", !memcmp(bh.tgt[BLE_SLOT_CTRL], a.addr, 6));

    /* the FM-1's own Bluetooth address: made once, kept (design 5.5) */
    bt_fresh(0);
    {
        uint8_t m1[6], m2[6];
        bt_mac(m1);
        ok("mac_made_once: none stored -> made and stored with a good check",
           bts.mac[6] == 0xA5u && !memcmp(bts.mac, m1, 6) && bts_dirty);
        host_ticks += 123457u;
        fm1_ms += 999u;
        bt_mac(m2);
        ok("mac_kept: asked again (another moment) -> the same", !memcmp(m1, m2, 6));
        ok("mac_valid: a random static address (the top two bits set at both ends, as ble_start's)",
           (m1[0] & 0xC0u) == 0xC0u && (m1[5] & 0xC0u) == 0xC0u);
        flush();
        bt_fresh(1);
        bt_mac(m2);
        ok("mac_kept: over a power-off", !memcmp(m1, m2, 6));
        bts.mac[7] ^= 1u;                           /* the check broken: a new one */
        bt_mac(m2);
        ok("mac_made_once: a damaged record -> a new address, stored", bts.mac[6] == 0xA5u && !memcmp(bts.mac, m2, 6));
    }

    {   /* rf_keep: the radio calibration's copy (core_rf_keep_*) in the record's 72 bytes */
        uint8_t rec[68], got[68];
        bt_fresh(0);
        ok("rf_keep: none kept in an empty record", !bt_rf_io(got, 0));
        for (i = 0; i < 68u; i++)
            rec[i] = (uint8_t)(i * 7u + 3u);
        (void)bt_rf_io(rec, 1);
        ok("rf_keep: a kept copy reads back as put", bt_rf_io(got, 0) && !memcmp(got, rec, 68) && bts.rf[71] == 0xA5u);
        flush();
        saves = host_bts_saves;
        bt_fresh(1);
        ok("rf_keep: over a power-off", bt_rf_io(got, 0) && !memcmp(got, rec, 68));
        (void)bt_rf_io(rec, 1);
        flush();
        ok("rf_keep: the same record handed over again (each power-on) -> no flash write", host_bts_saves == saves);
    }
    printf(fails ? "bt_devs: %d FAILED\n" : "bt_devs: all checks passed\n", fails);
    return fails != 0;
}
