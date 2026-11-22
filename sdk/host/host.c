/* The SDK's hardware layer, for a computer instead of the synth.
 *
 * `make host APP=name` compiles a firmware's own source, unchanged, into a shared library
 * with this file in place of the drivers. tools/panel.py loads it, draws the synth, and
 * pulls the sound. The firmware's main() runs on its own thread; time is the audio clock:
 * jt_delay_ms(8) returns when 8 ms of sound have been asked for, so the firmware runs in
 * step with the audio whether that is played live or rendered to a file as fast as it goes.
 *
 * What is not the real thing: the timing inside a block, USB, and the analog filter, which
 * is a digital stand-in here (jt_filter's two numbers drive a four-pole low-pass). Check
 * anything timing- or hardware-sensitive with tools/emu.py, which runs the real ARM binary.
 */
#include <pthread.h>
#include <string.h>
#include <time.h>

#include "jt4000.h"
#include "jt_dsp.h"

int jt_firmware_main(void);                 /* the firmware's main(), renamed by the build */
extern uint8_t jt_oled_fb[JT_OLED_W * JT_OLED_H / 8];

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;        /* the clock and the display */
static pthread_mutex_t irq = PTHREAD_MUTEX_INITIALIZER;         /* stands in for "interrupts off" */
static pthread_cond_t moved = PTHREAD_COND_INITIALIZER;
static uint64_t blocks;                     /* 2 ms each, counted as the host pulls sound */
static bool waiting;                        /* the firmware is asleep in jt_delay_ms */
static bool strict = true;                  /* the sound waits for the firmware to catch up */
static jt_audio_fn render;

static uint8_t front[sizeof jt_oled_fb];
static bool inverted;
static uint32_t flushes;
static uint8_t leds;
static uint16_t filter_cutoff = 1000, filter_resonance;

static volatile uint8_t in_buttons, in_keys;        /* bit 0 OSC, bit 1 SETTINGS; JT_KEY_* bits */
static volatile uint16_t in_keyboard, in_pot[2] = {4095, 0};
static volatile int32_t in_encoder;
static uint8_t midi_q[64][4];
static volatile unsigned midi_r, midi_w;

uint32_t jt_clock_hz = 170000000;
volatile uint8_t jt_usb_update_request;

/* ---- what the firmware calls ---- */
void jt_init(void) {}
void jt_clock_170mhz(void) {}
void jt_clock_hsi16(void) {}
void jt_gpio_init(void) {}
void jt_oled_init(void) {}
void jt_i2c_init(void) {}
void jt_panel_init(void) {}
void jt_adc_init(void) {}
void jt_keyboard_init(void) {}
void jt_encoder_init(void) {}
void jt_usb_init(void) {}
void jt_poll_escape(void) {}

void jt_delay_ms(uint32_t ms) {
    pthread_mutex_lock(&lock);
    uint64_t until = blocks + (ms + 1) / 2;
    if (until == blocks) until++;
    while (blocks < until) {
        waiting = true;
        pthread_cond_broadcast(&moved);
        pthread_cond_wait(&moved, &lock);
    }
    waiting = false;
    pthread_mutex_unlock(&lock);
}

static void stop_here(void) {               /* a reset on the synth: here the firmware just stops */
    for (;;) jt_delay_ms(1000);
}
void jt_reset(void) { stop_here(); }
void jt_enter_update_mode(void) { stop_here(); }

bool jt_button_osc(void) { return in_buttons & 1; }
bool jt_button_settings(void) { return in_buttons & 2; }
uint8_t jt_keys_raw(uint8_t out[6]) {
    if (out) { memset(out, 0, 6); out[0] = in_keys; }
    return in_keys;
}
void jt_leds(uint8_t mask) { leds = mask; }
void jt_leds_raw(uint8_t row4, uint8_t row6) { (void)row6; leds = row4; }

/* The EEPROM: the bytes the SDK's store uses, blank at every start. */
static uint8_t eeprom[0x8000];
static int eeprom_at = -1;
static bool eeprom_blank = true;
bool jt_i2c_write(uint8_t addr7, const uint8_t *data, int n) {
    if (addr7 != 0x50 || n < 2) return true;
    if (eeprom_blank) { memset(eeprom, 0xFF, sizeof eeprom); eeprom_blank = false; }
    eeprom_at = (data[0] << 8 | data[1]) & 0x7FFF;
    for (int i = 2; i < n; i++) eeprom[(eeprom_at & ~63) | ((eeprom_at + i - 2) & 63)] = data[i];     /* writes wrap in the page */
    return true;
}
bool jt_i2c_read(uint8_t addr7, uint8_t *data, int n) {
    if (addr7 != 0x50 || eeprom_at < 0 || eeprom_blank) { memset(data, 0xFF, (size_t)n); return true; }
    for (int i = 0; i < n; i++) data[i] = eeprom[(eeprom_at + i) & 0x7FFF];
    return true;
}

uint16_t jt_pot(int n) {
    if (n == JT_ADC_KEYBOARD) return in_keyboard ? 52 : 4095;
    return n >= 0 && n < 2 ? in_pot[n] : 0;
}
uint16_t jt_keyboard(void) { return in_keyboard; }
int jt_keyboard_lowest(uint16_t bits) {
    for (int k = 0; k < JT_KEYBOARD_KEYS; k++)
        if (bits & JT_KEYBOARD_BIT(k)) return k;
    return -1;
}
int32_t jt_encoder(void) { return in_encoder; }

void jt_audio_init(jt_audio_fn fn) { render = fn; }
void jt_filter(uint16_t cutoff, uint16_t resonance) {
    filter_cutoff = cutoff > 1000 ? 1000 : cutoff;
    filter_resonance = resonance > 1000 ? 1000 : resonance;
}
void jt_irq_off(void) { pthread_mutex_lock(&irq); }
void jt_irq_on(void) { pthread_mutex_unlock(&irq); }

