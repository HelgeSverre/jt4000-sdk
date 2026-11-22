/* Choir: frozen speech as a pad.
 *
 * LPC speech stores a word as a row of 20 ms snapshots of the mouth's shape. A speech
 * synthesizer runs through them in order; this one stops wherever you like. One knob
 * moves through the word, and up to four voices sing the sound found there, at the notes
 * you hold, for as long as you hold them.
 *
 *   keys             sing (up to four notes)
 *   SELECT turn      the word
 *   SELECT press     hold on / off. On: notes keep sounding after you let go. Off: silence;
 *                    on again brings the last chord back
 *   six buttons      six pages; the two knobs are the open page's:
 *     WORD (OSC)         position in the word      size of the mouth: giant .. child
 *     MOTION (LFO)       drift: it wanders         travel: left = backwards, middle = still, right = forwards
 *     FILTER             cutoff                    resonance (the analog filter)
 *     SHAPE (AMP)        attack                    release
 *     CHOIR (ARP)        spread: two singers per   vibrato
 *                        note, detuned
 *     SPACE (SETTINGS)   reverb                    breath: 0 sung .. 1 whispered
 *   USB MIDI notes   sing too
 */
#include "jt4000.h"
#include "jt_dsp.h"
#include "jt_lpc.h"
#include "words.h"

enum { WORD, MOTION, FILTER, SHAPE, CHOIR, SPACE, PAGES };
#define VOICES 4
#define KEY_LOW 47                          /* the lowest key is B2 */
#define MAX_FRAMES 128

static const char *const PAGE_NAMES[PAGES] = {"WORD", "MOTION", "FILTER", "SHAPE", "CHOIR", "SPACE"};
static float knob[PAGES][2] = {{0.3f, 0.5f}, {0.25f, 0.57f}, {0.85f, 0.1f}, {0.45f, 0.55f}, {0.4f, 0.35f}, {0.55f, 0.1f}};

/* ---- shared with the audio interrupt ---- */
static volatile uint32_t audio_blocks;
static struct {
    float k[JT_LPC_ORDER], k_goal[JT_LPC_ORDER];           /* the mouth's shape now, and where it is going */
    float level, level_goal, voiced, voiced_goal;
} tract;
static struct {
    volatile float note;                    /* MIDI note, fractions allowed; 0 = free */
    volatile float goal;                    /* 1 while the note is held */
    float env, lfo, phase[2], inc[2], amp, carry;
    jt_lattice_t filter;
} voice[VOICES];
static volatile float ratio = 1.0f, spread, vibrato, breath, attack = 0.1f, release = 0.02f, reverb_mix;
static volatile int8_t scope[128];
static jt_reverb_t reverb = {.decay = 0.88f, .damp = 0.4f};

/* one sample at the speech clock: every voice sings through the same mouth */
static float tick(void) {
    for (int i = 0; i < JT_LPC_ORDER; i++) tract.k[i] += (tract.k_goal[i] - tract.k[i]) * 0.03f;
    tract.level += (tract.level_goal - tract.level) * 0.03f;
    tract.voiced += (tract.voiced_goal - tract.voiced) * 0.03f;
    float sung = tract.voiced * (1.0f - breath), sum = 0.0f;
    for (int v = 0; v < VOICES; v++) {
        if (voice[v].env < 1e-4f) continue;
        float e = voice[v].carry;
        voice[v].carry = 0.0f;
        for (int s = 0; s < 2; s++) {       /* two singers on the note, a little apart */
            voice[v].phase[s] += voice[v].inc[s];
            if (voice[v].phase[s] >= 1.0f) {
                voice[v].phase[s] -= 1.0f;
                float late = voice[v].phase[s] / voice[v].inc[s];
                e += voice[v].amp * late;
                voice[v].carry += voice[v].amp * (1.0f - late);
            }
        }
        float x = tract.level * (sung * e + (1.0f - sung) * 1.732f * jt_rand());
        sum += jt_lattice_next(&voice[v].filter, tract.k, x) * voice[v].env;
    }
    return sum;
}

