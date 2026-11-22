/* Speak: a talking typewriter. Pick words with the SELECT knob, touch a key to add the
 * word at that note, and play the sentence back. The words are LPC speech, the method of
 * the 1978 Speak & Spell: each 20 ms frame is a gain, a voiced/unvoiced flag and the
 * coefficients of a lattice filter (words.h, made by tools/lpcwords.py). The pitch is not
 * stored, so every word can be spoken or sung at any note.
 *
 * The knob says where, the keys say which note, the buttons do things. The display has
 * two rows, the word to add and the sentence, and SELECT press moves the knob between them.
 *
 *   SELECT press   switch between the word row and the sentence row
 *   SELECT turn    word row: choose a word. Sentence row: move the cursor from word to word.
 *                  Either way the word is spoken when you stop turning.
 *   touch a key    word row: add the word at that note, after the cursor.
 *                  Sentence row: give the word under the cursor that note.
 *                  While a sentence plays: re-pitch the word being spoken.
 *   OSC            next word bank (letters, numbers, words); hold and turn to jump by first letter
 *   LFO            voice settings: each press shows the next one, SELECT turn changes it,
 *                  keys try the word without adding it, SELECT press goes back
 *   FILTER         add a pause after the cursor
 *   AMP            delete the word at the cursor (backspace); hold to clear the sentence
 *   ARP            play / stop
 *   SETTINGS       loop on / off
 *   CUTOFF knob    speed
 *   RESONANCE knob size of the mouth: giant to child
 *   USB MIDI       each note speaks the next word of the sentence at that note
 */
#include "jt4000.h"
#include "jt_dsp.h"
#include "words.h"

#define SENTENCE_MAX 32
#define REST 0xFF
#define KEY_LOW 47                          /* the lowest key is B2 */
#define VOLUME 2.2f

/* The voice: one pulse-and-noise source, shaped by these settings. */
enum { OCTAVE, TONE, BREATH, ROUGH, VIBRATO, BEND, DOUBLE, SETTINGS };
static const struct { const char *name; int8_t min, max; } SETTING[SETTINGS] = {
    {"OCTAVE", -2, 2},      /* whole octaves up or down */
    {"TONE", 0, 10},        /* 0: a bare click per period, the Speak & Spell buzz. 10: round and dull */
    {"BREATH", 0, 10},      /* noise mixed into the voice; 10 is a whisper */
    {"ROUGH", 0, 10},       /* uneven periods: gravel */
    {"VIBRATO", 0, 10},
    {"BEND", -10, 10},      /* pitch falls (-) or rises (+) through each word */
    {"DOUBLE", 0, 10},      /* a second voice, detuned */
};
static volatile int8_t setting[SETTINGS];

#define fsqrt jt_sqrt

/* ---- the speech engine: runs in the audio interrupt ---- */
static struct {
    /* set by the main loop */
    const uint8_t *volatile pending;        /* a word to start, or 0 */
    volatile float hz, ratio, speed;        /* pitch, mouth size (clock ratio), frame stretch */
    /* state */
    const uint8_t *p;
    int frames, frames_left, ticks_left;
    volatile bool active;
    bool voiced, tail;
    float k[LPC_ORDER], dk[LPC_ORDER], g, dg;
    float b[LPC_ORDER + 1], y1;
    float phase, phase2, inc, inc2, amp, carry, lp;
    float pole, pole_gain, breath, rough, lfo;
    float cur, prev, acc;
} E = {.hz = 110.0f, .ratio = 1.0f, .speed = 1.0f};

static volatile uint32_t audio_blocks;      /* 2 ms each: the main loop's clock */

