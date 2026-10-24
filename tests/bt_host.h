/* SPDX-License-Identifier: GPL-3.0-only
 * BLE MIDI: 2026 DEADACTIVE (after Felucca's tests/ble_ui_stub.h) */
/* The Bluetooth build (FELUCCA_BLE=1) on the host (tests/host.c includes it): what the firmware's platform and the
 * BLE unit give the core's Bluetooth files there, as stand-ins a test sets and reads:
 *   the BLE unit's entry points (firmware/src/ble/ble_core.h): the state, the scan list and the two links in `bs` /
 *     `bh`, the calls counted, MIDI and FM-Link out recorded;
 *   the Bluetooth store's two sectors (bt_store.c bts_io_*), in RAM, each save printed;
 *   the platform's audio timing (platform.c audio_*), fm1_ticks (24 ticks a microsecond, from fm1_ms), crc32. */
#undef memset                                        /* (the host libc may have them as macros) */
#undef memcpy
#undef memcmp
#define memset obv_memset                             /* (ble_core.h declares libc.c's, as the firmware has them) */
#define memcpy obv_memcpy
#define memcmp obv_memcmp
#include "../firmware/src/ble/ble_core.h"
#undef memset
#undef memcpy
#undef memcmp
#ifndef FM1_TICKS_PER_US
#define FM1_TICKS_PER_US 24u
#endif
static uint32_t host_ticks;                           /* (a test may move it on by itself) */
static uint32_t fm1_ticks(void) { return fm1_ms * 24000u + host_ticks; }
static volatile uint32_t audio_t0, audio_gap_max, audio_gap_page, audio_late;
static uint32_t crc32(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFu, k;
    while (n--) {
        c ^= *p++;
        for (k = 0; k < 8u; k++)
            c = c & 1u ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    return ~c;
}

/* the Bluetooth store's sectors (platform.c sector_load / sector_save at BTS_AT): the exact length, as the platform's */
static struct { uint8_t ok; uint32_t n; uint8_t b[512]; } host_bts[2];
static uint32_t host_bts_saves;
static int bts_io_load(uint32_t c, void *p, uint32_t n)
{
    if (c > 1u || !host_bts[c].ok || host_bts[c].n != n)
        return -1;
    memcpy(p, host_bts[c].b, n);
    return 0;
}
static int bts_io_save(uint32_t c, const void *p, uint32_t n)
{
    if (c > 1u || n > sizeof host_bts[c].b)
        return -1;
    host_bts[c].ok = 1;
    host_bts[c].n = n;
    memcpy(host_bts[c].b, p, n);
    host_bts_saves++;
    printf("%6u ms  bluetooth store: copy %u saved (%u B)\n", fm1_ms, c, n);
    return 0;
}

/* ---- the BLE unit */
static char conn_name[24];
static int conn_up;
int ble_ui_conn_name(char out[24])                   /* (the BLE unit: SUBSCRIBED / RECEIVING only) */
{
    if (!conn_up)
        return 0;
    memcpy(out, conn_name, 24);
    return 1;
}
static struct {
    uint32_t state, ndev, code;                      /* ble_ui_state, the list's count, the error */
    const char *err;
    ble_ui_dev_t dev[16];                            /* the list in ble_scan_sorted's order (by signal) */
    uint32_t starts, scans, wants, services;         /* calls */
    int want;                                        /* the last value */
} bs;
uint32_t ble_ui_state(void) { return bs.state; }
const char *ble_ui_err(uint32_t *code) { *code = bs.code; return bs.err ? bs.err : ""; }
uint32_t ble_ui_devs(void) { return bs.ndev; }
int ble_ui_dev(uint32_t i, ble_ui_dev_t *out)
{
    if (i >= bs.ndev)
        return 0;
    *out = bs.dev[i];
    return 1;
}
static struct {                                     /* the host's slots as the test sets them; the targets as set */
    ble_ui_slot_t slot[2];
    uint8_t tgt[2][6], tgt_type[2];
    uint32_t targets[2];                             /* ble_ui_slot_target calls per slot */
    int scan_want;
} bh;
void ble_ui_slot_target(uint32_t slot, const uint8_t addr[6], uint32_t type)
{
    if (slot < 2u) {
        bh.targets[slot]++;
        memcpy(bh.tgt[slot], addr, 6);
        bh.tgt_type[slot] = (uint8_t)type;
    }
}
int ble_ui_slot(uint32_t slot, ble_ui_slot_t *out)
{
    if (slot >= 2u)
        return 0;
    *out = bh.slot[slot];
    return 1;
}
void ble_ui_scan_want(int on) { bs.scans += on && !bh.scan_want; bh.scan_want = on; }
void ble_ui_want(int on) { bs.wants++; bs.want = on; }
static int bs_role = -1;                             /* the last ble_ui_role (-1 none) */
void ble_ui_role(int device) { bs_role = device; }
uint32_t ble_ui_role_now(void) { return bs_role > 0; }
static int bs_fml_ready;
int ble_fml_ready(void) { return bs_fml_ready; }
void ble_user_start(void)
{
    bs.starts++;
    if (bs.state == BLE_ST_OFF)
        bs.state = BLE_ST_STARTING;
}
int ble_started(void) { return bs.state != BLE_ST_OFF; }
void ble_service(void) { bs.services++; }
static uint8_t host_ble_out[256][3];                 /* MIDI to the device, as sent (ble_midi_send) */
static uint32_t host_ble_out_n, host_ble_room = 64;
int ble_midi_send(const uint8_t *m, uint32_t n)
{
    if (!host_ble_room)
        return 0;
    host_ble_out[host_ble_out_n % 256u][0] = m[0];
    host_ble_out[host_ble_out_n % 256u][1] = n > 1u ? m[1] : 0;
    host_ble_out[host_ble_out_n % 256u][2] = n > 2u ? m[2] : 0;
    host_ble_out_n++;
    return 1;
}
uint32_t ble_midi_room(void) { return host_ble_room; }
static uint32_t host_fml_out;
static int (*fml_send_hook)(const uint8_t *p, uint32_t len);   /* (a test's link: bt_fml_test) */
int ble_fml_send(const uint8_t *p, uint32_t len)
{
    host_fml_out++;
    return fml_send_hook ? fml_send_hook(p, len) : bs_fml_ready;
}