static void render(int16_t *out, int n) {
    static float cur, prev, acc, wet_now, wet_prev, half, dc_in, dc_out;
    static int alt, scope_w;
    audio_blocks++;
    float r = ratio, step = r * (8000.0f / (float)JT_SAMPLE_RATE);
    for (int v = 0; v < VOICES; v++) {
        float goal = voice[v].goal;
        voice[v].env += (goal - voice[v].env) * (goal > voice[v].env ? attack : release);
        voice[v].lfo += (4.6f + 0.45f * (float)v) * (float)n / (float)JT_SAMPLE_RATE;
        if (voice[v].lfo >= 1.0f) voice[v].lfo -= 1.0f;
        float hz = jt_note_hz(voice[v].note + 0.5f * vibrato * jt_sin(voice[v].lfo));
        for (int s = 0; s < 2; s++) {
            float inc = hz * (1.0f + (s ? spread : -spread) * 0.012f) / (8000.0f * r);
            voice[v].inc[s] = inc > 0.45f ? 0.45f : inc < 0.002f ? 0.002f : inc;
        }
        voice[v].amp = jt_sqrt(0.5f / voice[v].inc[0]);
    }
    for (int i = 0; i < n; i++) {
        acc += step;
        if (acc >= 1.0f) { acc -= 1.0f; prev = cur; cur = tick(); }
        float raw = prev + (cur - prev) * acc, y;
        dc_out = raw - dc_in + 0.996f * dc_out;         /* glottal pulses all point one way: take the offset out */
        dc_in = raw;
        float x = dc_out * 1.1f;
        if (alt ^= 1) {                     /* reverb on every second sample */
            half = x;
            y = jt_softclip(x + 0.5f * (wet_prev + wet_now));
        } else {
            wet_prev = wet_now;
            wet_now = jt_reverb_next(&reverb, 0.5f * (half + x)) * reverb_mix;
            y = jt_softclip(x + wet_prev);
        }
        out[i] = (int16_t)(y * 30000.0f);
        if ((i & 3) == 0) scope[scope_w++ & 127] = (int8_t)(y * 120.0f);
    }
}

/* ---- the word ---- */
static const uint8_t *word_data;
static int frames;
static float loudest;
static uint8_t line_level[MAX_FRAMES];      /* for the picture: 0..6 per frame, +0x80 if voiced */

static void choose(int w) {
    float k[JT_LPC_ORDER], level;
    bool voiced;
    word_data = &WORD_DATA[WORD_AT[w]];
    frames = jt_lpc_frames(word_data);
    if (frames > MAX_FRAMES) frames = MAX_FRAMES;
    loudest = 1e-6f;
    for (int i = 0; i < frames; i++) {
        jt_lpc_frame(word_data, i, LPC_SIN, LPC_GAIN, k, &level, &voiced);
        if (level > loudest) loudest = level;
    }
    for (int i = 0; i < frames; i++) {
        jt_lpc_frame(word_data, i, LPC_SIN, LPC_GAIN, k, &level, &voiced);
        line_level[i] = (uint8_t)((int)(jt_sqrt(level / loudest) * 6.0f + 0.5f) | (voiced ? 0x80 : 0));
    }
}

/* Aim the mouth at a place in the word, 0..1: a blend of the two snapshots around it. */
static void aim(float where) {
    float a[JT_LPC_ORDER], b[JT_LPC_ORDER], la, lb;
    bool va, vb;
    float f = where * (float)(frames - 1);
    int i = (int)f;
    if (i > frames - 2) i = frames - 2;
    if (i < 0) i = 0;
    float t = f - (float)i;
    if (t > 1.0f) t = 1.0f;
    for (int j = 0; j < JT_LPC_ORDER; j++) a[j] = b[j] = tract.k_goal[j];
    jt_lpc_frame(word_data, i, LPC_SIN, LPC_GAIN, a, &la, &va);
    jt_lpc_frame(word_data, i + 1, LPC_SIN, LPC_GAIN, b, &lb, &vb);
    if (la <= 0.0f) t = 1.0f;               /* next to silence: keep the shape of the sounding side */
    else if (lb <= 0.0f) t = 0.0f;
    for (int j = 0; j < JT_LPC_ORDER; j++) tract.k_goal[j] = a[j] + (b[j] - a[j]) * t;
    float level = la + (lb - la) * (f - (float)i);
    /* halve the word's dynamics, so quiet sounds can be sung too */
    tract.level_goal = level > 0.0f ? jt_sqrt(level * loudest) : 0.0f;
    tract.voiced_goal = (va ? 1.0f : 0.0f) + ((vb ? 1.0f : 0.0f) - (va ? 1.0f : 0.0f)) * t;
}