static void next_frame(void) {
    float tk[LPC_ORDER], tg = 0.0f;
    bool v = E.voiced;
    if (E.pending) {
        E.p = E.pending + 1;
        E.frames = E.frames_left = E.pending[0];
        E.pending = 0;
        E.active = true;
        E.tail = false;
    }
    E.ticks_left = (int)((float)LPC_FRAME * E.speed * E.ratio);
    if (E.ticks_left < 16) E.ticks_left = 16;
    for (int i = 0; i < LPC_ORDER; i++) tk[i] = E.k[i];
    if (!E.active) {
        E.dg = 0.0f;
        E.g = 0.0f;
        for (int i = 0; i < LPC_ORDER; i++) E.dk[i] = 0.0f;
        return;
    }
    if (E.frames_left == 0) {               /* one more frame to fade out, then rest */
        if (E.tail) { E.active = false; E.g = 0.0f; E.dg = 0.0f; return; }
        E.tail = true;
    } else {
        uint8_t head = *E.p++;
        int gain = head & 0x7F;
        v = head & 0x80;
        E.frames_left--;
        if (gain) {
            int n = v ? LPC_ORDER : LPC_ORDER_UNVOICED;
            for (int i = 0; i < LPC_ORDER; i++) {
                int8_t c = i < n ? (int8_t)*E.p++ : 0;
                tk[i] = c < 0 ? -LPC_SIN[-c > 127 ? 127 : -c] : LPC_SIN[c];
            }
            tg = LPC_GAIN[gain];
        }
    }
    if (v != E.voiced || E.g < 1e-7f)       /* no gliding between a hiss and a vowel */
        for (int i = 0; i < LPC_ORDER; i++) E.k[i] = tk[i];
    E.voiced = v;
    float per_tick = 1.0f / (float)E.ticks_left;
    for (int i = 0; i < LPC_ORDER; i++) E.dk[i] = (tk[i] - E.k[i]) * per_tick;
    E.dg = (tg - E.g) * per_tick;
}

/* one sample at the speech clock (8 kHz times the mouth-size ratio) */
static float tick(void) {
    if (--E.ticks_left <= 0) next_frame();
    E.g += E.dg;
    for (int i = 0; i < LPC_ORDER; i++) E.k[i] += E.dk[i];

    float e = E.carry;
    E.carry = 0.0f;
    if (E.voiced) {
        E.phase += E.inc;
        if (E.phase >= 1.0f) {              /* a glottal pulse, placed between two samples */
            E.phase -= 1.0f;
            float late = E.phase / E.inc;
            e += E.amp * late;
            E.carry = E.amp * (1.0f - late);
            E.phase += E.rough * jt_rand();                 /* an uneven period */
            if (E.phase < 0.0f) E.phase = 0.0f;
        }
        if (E.inc2 > 0.0f) {                /* the second voice */
            E.phase2 += E.inc2;
            if (E.phase2 >= 1.0f) {
                E.phase2 -= 1.0f;
                float late = E.phase2 / E.inc2;
                e += E.amp * late;
                E.carry += E.amp * (1.0f - late);
            }
        }
        E.lp = e + E.pole * E.lp;           /* TONE: a one-pole low-pass rounds the click */
        e = E.pole_gain * E.lp;
        e += E.breath * (1.732f * jt_rand() - e);
    } else {
        e += 1.732f * jt_rand();
    }

    float s = E.g * e;
    for (int i = LPC_ORDER - 1; i >= 0; i--) {
        s -= E.k[i] * E.b[i];
        E.b[i + 1] = E.b[i] + E.k[i] * s;
    }
    E.b[0] = s;
    E.y1 = s + LPC_PRE * E.y1;              /* undo the encoder's pre-emphasis */
    return E.y1;
}

