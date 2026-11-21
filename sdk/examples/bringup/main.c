/* Bring-up ladder: run the first STAGE steps of board start-up, then return to update
 * mode. If the bootloader's serial port comes back, every step up to STAGE worked.
 *   make APP=bringup DEFS=-DSTAGE=2 flash */
#include "jt4000.h"

#ifndef STAGE
#define STAGE 0
#endif

int main(void) {
#if STAGE < 0
    /* Reset loop: bootloader -> app -> reset -> ... Holding OSC + SETTINGS at any moment
     * makes the bootloader stop in update mode, which proves this code is running. */
    jt_reset();
#endif
#if STAGE >= 1 && STAGE <= 4
    jt_clock_hsi16();
#endif
#if STAGE >= 2 && STAGE <= 4
    jt_delay_ms(20);
#endif
#if STAGE >= 3 && STAGE <= 4
    jt_gpio_init();
#endif
#if STAGE == 4
    jt_oled_init();
    jt_oled_text(0, 0, "JT-4000 SDK");
    jt_oled_text(0, 12, "bring-up stage 4");
    jt_oled_flush();
    jt_delay_ms(4000);
#endif
#if STAGE == 5        /* clock: drop to 16 MHz, bring the 170 MHz PLL up ourselves, wait 3 s */
    jt_clock_hsi16();
    jt_clock_170mhz();
    jt_delay_ms(3000);
#endif
#if STAGE == 6        /* clock: keep what the bootloader left and assume 170 MHz, wait 3 s */
    jt_clock_170mhz();
    jt_delay_ms(3000);
#endif
    jt_enter_update_mode();
    for (;;) { }
}
