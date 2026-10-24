/* SPDX-License-Identifier: GPL-3.0-only
 * BLE MIDI: 2026 DEADACTIVE (from Felucca's ble-kit core_ble_api.c, after Hortator's) */
/* FELUCCA_BLE: the core functions the BLE unit calls (firmware/src/ble/ble_core.h), on OberVave's globals. Thin
 * wrappers over the core's static functions and data (obervave.c includes this file last, after the Bluetooth
 * features), so the core's other functions compile as without BLE. */
static uint32_t bt_frames;                  /* main-loop passes (core_ui_frames: the BLE unit's loop period) */
#ifndef OBV_HOST                            /* (the host tests: tests/bt_host.h stands in for the BLE unit) */
#include "fm1_ble_hal.h"                    /* fm1_guard_stack_window, fm1_irq_unmask / _is_masked */
extern uint32_t _pool_start[];
uint32_t core_ms(void) { return fm1_ms; }
void core_wdt_feed(void) { fm1_wdt_feed(); }
void core_reboot(void) { fm1_reboot(); }
void core_audio_stop(void) { fm1_audio_stop(); }
/* IRQs off (the caller). The BT libraries' requests come at priority 2 at most (ble_port.c request_irq lowers any
 * above): under the audio interrupt (3, platform.c fm1_audio_init) and TIMER5 (4). OberVave runs on one core (no
 * second core waits inside the audio interrupt), so the libraries' long interrupts-off stretches can only delay the
 * audio interrupt (platform.c audio_gap_*, audio_late: the Bluetooth page shows them) */
void core_irq_attach(uint32_t n, void (*h)(void), uint32_t prio)
{
    fm1_guard_unlock_top();                     /* the vector table is write-protected after boot */
    fm1_irq_attach(n, h, prio);
    fm1_guard_lock_top();
}
void core_ble_stack_window(void)                /* the BLE tasks' stacks are in .pool, below the main stacks */
{
    fm1_guard_stack_window((uint32_t)(uintptr_t)_pool_start, (uint32_t)(uintptr_t)_sstack_top);
}
void core_irq_mask(uint32_t n) { fm1_irq_mask(n); }
void core_irq_unmask(uint32_t n) { fm1_irq_unmask(n); }
uint32_t core_irq_is_masked(uint32_t n) { return fm1_irq_is_masked(n); }
void core_midi_out(uint32_t pkt) { midi_out_event(pkt); }
int core_flash_read(uint32_t off, void *buf, uint32_t n)    /* plain flash only (0x93000..), read only */
{
    if (!flash_ok || off < 0x93000u || off > 0x100000u || n > 0x100000u - off)
        return -1;
    return st_read(off, buf, n);
}
#if BLE_CONSOLE
uint32_t core_audio_late(void) { return audio_late; }
uint32_t core_audio_gap_us(void)                /* the longest gap between two audio halves since the last call */
{
    uint32_t g = audio_gap_max;
    audio_gap_max = 0;
    return g / FM1_TICKS_PER_US;
}
uint32_t core_ui_frames(void) { return bt_frames; }
uint32_t core_ble_in_drops(void) { return bt_in.drops; }
#endif
int core_ble_slow(void) { return bt_set_slow((int)ctl_seq_run); }
void core_con_puts(const char *s) { (void)s; }   /* (OberVave has no serial console: FELUCCA_CDC 0) */
void core_ble_mac(uint8_t mac[6]) { bt_mac(mac); }
int core_rf_keep_get(uint8_t rec[68]) { return bt_rf_io(rec, 0); }
void core_rf_keep_put(const uint8_t rec[68]) { (void)bt_rf_io((uint8_t *)rec, 1); }
void core_ble_midi_in(uint32_t pkt) { bt_midi_in(pkt); }
void core_fml_rx(const uint8_t *p, uint32_t len, uint32_t t) { bt_fml_rx(p, len, t); }
void core_ble_link(int up)
{
    bt_link(up);                                /* first: smc_link's frame needs the mode */
    smc_link(up);
    say(up ? "BT CONNECTED" : "BT LOST");
}
#endif
/* the main loop's hook (app_main, every pass): the BT stack's slice (at most 5 ms, ble/ble.c), then the core's own
 * Bluetooth work, cheap while nothing changed */
__attribute__((noinline)) static void bt_service(void)
{
    bt_frames++;
    ble_service();
    bt_page_auto();                             /* AUTOSTART: once, a few seconds after a clean boot (bt_page.c) */
    bt_devs_frame();                            /* the saved devices: each link's target, the scan wanted */
    bt_midi_frame();                            /* MIDI mode: what the keys sent, to the device */
    bt_fml_frame();                             /* FM-Link: the role from SYNC, its messages to the other FM-1 */
    smc_frame();                                /* the SMC-Mixer's LEDs and fader positions */
    bt_head_frame();                            /* the header's BT (bt_page.c) */
    bt_store_frame();                           /* the Bluetooth store saved once stopped and quiet (bt_store.c) */
}
