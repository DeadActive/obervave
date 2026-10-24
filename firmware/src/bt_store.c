/* SPDX-License-Identifier: GPL-3.0-only
 * BLE MIDI: 2026 DEADACTIVE */
/* FELUCCA_BLE (obervave.c includes it with the other Bluetooth files): what the Bluetooth build keeps over power-off,
 * a 300-byte record of its own in two flash sectors after the pattern slots (platform.c BTS_AT: 0xB9000, 0xBA000;
 * FL_RANGE_OK allows exactly those two in this build), each in the platform's sector format (platform.c sector_save:
 * a header with the length and a CRC-32, written last, so a save cut off leaves that copy invalid). OberVave's own
 * settings (`set`, read back by its exact size) are not touched: the plain build and older OberVaves read them as
 * before and never look at these sectors. Nothing of the stock config store (0xE5000..0xFBFFF, the radio
 * calibration) either.
 *   The record (SLOOP's layout): a magic, its length and version, a sequence number, the Bluetooth page's settings
 * (bt_set.c: MODE, AUTOSTART, ROLE, RECONNECT, in Felucca's 8 bytes), SYNC BLE / LINK (set.sync_int keeps INTERNAL for
 * them: the plain build reads that), this FM-1's Bluetooth address (bt_devs.c bt_mac), the 8 saved devices (24 bytes
 * each), the radio calibration's copy (72 bytes: bt_devs.c bt_rf_io, ble/ble_vm.c), then a CRC-32 of the rest.
 *   Read once, at the first use (bts_get): of the two copies the valid one with the higher sequence number. A change
 * (bts_changed) is saved by the main loop once the sequencer is stopped and the voices quiet (an erase stalls the
 * sound for ~50 ms, as every OberVave save), into the other copy: a power cut during it keeps the last one. Main loop
 * only. */
#define BTS_MAGIC 0x31544253u                       /* "SBT1" */
#define BTS_VER 1u
typedef struct {
    uint32_t magic;
    uint16_t len, ver;                              /* sizeof, BTS_VER */
    uint32_t seq;
    uint8_t set[8];                                 /* bt_set.c */
    uint8_t sync, rsv[3];                           /* SYNC: 0 as set.sync_int says, BT_SYNC_BLE, BT_SYNC_LINK */
    uint8_t mac[8];                                 /* bt_devs.c bt_mac */
    uint8_t dev[8][24];                             /* bt_devs.c, the saved devices */
    uint8_t rf[72];                                 /* bt_devs.c bt_rf_io */
    uint32_t crc;                                   /* crc32 of the bytes before it */
} bts_t;
_Static_assert(sizeof(bts_t) == 300u && sizeof(bts_t) <= SETTINGS_MAX, "the Bluetooth record");
static bts_t bts;
static uint8_t bts_loaded, bts_dirty, bts_copy;    /* read; changed since the last save; the copy it came from */
static uint32_t bts_t_chg;                          /* when it last changed */

#ifndef OBV_HOST
static int bts_io_load(uint32_t c, void *p, uint32_t n) { return sector_load(BTS_AT + c * 0x1000u, p, n); }
static int bts_io_save(uint32_t c, const void *p, uint32_t n)
{
    fm1_wdt_feed();                                 /* (~50 ms the erase) */
    return sector_save(BTS_AT + c * 0x1000u, p, n);
}
#endif

static int bts_valid(const bts_t *r)
{
    return r->magic == BTS_MAGIC && r->len == sizeof *r && r->ver == BTS_VER &&
           r->crc == crc32((const uint8_t *)r, sizeof *r - 4u);
}
static void bts_load(void)                          /* the newer of the two copies' records, else none (all zero) */
{
    static bts_t b;                                 /* (not on the main loop's stack) */
    int va, vb;
    bts_loaded = 1;
    va = !bts_io_load(0, &bts, sizeof bts) && bts_valid(&bts);
    vb = !bts_io_load(1, &b, sizeof b) && bts_valid(&b);
    if (vb && (!va || b.seq > bts.seq)) {
        bts = b;
        bts_copy = 1;
    } else {
        bts_copy = 0;
        if (!va)
            memset(&bts, 0, sizeof bts);
    }
}
static bts_t *bts_get(void)
{
    if (!bts_loaded)
        bts_load();
    return &bts;
}
static void bts_changed(void)                       /* a Bluetooth setting changed: saved once stopped and quiet */
{
    (void)bts_get();
    bts_dirty = 1;
    bts_t_chg = fm1_ms;
}
/* the main loop (bt_service): a change saved into the other copy, 1 s after the last one, stopped and quiet */
static void bt_store_frame(void)
{
    if (!bts_dirty || ctl_seq_run || !engine_quiet() || fm1_ms - bts_t_chg < 1000u)
        return;
    bts_dirty = 0;
    bts.magic = BTS_MAGIC;
    bts.len = (uint16_t)sizeof bts;
    bts.ver = BTS_VER;
    bts.seq++;
    bts.crc = crc32((const uint8_t *)&bts, sizeof bts - 4u);
    if (!bts_io_save(bts_copy ^ 1u, &bts, sizeof bts))
        bts_copy ^= 1u;                             /* (failed: the next change tries the same copy again) */
}
