/* SPDX-License-Identifier: GPL-3.0-only
 * BLE MIDI: 2026 DEADACTIVE */
/* The Bluetooth build's core side on the host, for the tests/bt_*_test.c: OberVave itself (firmware/src/obervave.c,
 * OBV_HOST, FELUCCA_BLE=1) with the BLE unit and the platform as tests/bt_host.h has them, its main renamed (a test
 * calls the functions itself), started as app_main starts it (bt_start: the engine, the settings' defaults, the four
 * tracks' sounds, the mixer, pattern 1) without its loop. Build (tests/run_tests.sh):
 *   cc -DOBV_HOST -DFELUCCA_BLE=1 -Ifirmware/src -Ifirmware/gen -o build/host/bt_x_test tests/bt_x_test.c */
#if !FELUCCA_BLE
#error "build with -DFELUCCA_BLE=1"
#endif
#define main obv_host_main
#include "../firmware/src/obervave.c"
#undef main

static int fails;
static void ok(const char *what, int good)
{
    printf("%s  %s\n", good ? "ok  " : "FAIL", what);
    fails += !good;
}
static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}
static void bt_start(void)                          /* app_main's start, as far as the tests need it */
{
    uint32_t t;
    rnd = 0xC0FFEE42u;
    engine_init();
    cc_map_init();
    memset(&set, 0, sizeof set);
    set.version = SET_VERSION;
    set.tempo = 120;
    for (t = 0; t < NTRK; t++) {
        set.tprog[t] = TPROG_DEF[t];
        set.mix_lvl[t] = 100;
    }
    set.dv_ok = DV_OK; set.m_vol = 100; set.m_dj = 64; set.m_ok = M_OK;
    ctl_tempo = set.tempo;
    for (t = 0; t < NTRK; t++)
        trk_load(t, set.tprog[t]);
    mix_apply();
    pat_init(&pat);
    set_dirty = 0;
}
/* the Bluetooth state as at a power-on with an empty store (or the store as the test left it: keep 1) */
static void bt_fresh(int keep_store)
{
    if (!keep_store)
        memset(host_bts, 0, sizeof host_bts);
    memset(&bts, 0, sizeof bts);
    bts_loaded = bts_dirty = bts_copy = 0;
    memset(&bs, 0, sizeof bs);
    memset(&bh, 0, sizeof bh);
    memset(&bt_dv, 0, sizeof bt_dv);
    memset(&btp, 0, sizeof btp);
    memset(&bt_in, 0, sizeof bt_in);
    bt_rx_w = bt_rx_r = bt_out_w = bt_out_r = 0;
    memset(&bt_auto, 0, sizeof bt_auto);
    memset(&smc, 0, sizeof smc);
    conn_up = 0;
    bs_role = -1;
    bs_fml_ready = 0;
    host_ble_out_n = 0;
    host_ble_room = 64;
    fm1_ms = 100000u;
}
static uint32_t ble_out_find(uint8_t st, uint8_t d1, uint8_t d2)   /* how many sent to the device */
{
    uint32_t i, n = 0;
    for (i = 0; i < host_ble_out_n && i < 256u; i++)
        n += host_ble_out[i][0] == st && host_ble_out[i][1] == d1 && host_ble_out[i][2] == d2;
    return n;
}
