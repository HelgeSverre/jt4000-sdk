/* The starting point for a new firmware (`make new APP=name` copies this folder):
 * text on the display, the two direct buttons, and the two ways back to update mode
 * (OSC + SETTINGS, or `make flash` over USB). */
#include "jt4000.h"

static void u32_to_str(uint32_t v, char *out) {
    char tmp[11]; int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *out++ = tmp[--n];
    *out = 0;
}

int main(void) {
    char num[12];
    uint32_t count = 0;
    jt_init();
    jt_usb_init();                  /* lets `make flash` restart the synth by itself */
    for (;;) {
        jt_poll_escape();
        jt_oled_clear();
        jt_oled_text(0, 0, "JT-4000 SDK");
        jt_oled_text(0, 8, "hello, world");
        u32_to_str(count++, num);
        jt_oled_text(0, 16, "count");
        jt_oled_text(48, 16, num);
        jt_oled_text(0, 24, jt_button_osc() ? "OSC:on " : "OSC:off");
        jt_oled_text(64, 24, jt_button_settings() ? "SET:on " : "SET:off");
        jt_oled_flush();
        jt_delay_ms(100);
    }
}
