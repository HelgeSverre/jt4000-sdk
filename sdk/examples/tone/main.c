/* A playable monosynth voice: one oscillator through the analog filter.
 *   keyboard           16 keys, B to D; lowest held key sounds
 *   SELECT knob        octave
 *   cutoff, resonance  the analog filter
 *   OSC button         saw / square
 *   USB MIDI           notes in (a held MIDI note wins over the keyboard); keyboard notes out;
 *                      cutoff = CC 74, resonance = CC 71, both directions (the stock numbers).
 *                      A received CC holds until that knob is moved again.
 * Audio is rendered in the interrupt; the main loop only reads controls and draws. */
#include "jt4000.h"

static const uint32_t C4_INC[12] = {      /* phase step per sample for C4..B4 at 32 kHz */
    35114794, 37202828, 39415023, 41758762, 44241868, 46872626,
    49659817, 52612744, 55741260, 59055808, 62567449, 66287903,
};
static const char *const NAME[12] = {"C ", "C#", "D ", "D#", "E ", "F ", "F#", "G ", "G#", "A ", "A#", "B "};

static volatile uint32_t inc = 35114794;
static volatile int gate, square;

static void render(int16_t *out, int n) {
    static uint32_t phase;
    static int32_t level;                              /* 0..32767, about 8 ms fade */
    uint32_t step = inc;
    int32_t target = gate ? 32767 : 0;
    for (int i = 0; i < n; i++) {
        phase += step;
        int32_t wave = square ? ((phase & 0x80000000u) ? 16000 : -16000)
                              : (int32_t)(phase >> 17) - 16384;
        if (level < target) level += 128; else if (level > target) level -= 128;
        if (level < 0) level = 0;
        out[i] = (int16_t)(wave * level >> 15);
    }
}

static void dec4(int v, char *o) {
    for (int i = 3; i >= 0; i--, v /= 10) o[i] = (char)('0' + v % 10);
    o[4] = 0;
}

int main(void) {
    char oct[2] = "3", num[5];
    int osc_was = 0, quiet = 0, midi_note = -1, sent = -1;
    uint8_t ev[4];
    int knob_raw[2] = {-1000, -1000}, value[2] = {0, 0};   /* cutoff, resonance: 0..127 */
    static const uint8_t CC[2] = {74, 71};
    uint16_t last_bits = 0, stable_bits = 0;

    jt_init();
    jt_audio_init(render);
    jt_usb_init();
    int32_t base = jt_encoder();

    for (;;) {
        int octave = 3 + (jt_encoder() - base) / 16;
        if (octave < 1) octave = 1;
        if (octave > 6) octave = 6;

        /* Read the keyboard about 100 times a second. A note starts after two agreeing
         * readings and stops after 40 ms of "nothing held": polled flat out, or trusted
         * reading by reading, the chip gives gaps that retrigger the note. */
        jt_delay_ms(10);
        uint16_t bits = jt_keyboard();
        if (bits == last_bits && bits) { stable_bits = bits; quiet = 0; }
        else if (!bits && ++quiet >= 4) stable_bits = 0;
        last_bits = bits;
        int k = jt_keyboard_lowest(stable_bits);
        int semi = k + 11;                             /* the lowest key is the B below C */
        int own = k >= 0 ? 12 * octave + semi : -1;    /* MIDI note number; C4 = 60 */

        while (jt_midi_get(ev)) {
            int kind = ev[0] & 0x0F;
            if (kind == 9 && ev[3]) midi_note = ev[2];
            else if ((kind == 8 || kind == 9) && ev[2] == midi_note) midi_note = -1;
            else if (kind == 0xB)
                for (int i = 0; i < 2; i++) if (ev[2] == CC[i]) value[i] = ev[3] & 0x7F;
        }
        if (own != sent) {                             /* the keyboard plays out over USB */
            if (sent >= 0) jt_midi_send(0x80, (uint8_t)sent, 0);
            if (own >= 0) jt_midi_send(0x90, (uint8_t)own, 100);
            sent = own;
        }

        int note = midi_note >= 0 ? midi_note : own;
        if (note >= 0) {
            int o = note / 12 - 1;                     /* table is octave 4 */
            uint32_t v = C4_INC[note % 12];
            inc = o >= 4 ? v << (o - 4) : v >> (4 - o);
        }
        gate = note >= 0;

        /* A knob takes over, and sends its CC, once it has moved by more than its noise. */
        for (int i = 0; i < 2; i++) {
            int raw = jt_pot(i == 0 ? JT_ADC_CUTOFF : JT_ADC_RESONANCE);
            int d = raw - knob_raw[i];
            if (d > 40 || d < -40) {
                int first = knob_raw[i] < 0;
                knob_raw[i] = raw;
                int v = raw * 127 / 4000;
                if (v > 127) v = 127;
                if (v != value[i] && !first) jt_midi_send(0xB0, CC[i], (uint8_t)v);
                value[i] = v;
            }
        }
        uint16_t cut = (uint16_t)(value[0] * 1000 / 127);
        uint16_t res = (uint16_t)(value[1] * 1000 / 127);
        int osc = jt_button_osc();
        jt_filter(cut, res);

        if (osc && !osc_was) square = !square;
        osc_was = osc;
        jt_leds((uint8_t)(square ? JT_LED_LFO : JT_LED_OSC));

        jt_oled_clear();                               /* redrawn every pass: live values */
        jt_oled_text(0, 0, square ? "square" : "saw");
        oct[0] = (char)('0' + octave);
        jt_oled_text(64, 0, "oct");
        jt_oled_text(96, 0, oct);
        if (note >= 0) {
            jt_oled_text(0, 8, midi_note >= 0 ? "midi" : "note");
            jt_oled_text(40, 8, NAME[note % 12]);
            oct[0] = (char)('0' + (note / 12 - 1) % 10);
            jt_oled_text(56, 8, oct);
        }
        dec4(cut, num);
        jt_oled_text(0, 16, "cut");
        jt_oled_text(32, 16, num);
        dec4(res, num);
        jt_oled_text(72, 16, "res");
        jt_oled_text(96, 16, num);
        jt_oled_flush();
    }
}
