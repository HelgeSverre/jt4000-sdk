/* Button lights and the four scanned buttons: an HT16K33-style chip at I2C 0x70.
 * Start-up bytes and register use are the stock firmware's (0x0800D8CC, 0x0800FC48). */
#include "jt4000.h"

#define CHIP 0x70
static uint8_t ram[17];                             /* [0] = address 0x00, then 16 display bytes */

void jt_panel_init(void) {
    static const uint8_t on[] = {0x21}, display[] = {0x81}, setup[] = {0xF2};
    jt_i2c_init();
    jt_i2c_write(CHIP, on, 1);                      /* oscillator on */
    jt_i2c_write(CHIP, display, 1);                 /* display on, no blink */
    jt_i2c_write(CHIP, setup, 1);                   /* sent by stock; meaning not confirmed */
    jt_leds_raw(0, 0);
}

void jt_leds_raw(uint8_t row4, uint8_t row6) {
    ram[5] = row4;                                  /* display byte 4: the six button lights */
    ram[7] = row6;                                  /* display byte 6: also driven by stock's start-up animation */
    jt_i2c_write(CHIP, ram, sizeof ram);
}

void jt_leds(uint8_t mask) { jt_leds_raw(mask, ram[7]); }

uint8_t jt_keys_raw(uint8_t out[6]) {
    static const uint8_t reg[] = {0x40};
    uint8_t tmp[6];
    if (!out) out = tmp;
    jt_i2c_write(CHIP, reg, 1);
    jt_i2c_read(CHIP, out, 6);
    return out[0];
}