void jt_oled_invert(bool on) { inverted = on; }
void jt_oled_flush(void) {
    pthread_mutex_lock(&lock);
    memcpy(front, jt_oled_fb, sizeof front);
    flushes++;
    pthread_mutex_unlock(&lock);
    /* Sending a frame to the real display takes a few milliseconds. Taking 2 ms here also
     * keeps firmware that draws in a loop without jt_delay_ms in step with the sound. */
    jt_delay_ms(2);
}

bool jt_midi_get(uint8_t ev[4]) {
    if (midi_r == midi_w) return false;
    memcpy(ev, midi_q[midi_r % 64], 4);
    midi_r++;
    return true;
}
void jt_midi_send(uint8_t status, uint8_t d1, uint8_t d2) { (void)status; (void)d1; (void)d2; }
void jt_midi_send_sysex(const uint8_t *msg, int n) { (void)msg; (void)n; }

/* ---- what the window calls ---- */
static void *run(void *arg) {
    (void)arg;
    jt_firmware_main();
    return 0;
}

void jt_host_start(void) {
    pthread_t thread;
    pthread_create(&thread, 0, run, 0);
    pthread_detach(thread);
}

/* strict: sound waits for the firmware to finish its turn (for rendering to a file).
 * Not strict: it waits a millisecond at most (for playing live). */
void jt_host_strict(int on) { strict = on; }

void jt_host_input(int buttons, int keys, int keyboard, int cutoff, int resonance, int encoder) {
    in_buttons = (uint8_t)buttons;
    in_keys = (uint8_t)keys;
    in_keyboard = (uint16_t)keyboard;
    in_pot[0] = (uint16_t)cutoff;
    in_pot[1] = (uint16_t)resonance;
    in_encoder = encoder;
}

void jt_host_midi(int status, int d1, int d2) {
    uint8_t *ev = midi_q[midi_w % 64];
    ev[0] = (uint8_t)(status >> 4);         /* the USB-MIDI event kind is the status' high half */
    ev[1] = (uint8_t)status;
    ev[2] = (uint8_t)d1;
    ev[3] = (uint8_t)d2;
    midi_w++;
}

/* The display as one byte per pixel (0 or 1), 128 x 32. Returns how often it has been redrawn. */
unsigned jt_host_display(uint8_t *pixels) {
    pthread_mutex_lock(&lock);
    for (int y = 0; y < JT_OLED_H; y++)
        for (int x = 0; x < JT_OLED_W; x++)
            pixels[y * JT_OLED_W + x] = (uint8_t)((front[x + (y >> 3) * JT_OLED_W] >> (y & 7) & 1) ^ (inverted ? 1 : 0));
    unsigned n = flushes;
    pthread_mutex_unlock(&lock);
    return n;
}

int jt_host_leds(void) { return leds; }
int jt_host_filter(int which) { return which ? filter_resonance : filter_cutoff; }

/* A stand-in for the analog filter: four poles, resonance fed back around them. */
static float analog(float x) {
    static float z[4], g_now;
    float hz = 30.0f * jt_exp2((float)filter_cutoff * 0.0095f);          /* 30 Hz .. 21 kHz */
    if (hz > 14000.0f) hz = 14000.0f;
    float w = 3.14159f * hz / (float)JT_SAMPLE_RATE, w2 = w * w;
    float g = w * (1.0f + w2 * (0.33333f + w2 * (0.13333f + w2 * 0.05397f)));
    g_now += (g / (1.0f + g) - g_now) * 0.01f;                           /* the control voltage is smoothed */
    float G = g_now, k = 4.0f * (float)filter_resonance * 0.001f;
    float back = (1.0f - G) * (G * G * G * z[0] + G * G * z[1] + G * z[2] + z[3]);
    float u = jt_softclip((x * (1.0f + 0.4f * k) - k * back) / (1.0f + k * G * G * G * G));
    for (int s = 0; s < 4; s++) {
        float v = G * (u - z[s]);
        u = v + z[s];
        z[s] = u + v;
    }
    return u;
}

/* Fill `frames` samples of sound (any number), advancing the firmware's clock as it goes. */
void jt_host_audio(int16_t *out, int frames) {
    static int16_t block[JT_AUDIO_BLOCK];
    static int have, at;
    for (int i = 0; i < frames; i++) {
        if (at >= have) {
            pthread_mutex_lock(&lock);
            if (strict) {
                while (!waiting) pthread_cond_wait(&moved, &lock);
            } else if (!waiting) {
                struct timespec until;
                clock_gettime(CLOCK_REALTIME, &until);
                until.tv_nsec += 1000000;
                if (until.tv_nsec >= 1000000000) { until.tv_nsec -= 1000000000; until.tv_sec++; }
                pthread_cond_timedwait(&moved, &lock, &until);
            }
            pthread_mutex_unlock(&lock);

            memset(block, 0, sizeof block);
            pthread_mutex_lock(&irq);
            if (render) render(block, JT_AUDIO_BLOCK);
            pthread_mutex_unlock(&irq);
            for (int s = 0; s < JT_AUDIO_BLOCK; s++)
                block[s] = (int16_t)(analog((float)block[s] * (1.0f / 32768.0f)) * 32000.0f);
            have = JT_AUDIO_BLOCK;
            at = 0;

            pthread_mutex_lock(&lock);
            blocks++;
            waiting = false;
            pthread_cond_broadcast(&moved);
            pthread_mutex_unlock(&lock);
        }
        out[i] = block[at++];
    }
}