/* ---- notes ---- */
static void sing(const uint8_t *want, int count) {
    for (int v = 0; v < VOICES; v++) {      /* let go of notes nobody wants any more */
        bool keep = false;
        for (int i = 0; i < count; i++) keep |= voice[v].goal > 0.0f && (uint8_t)voice[v].note == want[i];
        if (!keep) voice[v].goal = 0.0f;
    }
    for (int i = 0; i < count; i++) {
        int best = -1;
        bool have = false;
        for (int v = 0; v < VOICES; v++) {
            if (voice[v].goal > 0.0f && (uint8_t)voice[v].note == want[i]) have = true;
            if (voice[v].goal <= 0.0f && (best < 0 || voice[v].env < voice[best].env)) best = v;
        }
        if (!have && best >= 0) { voice[best].note = (float)want[i]; voice[best].goal = 1.0f; }
    }
}

/* ---- pictures ---- */
static void block(int x, int y, int w, int h) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) jt_oled_pixel(i, j, true);
}

/* The mouth and throat as a tube: each reflection coefficient is the step in width from one
 * section to the next, which is what the numbers meant before they were a filter. */
static void draw_tube(void) {
    float area[JT_LPC_ORDER + 1], most = 1.0f;
    area[0] = 1.0f;
    for (int i = 0; i < JT_LPC_ORDER; i++) {
        float k = tract.k[i];
        area[i + 1] = area[i] * (1.0f - k) / (1.0f + k);
        if (area[i + 1] > most) most = area[i + 1];
    }
    int last = 0;
    for (int i = 0; i <= JT_LPC_ORDER; i++) {
        int r = 1 + (int)(jt_sqrt(area[i] / most) * 5.5f), x = 4 + i * 11;
        for (int px = x; px < x + 11; px++) { jt_oled_pixel(px, 16 - r, true); jt_oled_pixel(px, 16 + r, true); }
        if (i) {
            int lo = r < last ? r : last, hi = r < last ? last : r;
            for (int d = lo; d <= hi; d++) { jt_oled_pixel(x, 16 - d, true); jt_oled_pixel(x, 16 + d, true); }
        }
        last = r;
    }
}

static void draw_filter(void) {
    int cx = 6 + (int)(knob[FILTER][0] * 112.0f), peak = (int)(knob[FILTER][1] * 8.0f), last = 15;
    for (int x = 0; x < JT_OLED_W; x++) {
        int y = 15;
        if (x > cx) y = 15 + (x - cx) * 2;
        else if (cx - x < 8) y = 15 - peak * (8 - (cx - x)) / 8;
        if (y > 22) y = 22;
        if (x) jt_oled_line(x - 1, last, x, y, true);
        last = y;
    }
}

static void draw_shape(void) {
    int a = 4 + (int)(knob[SHAPE][0] * 50.0f), r = 4 + (int)(knob[SHAPE][1] * 50.0f);
    jt_oled_line(2, 22, 2 + a, 10, true);
    jt_oled_line(2 + a, 10, 124 - r, 10, true);
    jt_oled_line(124 - r, 10, 124, 22, true);
}

static void draw_choir(void) {                   /* four mouths, open as wide as each voice is loud */
    for (int v = 0; v < VOICES; v++) {
        int cx = 16 + v * 32, open = (int)(voice[v].env * (0.3f + tract.level / (loudest + 1e-6f)) * 5.0f);
        if (open > 6) open = 6;
        for (int dx = -10; dx <= 10; dx++) {
            int h = open * (100 - dx * dx) / 100;
            jt_oled_pixel(cx + dx, 16 - h, true);
            jt_oled_pixel(cx + dx, 16 + h, true);
        }
        jt_oled_pixel(cx - 6, 9, voice[v].goal > 0.0f);
        jt_oled_pixel(cx + 6, 9, voice[v].goal > 0.0f);
    }
}

static void draw_scope(void) {
    int last = 16;
    for (int x = 0; x < JT_OLED_W; x++) {
        int y = 16 - scope[x] / 16;
        if (y < 9) y = 9;
        if (y > 22) y = 22;
        if (x) jt_oled_line(x - 1, last, x, y, true);
        last = y;
    }
}

