/* The boot sequence. Entirely unnecessary.
 *   0.0 s  jump to warp: stars streak past, the lights chase, a chord swells and the
 *          analog filter opens
 *   1.5 s  flash; the logo tears into place
 *   2.2 s  the subtitle types itself
 *   2.9 s  everything dissolves into the menu; the reverb tail carries on
 * Timed from the audio block counter, so it runs the same whatever the display costs. */
#include "jt4000.h"
#include "jt_dsp.h"

extern volatile uint32_t audio_blocks;
void note_on(int key, int velocity);
void note_off(int key);

#define STARS 26
static struct { float x, y, z; int px, py; } star[STARS];

static uint32_t now(void) { return audio_blocks * 2; }

static void new_star(int i, float z) {
    star[i].x = jt_rand();
    star[i].y = jt_rand();
    star[i].z = z;
    star[i].px = -1;
}

static void stars(float speed, bool streaks) {
    for (int i = 0; i < STARS; i++) {
        star[i].z -= speed;
        if (star[i].z < 0.04f) new_star(i, 1.0f);
        int x = 64 + (int)(star[i].x / star[i].z * 40.0f), y = 16 + (int)(star[i].y / star[i].z * 14.0f);
        if (x < 0 || x > 127 || y < 0 || y > 31) { new_star(i, 1.0f); continue; }
        if (streaks && star[i].px >= 0) jt_oled_line(star[i].px, star[i].py, x, y, true);
        else jt_oled_pixel(x, y, true);
        star[i].px = x;
        star[i].py = y;
    }
}

/* Slide one pixel row sideways, losing what falls off the edge. */
static void tear(int y, int shift) {
    if (shift > 0) for (int x = 127; x >= 0; x--) jt_oled_pixel(x, y, jt_oled_get(x - shift, y));
    if (shift < 0) for (int x = 0; x < 128; x++) jt_oled_pixel(x, y, jt_oled_get(x - shift, y));
}

void intro(void) {
    static const uint8_t RISE[4] = {26, 38, 45, 50};            /* D1, D2, A2, D3 */
    static const uint8_t CHORD[4] = {38, 57, 62, 66};           /* D2, A3, D4, F#4 */
    static const char SUB[] = "AMBIENT  SDK 0.1";
    int risen = 0, hit = 0;
    uint32_t t, start = now();

    for (int i = 0; i < STARS; i++) new_star(i, 0.1f + 0.45f * (jt_rand() + 1.0f));

    while ((t = now() - start) < 3500) {
        jt_poll_escape();
        jt_oled_clear();

        if (t < 1500) {                                         /* warp */
            float k = (float)t * (1.0f / 1500.0f);
            stars(0.004f + 0.07f * k * k, t > 300);
            if (risen < 4 && t >= (uint32_t)risen * 330) note_on(RISE[risen++], 90);
            jt_filter((uint16_t)(120.0f + 780.0f * k * k), 350);
            jt_leds((uint8_t)(1u << (t * (1 + t / 250) / 90 % 6)));     /* a chase that speeds up */
        } else {
            if (!hit) {                                         /* impact */
                hit = 1;
                for (int i = 0; i < 4; i++) note_off(RISE[i]);
                for (int i = 0; i < 4; i++) note_on(CHORD[i], 127);
                jt_filter(1000, 0);
                jt_oled_invert(true);
            }
            uint32_t u = t - 1500;
            if (u > 90) jt_oled_invert(false);
            if (u < 1400) stars(0.006f, false);                 /* the stars drift on behind */
            jt_oled_text_big(8, 1, "JT-4000", 2);
            if (u > 700) {                                      /* typed subtitle, with a cursor */
                char line[17];
                unsigned n = (u - 700) / 35;
                if (n > 16) n = 16;
                for (unsigned i = 0; i < 16; i++) line[i] = i < n ? SUB[i] : ' ';
                if (n < 16 && (u / 60 & 1)) line[n] = '_';
                line[16] = 0;
                jt_oled_text(0, 23, line);
            }
            if (u < 600) {                                      /* the logo arrives torn, and settles */
                int amount = (int)(600 - u) / 12;
                for (int y = 0; y < 18; y++)
                    if (jt_rand() > -0.4f) tear(y, (int)(jt_rand() * (float)amount));
            }
            if (u < 1400) {
                int w = (int)(u / 100) % 4;                     /* lights open out from the middle */
                jt_leds((uint8_t)(w == 0 ? 0x0C : w == 1 ? 0x12 : w == 2 ? 0x21 : 0x3F));
            } else {                                            /* dissolve */
                if (hit == 1) { hit = 2; for (int i = 0; i < 4; i++) note_off(CHORD[i]); }
                int gone = (int)(u - 1400) * 4096 / 600;
                for (int i = 0; i < gone; i++) {
                    uint32_t h = (uint32_t)i * 2654435761u;     /* the same scattered order every frame */
                    jt_oled_pixel((int)(h >> 8 & 127), (int)(h >> 20 & 31), false);
                }
                jt_leds(0);
            }
        }
        jt_oled_flush();
    }
    jt_oled_invert(false);
}
