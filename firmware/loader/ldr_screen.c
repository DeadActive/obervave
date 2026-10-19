/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 illuminus5 (Skiff: the update loader's screen)
 * Copyright (C) 2026 zvenson (OberVave: its name and colour, the letters V X Y) */
/* The update loader's screen (loader.c; tests/ldr_screen_test.c), from Skiff's: a unit whose install was cut off comes
 * back to the loader at every power-on, and showed a black screen. Now UPDATE MODE and what it waits for, then WRITING
 * and a bar while the app area is written, UPDATE FAILED when a session stops. Drawn with the loader's own 5 x 7
 * letters (the app area may be half written: never read), before USB starts (USB is polled: the panel's ~0.3 s init
 * would leave the host's enumeration unanswered). FM-1 only (the SMK-37's loader stays dark: its panel untested here).
 * LDR_NAME / LDR_TITLE: the firmware's name and colour (RGB565), a line of at most 20 letters at size 2. */
#ifndef LDR_NAME
#define LDR_NAME "OBERVAVE"
#endif
#ifndef LDR_TITLE
#define LDR_TITLE 0x3C1Du                /* OberVave's blue, #3C82EB */
#endif
static const char LDR_CH[] = "ABCDEFGHIKLMNOPRSTUVWXY";         /* the letters the screens use; others: a space */
static const uint8_t LDR_FONT[sizeof LDR_CH - 1u][7] = {         /* rows, bit 4 the left dot */
    {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},   /* A B */
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}, {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E},   /* C D */
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},   /* E F */
    {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}, {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},   /* G H */
    {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}, {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11},   /* I K */
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}, {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11},   /* L M */
    {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}, {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},   /* N O */
    {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}, {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},   /* P R */
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}, {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},   /* S T */
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04},   /* U V */
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}, {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11},   /* W X */
    {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04},                                               /* Y */
};
#define LDR_INK 0xFFFFu
#define LDR_BAR LDR_TITLE
#define LDR_DIM 0x2104u                  /* the bar's track */
#define LDR_BAR_X 20u
#define LDR_BAR_Y 190u
#define LDR_BAR_W 200u

static uint32_t ldr_missing;                        /* letters drawn as gaps: 0 (ldr_screen_test) */
static void ldr_text(uint32_t y, uint32_t s, uint16_t c, const char *t)   /* centred, a dot s x s px */
{
    uint32_t n = 0, x, r, k, i, b;
    while (t[n])
        n++;
    if (!n)
        return;
    for (x = (240u - (n * 6u - 1u) * s) / 2u; *t; t++, x += 6u * s) {
        for (k = 0; LDR_CH[k] && LDR_CH[k] != *t; k++)
            ;
        if (!LDR_CH[k]) {
            ldr_missing += *t != ' ';                   /* (a letter the font lacks: a gap) */
            continue;
        }
        for (r = 0; r < 7u; r++)
            for (i = 0; i < 5u; i = b) {                /* each run of dots in the row: one fill */
                for (; i < 5u && !(LDR_FONT[k][r] >> (4u - i) & 1u); i++)
                    ;
                for (b = i; b < 5u && (LDR_FONT[k][r] >> (4u - b) & 1u); b++)
                    ;
                if (b > i)
                    lcd_fill(x + i * s, y + r * s, (b - i) * s, s, c);
            }
    }
}
static void ldr_lines(const char *a, const char *b, const char *c)   /* the three lines under the title */
{
    lcd_fill(0, 106, 240, 66, 0);
    ldr_text(110, 2, LDR_INK, a);
    ldr_text(130, 2, LDR_INK, b);
    ldr_text(150, 2, LDR_INK, c);
}
static void ldr_screen(void)
{
    lcd_init();                                     /* (the panel cleared, on) */
    ldr_text(70, 3, LDR_TITLE, "UPDATE MODE");
    ldr_lines(LDR_NAME, "WAITING FOR INSTALL", "KEEP USB CONNECTED");   /* (an install on its way, or one cut off:
                                                                         * words for both, no alarm) */
    lcd_sync();
}
static uint32_t ldr_bar_w;                          /* the bar's px drawn */
static uint8_t ldr_writing;                         /* WRITING is on the screen */
static void ldr_bar(uint32_t done, uint32_t total)
{
    uint32_t w = done * LDR_BAR_W / total;          /* (the app area: below 2^32 / 200) */
    if (!ldr_writing || w < ldr_bar_w) {            /* a write starts (again) */
        ldr_lines("WRITING", LDR_NAME, "DO NOT SWITCH OFF");
        lcd_fill(LDR_BAR_X, LDR_BAR_Y, LDR_BAR_W, 8, LDR_DIM);
        ldr_bar_w = 0;
        ldr_writing = 1;
    }
    if (w > ldr_bar_w) {
        lcd_fill(LDR_BAR_X + ldr_bar_w, LDR_BAR_Y, w - ldr_bar_w, 8, LDR_BAR);
        ldr_bar_w = w;
    }
    lcd_sync();
}
static void ldr_failed(void)                        /* a session that wrote nothing or stopped: the way out */
{
    ldr_lines("UPDATE FAILED", "INSTALL AGAIN", "KEEP USB CONNECTED");
    lcd_fill(LDR_BAR_X, LDR_BAR_Y, LDR_BAR_W, 8, 0);
    lcd_sync();
    ldr_writing = 0;
}