static void render(int16_t *out, int n) {
    audio_blocks++;
    float ratio = E.ratio;
    float step = ratio * ((float)LPC_RATE / (float)JT_SAMPLE_RATE);
    /* once per block: turn the voice settings into numbers the per-sample code can use */
    float through = E.frames ? 1.0f - (float)E.frames_left / (float)E.frames : 0.0f;
    E.lfo += 5.5f * (float)n / (float)JT_SAMPLE_RATE;
    if (E.lfo >= 1.0f) E.lfo -= 1.0f;
    float semis = 12.0f * (float)setting[OCTAVE]
                + 0.07f * (float)setting[VIBRATO] * jt_sin(E.lfo)
                + 0.7f * (float)setting[BEND] * (through - 0.5f);
    E.inc = E.hz * jt_exp2(semis * (1.0f / 12.0f)) / ((float)LPC_RATE * ratio);
    if (E.inc > 0.45f) E.inc = 0.45f;
    if (E.inc < 0.002f) E.inc = 0.002f;
    bool two = setting[DOUBLE] > 0;
    E.inc2 = two ? E.inc * (1.0f + 0.004f * (float)setting[DOUBLE]) : 0.0f;
    E.amp = fsqrt((two ? 0.5f : 1.0f) / E.inc);     /* keeps the loudness the same at every pitch */
    E.pole = 0.085f * (float)setting[TONE];
    E.pole_gain = fsqrt(1.0f - E.pole * E.pole);
    E.breath = 0.1f * (float)setting[BREATH];
    E.rough = 0.012f * (float)setting[ROUGH];
    for (int i = 0; i < n; i++) {
        E.acc += step;
        if (E.acc >= 1.0f) {
            E.acc -= 1.0f;
            E.prev = E.cur;
            E.cur = tick();
        }
        float y = E.prev + (E.cur - E.prev) * E.acc;
        out[i] = (int16_t)(jt_softclip(y * VOLUME) * 30000.0f);
    }
}

static bool speaking(void) { return E.active || E.pending; }

static void say(int word, int note) {
    E.hz = jt_note_hz((float)note);
    E.pending = &WORD_DATA[WORD_AT[word]];
}

/* ---- the sentence ---- */
typedef struct { uint8_t word, note; } slot_t;
static slot_t sentence[SENTENCE_MAX];
static int length;

static int find(const char *name) {
    for (int w = 0; w < WORD_COUNT; w++) {
        const char *a = WORD_NAMES[w], *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (!*a && !*b) return w;
    }
    return 0;
}

static int cursor = -1;                     /* the word new ones go after; -1 = before the first */

static void insert(int word, int note) {
    if (length >= SENTENCE_MAX) return;
    for (int i = length; i > cursor + 1; i--) sentence[i] = sentence[i - 1];
    sentence[++cursor] = (slot_t){(uint8_t)word, (uint8_t)note};
    length++;
}

static void backspace(void) {
    if (cursor < 0) return;
    for (int i = cursor; i < length - 1; i++) sentence[i] = sentence[i + 1];
    length--;
    cursor--;
}

/* ---- display ---- */
static int text_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void flip(int x0, int y0, int w, int h) {
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            if (x >= 0 && x < JT_OLED_W) jt_oled_pixel(x, y, !jt_oled_get(x, y));
}

/* Top half: one word, large. Bottom half: the sentence, with a mark above each word at
 * the height of its note. One word is highlighted: the one being spoken while playing,
 * else the one under the cursor when the sentence row has the knob (`editing`). While the
 * word row has the knob, a blinking caret shows where the next word will go. */
static void draw(const char *big, int spoken, bool editing) {
    jt_oled_clear();
    int n = text_len(big);
    if (n <= 8) jt_oled_text_big((JT_OLED_W - 16 * n) / 2, 0, big, 2);
    else jt_oled_text((JT_OLED_W - 8 * n) / 2, 4, big);

    int mark = spoken >= 0 ? spoken : cursor;
    int end = 0, focus_a = 0, focus_b = 1;                  /* in characters */
    for (int i = 0; i < length; i++) {
        int w = sentence[i].word == REST ? 1 : text_len(WORD_NAMES[sentence[i].word]);
        if (i == mark) { focus_a = end; focus_b = end + w + 1; }
        end += w + 1;
    }
    int scroll = focus_b > 15 ? focus_b - 15 : 0;           /* keep the marked word in view */
    if (scroll > focus_a) scroll = focus_a;
    int x = 4 - 8 * scroll, caret = 1 - 8 * scroll;         /* the caret before the first word */
    for (int i = 0; i < length; i++) {
        bool rest = sentence[i].word == REST;
        const char *s = rest ? "_" : WORD_NAMES[sentence[i].word];
        int w = 8 * text_len(s);
        if (x + w > 0 && x < JT_OLED_W) {
            char one[2] = {0, 0};
            for (int c = 0; s[c]; c++) {
                one[0] = s[c];
                if (x + 8 * c >= 0 && x + 8 * c <= JT_OLED_W - 8) jt_oled_text(x + 8 * c, 24, one);
            }
            if (!rest) {
                int y = 22 - (sentence[i].note - KEY_LOW) * 5 / (JT_KEYBOARD_KEYS - 1);
                if (y < 17) y = 17;
                if (y > 22) y = 22;
                for (int px = x; px < x + w - 1; px++)
                    if (px >= 0 && px < JT_OLED_W) jt_oled_pixel(px, y, true);
            }
            if (i == spoken || (spoken < 0 && editing && i == cursor)) flip(x - 1, 23, w + 1, 9);
        }
        if (i == cursor) caret = x + w + 3;
        x += w + 8;
    }
    bool show = editing ? cursor < 0 : (audio_blocks & 128) != 0;
    if (spoken < 0 && show)
        for (int px = caret; px < caret + 2; px++)
            for (int y = 23; y < 32; y++)
                if (px >= 0 && px < JT_OLED_W) jt_oled_pixel(px, y, true);
    jt_oled_flush();
}

