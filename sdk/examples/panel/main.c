/* Panel explorer: shows the three pots, the raw key-scan bytes and the direct buttons,
 * and lights the LED of whichever button is held. Use it to label the hardware. */
#include "jt4000.h"

static void hex2(uint8_t v, char *o) { o[0] = "0123456789ABCDEF"[v >> 4]; o[1] = "0123456789ABCDEF"[v & 15]; o[2] = 0; }
static void dec4(uint16_t v, char *o) { for (int i = 3; i >= 0; i--) { o[i] = (char)('0' + v % 10); v /= 10; } o[4] = 0; }

int main(void) {
    char s[8];
    uint8_t k[6];
    uint32_t tick = 0;
    jt_init();
    jt_usb_init();                  /* lets `make flash` restart the synth by itself */
    for (;;) {
        jt_poll_escape();
        jt_keys_raw(k);
        uint8_t leds = (uint8_t)(k[0] << 1);                        /* LFO..ARP keys -> their LEDs */
        if (jt_button_osc()) leds |= JT_LED_OSC;
        if (jt_button_settings()) leds |= JT_LED_SETTINGS;
        if (!leds) leds = (uint8_t)(1u << ((tick / 3) % 6));        /* idle: chase */
        jt_leds_raw(leds, (tick & 16) ? 0 : (uint8_t)(1u << ((tick / 32) % 8)));
        jt_oled_clear();
        for (int i = 0; i < JT_POTS; i++) { dec4(jt_pot(i), s); jt_oled_text(i * 40, 0, s); }
        jt_oled_text(0, 8, "key");
        for (int i = 0; i < 6; i++) { hex2(k[i], s); jt_oled_text(32 + i * 16, 8, s); }
        jt_oled_text(0, 16, jt_button_osc() ? "OSC" : "osc");
        jt_oled_text(32, 16, jt_button_settings() ? "SET" : "set");
        hex2((uint8_t)(1u << ((tick / 32) % 8)), s); jt_oled_text(72, 16, "row6"); jt_oled_text(108, 16, s);
        jt_oled_text(0, 24, "enc");
        int32_t e = jt_encoder();
        jt_oled_text(32, 24, e < 0 ? "-" : "+");
        dec4((uint16_t)((e < 0 ? -e : e) % 10000), s); jt_oled_text(40, 24, s);
        uint16_t kb = jt_keyboard();
        jt_oled_text(80, 24, "kb");
        hex2((uint8_t)(kb >> 8), s); jt_oled_text(96, 24, s);
        hex2((uint8_t)kb, s); jt_oled_text(112, 24, s);
        jt_oled_flush();
        tick++;
    }
}