/* the word as a line of loudness along the bottom, sung parts solid, hissed parts dotted */
static void draw_word(float where) {
    for (int x = 0; x < JT_OLED_W; x++) {
        uint8_t v = line_level[x * frames / JT_OLED_W];
        int h = v & 0x7F;
        if (v & 0x80) { if (h) block(x, 32 - h, 1, h); }
        else if (h && (x & 1)) block(x, 32 - h, 1, h);
        if (!h) jt_oled_pixel(x, 31, (x & 3) == 0);
    }
    int cx = (int)(where * 127.0f);
    for (int y = 24; y < 32; y++) jt_oled_pixel(cx, y, !jt_oled_get(cx, y));
    block(cx - 1 < 0 ? 0 : cx - 1, 24, 3, 1);
}

/* bit n = the button of page n, bit 6 = SELECT pressed; debounced as in examples/speak */
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

static int find(const char *name) {
    for (int w = 0; w < WORD_COUNT; w++) {
        const char *a = WORD_NAMES[w], *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (!*a && !*b) return w;
    }
    return 0;
}

int main(void) {
    int page = WORD, word = 0, quiet = 0, midi_n = 0;
    int anchor[2], owns[2] = {0, 0};
    bool hold = true;
    uint16_t last_bits = 0, stable = 0, touched = 0;
    uint16_t latched = JT_KEYBOARD_BIT(1) | JT_KEYBOARD_BIT(8) | JT_KEYBOARD_BIT(13);     /* C3, G3, C4 */
    uint16_t last_chord = latched;
    uint8_t held = 0, ev[4], midi[VOICES] = {0}, midi_latched[VOICES] = {0};
    int midi_latched_n = 0;
    uint32_t t_last;
    int32_t enc_last;
    float pos = knob[WORD][0], wander = 0.0f, wander_goal = 0.0f;

    jt_init();
    word = find("ALLELUIA");
    choose(word);
    aim(pos);
    for (int i = 0; i < JT_LPC_ORDER; i++) tract.k[i] = tract.k_goal[i];
    jt_audio_init(render);
    jt_usb_init();

    for (int i = 0; i < 6; i++) { held = buttons(); jt_delay_ms(8); }
    enc_last = jt_encoder();
    anchor[0] = jt_pot(JT_ADC_CUTOFF);
    anchor[1] = jt_pot(JT_ADC_RESONANCE);
    t_last = audio_blocks * 2;
    jt_leds((uint8_t)(1u << page));

    for (;;) {
        jt_delay_ms(8);
        uint32_t t = audio_blocks * 2, dt = t - t_last;
        t_last = t;

        /* buttons: a page each; SELECT press is hold */
        uint8_t now = buttons(), pressed = (uint8_t)(now & ~held);
        held = now;
        if (!((now & 1) && (now & 32)))
            for (int i = 0; i < PAGES; i++)
                if ((pressed & (1u << i)) && i != page) {
                    page = i;
                    jt_leds((uint8_t)(1u << i));
                    anchor[0] = jt_pot(JT_ADC_CUTOFF);
                    anchor[1] = jt_pot(JT_ADC_RESONANCE);
                    owns[0] = owns[1] = 0;
                }
        if (pressed & 64) {                 /* hold off: silence. Hold on again: the last chord comes back */
            hold = !hold;
            if (!hold) { latched = 0; midi_latched_n = 0; }
            else if (!stable && !midi_n) latched = last_chord;
        }

        /* SELECT turn: the word */
        int steps = (jt_encoder() - enc_last) / 8;
        if (steps) {
            enc_last += steps * 8;
            word = ((word + steps) % WORD_COUNT + WORD_COUNT) % WORD_COUNT;
            choose(word);
        }

        /* knobs: each takes over the open page's control once it has been moved */
        for (int i = 0; i < 2; i++) {
            int raw = jt_pot(i == 0 ? JT_ADC_CUTOFF : JT_ADC_RESONANCE), d = raw - anchor[i];
            if (!owns[i] && (d > 90 || d < -90)) owns[i] = 1;
            if (!owns[i]) continue;
            float v = raw >= 4000 ? 1.0f : (float)raw * (1.0f / 4000.0f), old = knob[page][i];
            if (v - old > 0.004f || old - v > 0.004f) {
                knob[page][i] = v;
                if (page == WORD && i == 0) pos = v;        /* the knob puts you exactly there */
            }
        }

        /* where in the word: the knob, plus travel, plus drift */
        float travel = knob[MOTION][1] - 0.5f, speed = 0.0f;
        if (travel > 0.06f || travel < -0.06f) {            /* a word in 30 s .. a word in half a second */
            float a = (travel < 0 ? -travel : travel) - 0.06f;
            speed = (travel < 0 ? -1.0f : 1.0f) * 0.033f * jt_exp2(a * 13.5f);
        }
        pos += speed * (float)dt * 0.001f;
        if (pos > 1.0f) pos -= 1.0f;
        if (pos < 0.0f) pos += 1.0f;
        if (jt_rand() > 0.9f) wander_goal = jt_rand();
        wander += (wander_goal - wander) * 0.02f;
        float where = pos + wander * knob[MOTION][0] * 0.2f;
        where = where < 0.0f ? 0.0f : where > 1.0f ? 1.0f : where;
        aim(where);

        ratio = jt_exp2((knob[WORD][1] - 0.5f) * 1.2f);
        spread = knob[CHOIR][0];
        vibrato = knob[CHOIR][1];
        reverb_mix = knob[SPACE][0] * 1.3f;
        breath = knob[SPACE][1];
        /* envelope: the share of the remaining way covered every 2 ms */
        attack = 0.002f / (0.003f + 1.2f * knob[SHAPE][0] * knob[SHAPE][0]);
        release = 0.002f / (0.01f + 2.5f * knob[SHAPE][1] * knob[SHAPE][1]);
        if (attack > 1.0f) attack = 1.0f;
        if (release > 1.0f) release = 1.0f;
        jt_filter((uint16_t)(knob[FILTER][0] * 1000.0f), (uint16_t)(knob[FILTER][1] * 1000.0f));

        /* keyboard: two agreeing readings to change, a few empty ones to let go */
        uint16_t bits = jt_keyboard();
        if (bits && bits == last_bits) { stable = bits; quiet = 0; }
        else if (!bits && ++quiet >= 3) stable = 0;
        last_bits = bits;
        uint16_t down = (uint16_t)(stable & ~touched);
        if (down && !touched) { latched = 0; midi_latched_n = 0; }     /* a new chord replaces the held one */
        latched |= stable;
        touched = stable;
        if (latched) last_chord = latched;

        while (jt_midi_get(ev)) {
            int kind = ev[0] & 0x0F;
            if (kind == 9 && ev[3]) {
                if (!midi_n) { midi_latched_n = 0; latched = 0; }
                if (midi_n < VOICES) midi[midi_n++] = ev[2];
                if (midi_latched_n < VOICES) midi_latched[midi_latched_n++] = ev[2];
            } else if (kind == 8 || kind == 9) {
                for (int i = 0; i < midi_n; i++)
                    if (midi[i] == ev[2]) { midi[i] = midi[--midi_n]; break; }
            }
        }

        /* the notes to sing: keys, then MIDI, four at most */
        uint8_t want[VOICES];
        int count = 0;
        uint16_t keys = hold ? latched : stable;
        for (int k = 0; k < JT_KEYBOARD_KEYS && count < VOICES; k++)
            if (keys & JT_KEYBOARD_BIT(k)) want[count++] = (uint8_t)(KEY_LOW + k);
        const uint8_t *m = hold ? midi_latched : midi;
        for (int i = 0; i < (hold ? midi_latched_n : midi_n) && count < VOICES; i++) want[count++] = m[i];
        sing(want, count);

        jt_oled_clear();
        jt_oled_text(0, 0, WORD_NAMES[word]);
        int n = 0;
        while (PAGE_NAMES[page][n]) n++;
        jt_oled_text(JT_OLED_W - 8 * n, 0, PAGE_NAMES[page]);
        if (hold) block(JT_OLED_W / 2 - 2, 2, 4, 4);
        switch (page) {
        case FILTER: draw_filter(); break;
        case SHAPE:  draw_shape(); break;
        case CHOIR:  draw_choir(); break;
        case SPACE:  draw_scope(); break;
        default:     draw_tube();
        }
        draw_word(where);
        jt_oled_flush();
    }
}