/* The voice page: the setting's name, and its value as a bar and a number. */
static void draw_setting(int which) {
    int v = setting[which], lo = SETTING[which].min, hi = SETTING[which].max;
    char num[5];
    int a = v < 0 ? -v : v, i = 0;
    if (v < 0) num[i++] = '-';
    if (a >= 10) num[i++] = (char)('0' + a / 10);
    num[i++] = (char)('0' + a % 10);
    num[i] = 0;
    jt_oled_clear();
    int n = text_len(SETTING[which].name);
    jt_oled_text_big((JT_OLED_W - 16 * n) / 2, 0, SETTING[which].name, 2);
    const int x0 = 2, w = 96;
    for (int x = x0; x <= x0 + w; x++) { jt_oled_pixel(x, 20, true); jt_oled_pixel(x, 30, true); }
    for (int y = 20; y <= 30; y++) { jt_oled_pixel(x0, y, true); jt_oled_pixel(x0 + w, y, true); }
    int zero = x0 + (0 - lo) * w / (hi - lo), at = x0 + (v - lo) * w / (hi - lo);
    int from = zero < at ? zero : at, to = zero < at ? at : zero;
    for (int x = from; x <= to; x++)
        for (int y = 22; y <= 28; y++) jt_oled_pixel(x, y, true);
    jt_oled_text(JT_OLED_W - 8 * i, 22, num);
    jt_oled_flush();
}

/* bit 0 OSC, 1 LFO, 2 FILTER, 3 AMP, 4 ARP, 5 SETTINGS, 6 SELECT pressed.
 * The key chip's readings flicker while a button is held, so a button counts as down
 * after two readings in a row and as up only after five (40 ms): one press, one action. */
static uint8_t buttons(void) {
    static uint8_t state, count[7];
    uint8_t k = jt_keys_raw(0), raw = 0;
    if (jt_button_osc()) raw |= 1;
    if (k & JT_KEY_LFO) raw |= 2;
    if (k & JT_KEY_FILTER) raw |= 4;
    if (k & JT_KEY_AMP) raw |= 8;
    if (k & JT_KEY_ARP) raw |= 16;
    if (jt_button_settings()) raw |= 32;
    if (k & JT_KEY_SELECT) raw |= 64;
    for (int i = 0; i < 7; i++) {
        bool on = raw >> i & 1, was = state >> i & 1;
        if (on == was) count[i] = 0;
        else if (++count[i] >= (on ? 2 : 5)) { state ^= (uint8_t)(1u << i); count[i] = 0; }
    }
    return state;
}

static float knob(int which, float old) {
    int raw = jt_pot(which);
    float v = raw >= 4000 ? 1.0f : (float)raw * (1.0f / 4000.0f);
    return (v - old > 0.01f || old - v > 0.01f) ? v : old;    /* ignore the jitter of a resting knob */
}

int main(void) {
    int bank = 2, at[BANK_COUNT] = {0}, note = KEY_LOW + 2;
    int play = -1, midi_next = 0, quiet = 0, page = -1;    /* page: the voice setting shown, or -1 */
    bool playing = false, loop = false, osc_turned = false, editing = false;
    uint32_t wait_until = 0, preview_at = 0, note_until = 0, amp_since = 0;
    const char *notice = 0;
    uint16_t last_bits = 0, stable = 0, touched = 0;
    uint8_t held, leds = 0xFF, ev[4];
    int32_t enc_last;
    float speed_knob = 0.5f, size_knob = 0.5f;

    jt_init();
    jt_audio_init(render);
    jt_usb_init();
    jt_filter(1000, 0);

    if (bank >= BANK_COUNT) bank = BANK_COUNT - 1;
    static const struct { const char *name; uint8_t note; } HELLO[] = {
        {"HELLO", 52}, {"I", 54}, {"AM", 52}, {"A", 50}, {"SYNTHESIZER", 49}, {0, 0},
        {"RUNNING", 52}, {"CUSTOM", 54}, {"FIRMWARE", 47},
    };
    for (unsigned i = 0; i < sizeof HELLO / sizeof HELLO[0]; i++)
        insert(HELLO[i].name ? find(HELLO[i].name) : REST, HELLO[i].note);
    at[bank] = find("HELLO") - BANKS[bank].first;
    if (at[bank] < 0 || at[bank] >= BANKS[bank].count) at[bank] = 0;
#ifdef START_EDIT                           /* for the simulator: open the sentence row on this word */
    editing = true;
    cursor = START_EDIT;
#else
    playing = true;                         /* say hello at power-on */
#endif

    for (int i = 0; i < 6; i++) { held = buttons(); jt_delay_ms(8); }   /* let the debouncer settle */
    enc_last = jt_encoder();

    for (;;) {
        jt_delay_ms(8);
        uint32_t t = audio_blocks * 2;
        int word = BANKS[bank].first + at[bank];

        /* knobs */
        speed_knob = knob(JT_ADC_CUTOFF, speed_knob);
        size_knob = knob(JT_ADC_RESONANCE, size_knob);
        E.speed = jt_exp2(1.0f - 2.0f * speed_knob);        /* 2 = half speed ... 0.5 = double */
        E.ratio = jt_exp2((size_knob - 0.5f) * 1.2f);       /* 0.66 giant ... 1.5 child */

        /* buttons */
        uint8_t now = buttons(), pressed = (uint8_t)(now & ~held), released = (uint8_t)(held & ~now);
        held = now;
        if (pressed & 1) osc_turned = false;
        if ((released & 1) && !osc_turned && !(now & 32)) {
            bank = (bank + 1) % BANK_COUNT;
            editing = false;
            notice = BANKS[bank].name;
            note_until = t + 700;
            preview_at = t + 700;
        }
        if (pressed & 2) { if (++page >= SETTINGS) page = -1; }
        if (pressed & 4) insert(REST, note);
        if (pressed & 8) { backspace(); amp_since = t; }
        if ((now & 8) && amp_since && t - amp_since > 900) {
            length = 0; cursor = -1; editing = false; amp_since = 0; playing = false; play = -1;
            notice = "CLEAR"; note_until = t + 700;
        }
        if (!(now & 8)) amp_since = 0;
        if (pressed & 16) {
            playing = !playing && length > 0;
            play = -1;
            wait_until = t;
            preview_at = 0;
        }
        if ((pressed & 32) && !(now & 1)) {
            loop = !loop;
            notice = loop ? "LOOP" : "ONCE";
            note_until = t + 700;
        }
        if (pressed & 64) {
            if (page >= 0) page = -1;
            else { editing = !editing; preview_at = editing ? t + 100 : 0; }
        }

        /* SELECT turn: the next word; with OSC held, the next first letter */
        int steps = (jt_encoder() - enc_last) / 8;
        if (steps && page >= 0) {            /* the voice page: SELECT changes the setting */
            enc_last += steps * 8;
            int v = setting[page] + steps;
            if (v < SETTING[page].min) v = SETTING[page].min;
            if (v > SETTING[page].max) v = SETTING[page].max;
            setting[page] = (int8_t)v;
        } else if (steps && editing) {       /* the sentence row: SELECT moves the cursor */
            enc_last += steps * 8;
            cursor += steps;
            if (cursor < -1) cursor = -1;
            if (cursor > length - 1) cursor = length - 1;
            preview_at = t + 250;
        } else if (steps) {
            enc_last += steps * 8;
            int count = BANKS[bank].count, dir = steps > 0 ? 1 : -1;
            if (now & 1) {
                osc_turned = true;
                for (int s = 0; s != steps; s += dir) {
                    char first = WORD_NAMES[BANKS[bank].first + at[bank]][0];
                    for (int tries = 0; tries < count; tries++) {
                        at[bank] = (at[bank] + dir + count) % count;
                        if (WORD_NAMES[BANKS[bank].first + at[bank]][0] != first) break;
                    }
                }
            } else {
                at[bank] = ((at[bank] + steps) % count + count) % count;
            }
            word = BANKS[bank].first + at[bank];
            preview_at = t + 250;
            note_until = 0;
        }
        if (preview_at && t >= preview_at) {
            preview_at = 0;
            if (playing) {}
            else if (!editing || page >= 0) say(word, note);
            else if (cursor >= 0 && sentence[cursor].word != REST) say(sentence[cursor].word, sentence[cursor].note);
        }

        /* keyboard: two agreeing readings to change, a few empty ones to let go */
        uint16_t bits = jt_keyboard();
        if (bits && bits == last_bits) { stable = bits; quiet = 0; }
        else if (!bits && ++quiet >= 3) stable = 0;
        last_bits = bits;
        int key = jt_keyboard_lowest((uint16_t)(stable & ~touched));
        touched = stable;
        if (key >= 0) {
            note = KEY_LOW + key;
            if (playing && play >= 0 && play < length) {    /* sing over the sentence */
                sentence[play].note = (uint8_t)note;
                E.hz = jt_note_hz((float)note);
            } else if (page >= 0) {                         /* the voice page: only try the voice */
                say(word, note);
            } else if (editing) {                           /* the sentence row: a new note for this word */
                if (cursor >= 0 && sentence[cursor].word != REST) {
                    sentence[cursor].note = (uint8_t)note;
                    say(sentence[cursor].word, note);
                }
                preview_at = 0;
            } else {
                insert(word, note);
                say(word, note);
                preview_at = 0;
            }
        }

        /* USB MIDI: every note speaks the next word of the sentence */
        while (jt_midi_get(ev)) {
            if ((ev[0] & 0x0F) != 9 || !ev[3] || !length) continue;
            for (int tries = 0; tries < length && sentence[midi_next % length].word == REST; tries++) midi_next++;
            midi_next %= length;
            if (sentence[midi_next].word != REST) say(sentence[midi_next].word, ev[2]);
            midi_next++;
        }

        /* playback: one word after another */
        if (playing && !speaking() && t >= wait_until) {
            play++;
            if (play >= length) {
                play = -1;
                if (loop && length) wait_until = t + (uint32_t)(260.0f * E.speed);
                else playing = false;
            } else if (sentence[play].word == REST) {
                wait_until = t + (uint32_t)(220.0f * E.speed);
            } else {
                say(sentence[play].word, sentence[play].note);
                wait_until = t + (uint32_t)(40.0f * E.speed);
            }
        }

        uint8_t want = (uint8_t)((playing ? JT_LED_ARP : 0) | (loop ? JT_LED_SETTINGS : 0) |
                                 (page >= 0 ? JT_LED_LFO : 0) |
                                 (E.active && E.g > 0.004f ? JT_LED_OSC : 0));
        if (want != leds) { leds = want; jt_leds(leds); }

        if (page >= 0) draw_setting(page);
        else {
            const char *big = WORD_NAMES[word];
            if (editing) big = cursor < 0 ? "START" : sentence[cursor].word == REST ? "PAUSE" : WORD_NAMES[sentence[cursor].word];
            if (notice && t < note_until) big = notice;
            draw(big, playing ? play : -1, editing);
        }
    }
}
