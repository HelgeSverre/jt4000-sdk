/* An ambient sound box. No menus, no words: six buttons, two knobs, pictures.
 *
 * It plays itself (a slow generator picks notes from a scale) and you nudge it. Each
 * button opens a scene; the two knobs then shape that part of the sound, and the display
 * draws it. A knob does nothing after a scene change until you move it.
 *
 *   button     scene    left knob            right knob          picture
 *   OSC        tone     wave shape           detune              the waveform
 *   LFO        motion   speed                depth               an orbiting dot
 *   FILTER     colour   analog cutoff        analog resonance    the filter curve
 *   AMP        shape    attack               release             the envelope
 *   ARP        life     how often it plays   how far it wanders  falling notes, the scale
 *   SETTINGS   space    echo, then shimmer   reverb              ripples
 *
 *   SELECT turn    change the scale (shown as twelve boxes)
 *   SELECT press   hold: notes ring on until pressed again (a square top right)
 *   keyboard       play; the last key becomes the generator's home note
 *   USB MIDI       notes in and out
 *   OSC+SETTINGS   back to update mode
 *
 * Chain: four voices -> digital filter -> echo, grains, reverb (at 16 kHz) -> analog filter. */
#include "jt4000.h"
#include "jt_dsp.h"

#define SR 32000.0f
#define FX_SR 16000.0f
#define CONTROL_SR (SR / JT_AUDIO_BLOCK)           /* 500 Hz: once per audio block */
#define VOICES 4
#define TAPE 8192                                   /* 512 ms at 16 kHz */

enum { TONE, MOTION, COLOUR, SHAPE, LIFE, SPACE, SCENES };
#ifndef START_SCENE                                 /* make DEFS=-DSTART_SCENE=4 opens another scene first */
#define START_SCENE TONE
#endif

/* Every control is 0..1: knob[scene][0] is the left knob, [1] the right. */
static float knob[SCENES][2] = {
    {0.40f, 0.30f}, {0.30f, 0.25f}, {0.80f, 0.15f}, {0.55f, 0.70f}, {0.40f, 0.50f}, {0.50f, 0.70f},
};

static const uint16_t SCALES[] = {                  /* bit n = n semitones above the home note */
    0x4A9,  /* minor pentatonic  0 3 5 7 10 */
    0x295,  /* major pentatonic  0 2 4 7 9 */
    0x6AD,  /* dorian            0 2 3 5 7 9 10 */
    0xAD5,  /* lydian            0 2 4 6 7 9 11 */
    0x5AD,  /* minor             0 2 3 5 7 8 10 */
    0x18D,  /* hirajoshi         0 2 3 7 8 */
    0x555,  /* whole tone        0 2 4 6 8 10 */
    0x081,  /* fifths            0 7 */
};
#define SCALE_COUNT ((int)(sizeof SCALES / sizeof SCALES[0]))
static const uint8_t MORPH[4] = {JT_WAVE_SINE, JT_WAVE_TRIANGLE, JT_WAVE_SAW, JT_WAVE_SQUARE};

/* ---- the sound engine: these are read by the audio interrupt ---- */
typedef struct {
    jt_osc_t a1, b1, a2, b2;                        /* two oscillators, each a pair to blend wave shapes */
    jt_env_t env;
    float note, level;
    uint32_t age;
    int8_t key;                                     /* MIDI note held, -1 when released */
} voice_t;

static voice_t voice[VOICES];
static jt_env_t filter_env;
static jt_lfo_t lfo;
static jt_svf_t filter;
static jt_tape_t tape;
static jt_grains_t grains;
static jt_reverb_t reverb;
static int16_t tape_buf[TAPE];

static struct {
    float blend, ratio2, lfo_pitch, lfo_filter;
    float delay, feedback, delay_mix, grain_mix, reverb_mix;
    float interval, chaos, range;                   /* generator: ms between notes (0 = off), 0..1, octaves */
    uint16_t scale;
    uint8_t home, hold;
} P = {.scale = 0x4A9, .home = 57};
static uint32_t clock;
volatile uint32_t audio_blocks;                     /* a 2 ms clock for the pictures */
static volatile uint32_t note_count;                /* notes started so far, and the last one: for the pictures */
static volatile uint8_t note_last;
static volatile int8_t scope[256];                  /* the most recent output */
static volatile uint8_t scope_w;
void intro(void);

static void apply(void) {
    float w = knob[TONE][0] * 2.999f;
    int shape = (int)w;
    float d = knob[TONE][1], at = knob[SHAPE][0], re = knob[SHAPE][1];
    float release = jt_exp2(4.0f + re * 9.3f);      /* 16 ms .. 10 s */
    for (int i = 0; i < VOICES; i++) {
        voice[i].a1.wave = voice[i].a2.wave = MORPH[shape];
        voice[i].b1.wave = voice[i].b2.wave = MORPH[shape + 1];
        jt_env_set(&voice[i].env, SR, jt_exp2(at * 12.0f), release, 0.7f, release);
    }
    jt_env_set(&filter_env, CONTROL_SR, jt_exp2(at * 12.0f), release, 0.3f, release);
    P.blend = w - (float)shape;
    P.ratio2 = jt_exp2(d * d * 50.0f * (1.0f / 1200.0f));          /* up to 50 cents */
    jt_lfo_hz(&lfo, 0.05f * jt_exp2(knob[MOTION][0] * 8.0f), CONTROL_SR);   /* 0.05 .. 13 Hz */
    P.lfo_filter = knob[MOTION][1] * 30.0f;
    P.lfo_pitch = knob[MOTION][1] * knob[MOTION][1] * 0.6f;

    float density = knob[LIFE][0];
    P.interval = density < 0.03f ? 0.0f : 4000.0f * jt_exp2(-density * 6.0f);   /* 4 s .. 60 ms */
    P.chaos = knob[LIFE][1];
    P.range = 1.0f + 3.0f * knob[LIFE][1];

    float x = knob[SPACE][0], y = knob[SPACE][1];   /* first half: more echo; second half: grains an octave up */
    float echo = x < 0.5f ? x * 2.0f : 1.0f;
    P.delay = FX_SR * (0.18f + 0.30f * x);
    P.feedback = 0.25f + 0.6f * echo;
    P.delay_mix = 0.5f * echo;
    P.grain_mix = x > 0.5f ? (x - 0.5f) * 1.8f : 0.0f;
    grains.pitch = 2.0f;
    grains.size = FX_SR * 0.12f;
    grains.spread = 1500.0f;
    reverb.decay = 0.4f + 0.6f * y;
    reverb.damp = 0.4f;
    P.reverb_mix = y * 1.2f;
}

/* ---- voices: called from the audio interrupt, or with interrupts off ---- */
static void voice_on(int key, int velocity) {
    int best = 0;
    float score, best_score = 1e9f;
    if (key < 0 || key > 127) return;
    for (int i = 0; i < VOICES; i++) {              /* a silent voice, else the quietest released, else the oldest */
        voice_t *x = &voice[i];
        if (x->env.stage == 0) score = -1.0f;
        else if (x->key < 0) score = x->env.level;
        else score = 2.0f + (float)x->age * 1e-6f;
        if (score < best_score) { best_score = score; best = i; }
    }
    voice[best].note = (float)key;
    voice[best].key = (int8_t)key;
    voice[best].level = 0.3f + 0.7f * (float)velocity * (1.0f / 127.0f);
    voice[best].age = ++clock;
    jt_env_gate(&voice[best].env, true);
    note_last = (uint8_t)key;
    note_count++;
}

static void voice_off(int key) {
    for (int i = 0; i < VOICES; i++)
        if (voice[i].key == key) { voice[i].key = -1; jt_env_gate(&voice[i].env, false); }
}

void note_on(int key, int velocity) {               /* from the main loop (and the intro) */
    __asm__ volatile("cpsid i");
    voice_on(key, velocity);
    __asm__ volatile("cpsie i");
}

void note_off(int key) {
    __asm__ volatile("cpsid i");
    voice_off(key);
    __asm__ volatile("cpsie i");
}

/* ---- the generator: now and then, a note from the scale near the home note ---- */
static void generate(void) {
    static float wait = 600.0f;
    static struct { uint8_t key; float left; } sounding[3];
    for (int i = 0; i < 3; i++)
        if (sounding[i].left > 0.0f && (sounding[i].left -= 2.0f) <= 0.0f && !P.hold) voice_off(sounding[i].key);
    if (P.interval <= 0.0f || (wait -= 2.0f) > 0.0f) return;
    wait = P.interval * (1.0f + P.chaos * 0.9f * jt_rand());

    int degree, tries = 0;
    do degree = (int)((jt_rand() + 1.0f) * 5.999f); while (!(P.scale >> degree & 1) && ++tries < 40);
    int octave = (int)((jt_rand() + 1.0f) * 0.5f * P.range);
    int key = P.home - 12 + degree + 12 * octave;
    for (int i = 0; i < 3; i++) {
        if (sounding[i].left > 0.0f) continue;
        sounding[i].key = (uint8_t)key;
        sounding[i].left = 120.0f + 500.0f * (jt_rand() + 1.0f);
        voice_on(key, 50 + (int)(35.0f * (jt_rand() + 1.0f)));
        break;
    }
}

static float fx(float in) {
    static float delay, tone;
    delay += (P.delay - delay) * 0.0004f;           /* time changes glide, like tape speed */
    float d = jt_tape_read(&tape, delay);
    tone += (d - tone) * 0.55f;                     /* each repeat a little duller */
    jt_tape_write(&tape, in + tone * P.feedback);
    float wet = d * P.delay_mix;
    if (P.grain_mix > 0.0f) wet += jt_grains_next(&grains, &tape) * P.grain_mix;
    return wet + jt_reverb_next(&reverb, in + wet) * P.reverb_mix;
}

static void render(int16_t *out, int n) {
    static float wet_now, wet_prev, half;
    static int phase;

    audio_blocks++;
    generate();
    float mod = jt_lfo_next(&lfo), any = 0;
    for (int v = 0; v < VOICES; v++) {
        voice_t *x = &voice[v];
        if (x->key >= 0) any = 1;
        float hz = jt_note_hz(x->note + mod * P.lfo_pitch);
        x->a1.inc = x->b1.inc = hz / SR;
        x->a2.inc = x->b2.inc = hz * P.ratio2 / SR;
    }
    if (any && filter_env.stage != 1 && filter_env.stage != 2) jt_env_gate(&filter_env, true);
    if (!any) jt_env_gate(&filter_env, false);
    float e = jt_env_next(&filter_env);
    jt_svf_set(&filter, jt_note_hz(84.0f + e * 30.0f + mod * P.lfo_filter), 0.15f, SR);

    for (int i = 0; i < n; i++) {
        float s = 0.0f;
        for (int v = 0; v < VOICES; v++) {
            voice_t *x = &voice[v];
            float a = jt_env_next(&x->env);
            if (x->env.stage == 0) continue;
            float wa = jt_osc_next(&x->a1) + jt_osc_next(&x->a2);
            float wb = jt_osc_next(&x->b1) + jt_osc_next(&x->b2);
            s += (wa + (wb - wa) * P.blend) * a * x->level;
        }
        s = jt_svf_next(&filter, s * 0.2f);

        float y;
        if (phase ^= 1) {                           /* effects on every second sample */
            half = s;
            y = jt_softclip(s + 0.5f * (wet_prev + wet_now));
        } else {
            wet_prev = wet_now;
            wet_now = fx(0.5f * (half + s));
            y = jt_softclip(s + wet_prev);
        }
        out[i] = (int16_t)(y * 30000.0f);
        scope[scope_w++] = (int8_t)(y * 120.0f);
    }
}

/* ---- pictures ---- */
static float wave_at(float p, float w) {            /* the blended wave shape, for drawing */
    float v[4];
    p -= (float)(int)p;
    v[0] = jt_sin(p);
    v[1] = p < 0.5f ? 4.0f * p - 1.0f : 3.0f - 4.0f * p;
    v[2] = 2.0f * p - 1.0f;
    v[3] = p < 0.5f ? 1.0f : -1.0f;
    int i = (int)w;
    return v[i] + (v[i + 1] - v[i]) * (w - (float)i);
}

static void block(int x, int y, int w, int h) {
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) jt_oled_pixel(x + i, y + j, true);
}

static void draw_tone(uint32_t t) {
    int8_t s[256];
    int peak = 0, start = 1;
    uint8_t w = scope_w;
    for (int i = 0; i < 256; i++) { s[i] = scope[(uint8_t)(w + i)]; if (s[i] > peak) peak = s[i]; }
    if (peak < 4) {                                 /* silent: draw the shape itself, and its detuned twin */
        float shape = knob[TONE][0] * 2.999f, drift = (float)t * 0.0004f * (0.2f + 4.0f * knob[TONE][1]);
        int prev = 16;
        for (int x = 0; x < 128; x++) {
            int y = 16 - (int)(11.0f * wave_at((float)x * (1.0f / 64.0f), shape));
            if (x) jt_oled_line(x - 1, prev, x, y, true);
            prev = y;
            if (x & 1) jt_oled_pixel(x, 16 - (int)(11.0f * wave_at((float)x * (1.0f / 64.0f) + drift, shape)), true);
        }
        return;
    }
    for (int i = 1; i < 128; i++) if (s[i - 1] < 0 && s[i] >= 0) { start = i; break; }   /* hold the picture still */
    int prev = 16;
    for (int x = 0; x < 128; x++) {
        int y = 16 - s[start + x] * 14 / (peak < 24 ? 24 : peak);
        if (x) jt_oled_line(x - 1, prev, x, y, true);
        prev = y;
    }
}

static void draw_motion(uint32_t dt) {
    static float angle;
    static uint8_t trail[24][2];
    static int head;
    float r = 3.0f + 11.0f * knob[MOTION][1];
    angle += (float)dt * 0.001f * 0.05f * jt_exp2(knob[MOTION][0] * 8.0f);
    angle -= (float)(int)angle;
    for (int i = 0; i < 48; i += 2)                 /* the orbit, dotted */
        jt_oled_pixel(64 + (int)(r * 3.6f * jt_sin((float)i / 48.0f + 0.25f)), 16 + (int)(r * jt_sin((float)i / 48.0f)), true);
    int x = 64 + (int)(r * 3.6f * jt_sin(angle + 0.25f)), y = 16 + (int)(r * jt_sin(angle));
    trail[head][0] = (uint8_t)x;
    trail[head][1] = (uint8_t)y;
    head = (head + 1) % 24;
    for (int i = 0; i < 24; i++) jt_oled_pixel(trail[i][0], trail[i][1], true);
    block(x - 2, y - 2, 5, 5);
    block(63, 15, 2, 2);
}

static void draw_colour(void) {
    int cut = 6 + (int)(108.0f * knob[COLOUR][0]), peak = (int)(15.0f * knob[COLOUR][1]), prev = 18;
    for (int x = 0; x < 128; x++) {
        int y = 18, d = x - cut;
        if (d > -8 && d <= 0) y = 18 - peak * (8 + d) * (8 + d) / 64;          /* the resonance bump */
        else if (d > 0) y = 18 - peak + d * (2 + peak / 5);                    /* the fall after the cutoff */
        if (y > 31) y = 31;
        if (x) jt_oled_line(x - 1, prev, x, y, true);
        prev = y;
        if (!(x & 3)) for (int j = y + 2; j < 32; j += 3) jt_oled_pixel(x, j, true);   /* what passes */
        if (y >= 31 && d > 0) break;
    }
}

static void draw_shape(uint32_t t) {
    static uint32_t seen, born;
    int x1 = 8 + (int)(44.0f * knob[SHAPE][0]), x2 = x1 + 8 + (int)(66.0f * knob[SHAPE][1]), prev = 4;
    if (note_count != seen) { seen = note_count; born = t; }
    jt_oled_line(4, 29, x1, 4, true);
    for (int x = x1; x <= x2; x++) {                /* the release curve */
        float u = (float)(x2 - x) / (float)(x2 - x1);
        int y = 29 - (int)(25.0f * u * u);
        jt_oled_line(x > x1 ? x - 1 : x, prev, x, y, true);
        prev = y;
    }
    for (int x = 0; x < 128; x += 4) jt_oled_pixel(x, 31, true);
    uint32_t age = t - born;                        /* a note travels along the shape */
    if (age < 900) {
        int x = 4 + (int)((uint32_t)(x2 - 4) * age / 900), y;
        if (x <= x1) y = 29 - 25 * (x - 4) / (x1 - 4);
        else { float u = (float)(x2 - x) / (float)(x2 - x1); y = 29 - (int)(25.0f * u * u); }
        block(x - 2, y - 2, 5, 5);
    }
}

static void scale_boxes(int y) {                    /* twelve boxes: filled = in the scale */
    for (int i = 0; i < 12; i++) {
        int x = 17 + i * 8;
        if (P.scale >> i & 1) block(x, y, 6, i ? 4 : 6);
        else { jt_oled_pixel(x + 2, y + 2, true); jt_oled_pixel(x + 3, y + 2, true); }
    }
}

static void draw_life(void) {
    static struct { int8_t x; float y; } star[20];
    static uint32_t seen;
    static int next;
    if (note_count != seen) {
        seen = note_count;
        int x = ((int)note_last - 33) * 2;          /* low notes left, high notes right */
        star[next].x = (int8_t)(x < 1 ? 1 : x > 126 ? 126 : x);
        star[next].y = 1.0f;
        next = (next + 1) % 20;
    }
    for (int i = 0; i < 20; i++) {
        if (star[i].y <= 0.0f) continue;
        int x = star[i].x, y = (int)star[i].y;
        jt_oled_pixel(x, y, true);
        if (y < 6) { jt_oled_pixel(x - 1, y, true); jt_oled_pixel(x + 1, y, true); jt_oled_pixel(x, y - 1, true); jt_oled_pixel(x, y + 1, true); }
        star[i].y += 0.5f + star[i].y * 0.04f;
        if (star[i].y > 22.0f) star[i].y = 0.0f;
    }
    scale_boxes(25);
}

static void draw_space(void) {
    static float ring[8];
    static uint32_t seen;
    static int next;
    float reach = 8.0f + 24.0f * knob[SPACE][0];
    int blur = knob[SPACE][1] > 0.5f;
    if (note_count != seen) { seen = note_count; ring[next] = 1.0f; next = (next + 1) % 8; }
    for (int i = 0; i < 8; i++) {
        if (ring[i] <= 0.0f) continue;
        float r = ring[i];
        int points = 16 + (int)r * 2;
        for (int k = 0; k < points; k++) {
            float a = (float)k / (float)points;
            int x = 64 + (int)(r * 2.1f * jt_sin(a + 0.25f)), y = 16 + (int)(r * 0.55f * jt_sin(a));
            jt_oled_pixel(x, y, true);
            if (blur && (k & 1)) jt_oled_pixel(x + (x < 64 ? -2 : 2), y, true);
        }
        ring[i] += 0.35f + 0.5f * (1.0f - knob[SPACE][1]);      /* more reverb: slower ripples */
        if (ring[i] > reach) ring[i] = 0.0f;
    }
    block(63, 15, 2, 2);
}

/* bit n = the button of scene n, bit 6 = SELECT pressed */
static uint8_t buttons(void) {
    uint8_t k = jt_keys_raw(0), b = 0;
    if (jt_button_osc()) b |= 1;
    if (k & JT_KEY_LFO) b |= 2;
    if (k & JT_KEY_FILTER) b |= 4;
    if (k & JT_KEY_AMP) b |= 8;
    if (k & JT_KEY_ARP) b |= 16;
    if (jt_button_settings()) b |= 32;
    if (k & JT_KEY_SELECT) b |= 64;
    return b;
}

int main(void) {
    int scene = START_SCENE, scale = 0, quiet = 0;
    int anchor[2], owns[2] = {0, 0};
    uint16_t last_bits = 0, stable = 0, playing = 0;
    uint8_t held_buttons, ev[4];
    uint32_t t_last, scale_shown = 0;
    int32_t enc_last;

    jt_init();
    for (int i = 0; i < VOICES; i++) voice[i].key = -1;
    jt_tape_init(&tape, tape_buf, TAPE);
    float gen = knob[LIFE][0];
    knob[LIFE][0] = 0.0f;                           /* the generator waits until the intro is over */
    apply();
    jt_audio_init(render);
    jt_usb_init();
    intro();
    knob[LIFE][0] = gen;
    apply();

    held_buttons = buttons();
    enc_last = jt_encoder();
    anchor[0] = jt_pot(JT_ADC_CUTOFF);
    anchor[1] = jt_pot(JT_ADC_RESONANCE);
    t_last = audio_blocks * 2;
    jt_leds((uint8_t)(1u << scene));

    for (;;) {
        jt_delay_ms(8);
        jt_poll_escape();
        uint32_t t = audio_blocks * 2, dt = t - t_last;
        t_last = t;

        /* buttons: a scene each; SELECT toggles hold */
        uint8_t now = buttons(), pressed = (uint8_t)(now & ~held_buttons);
        held_buttons = now;
        if (!((now & 1) && (now & 32)))
            for (int i = 0; i < SCENES; i++)
                if ((pressed & (1u << i)) && i != scene) {
                    scene = i;
                    jt_leds((uint8_t)(1u << i));
                    anchor[0] = jt_pot(JT_ADC_CUTOFF);
                    anchor[1] = jt_pot(JT_ADC_RESONANCE);
                    owns[0] = owns[1] = 0;
                }
        if (pressed & 64) {
            P.hold = !P.hold;
            if (!P.hold) for (int k = 0; k < 128; k++) note_off(k);
        }

        /* SELECT turn: the scale */
        int steps = (jt_encoder() - enc_last) / 8;
        if (steps) {
            enc_last += steps * 8;
            scale = ((scale + steps) % SCALE_COUNT + SCALE_COUNT) % SCALE_COUNT;
            P.scale = SCALES[scale];
            scale_shown = t;
        }

        /* knobs: each takes over the open scene's control once it has been moved */
        bool changed = false;
        for (int i = 0; i < 2; i++) {
            int raw = jt_pot(i == 0 ? JT_ADC_CUTOFF : JT_ADC_RESONANCE), d = raw - anchor[i];
            if (!owns[i] && (d > 90 || d < -90)) owns[i] = 1;
            if (!owns[i]) continue;
            float v = raw >= 4000 ? 1.0f : (float)raw * (1.0f / 4000.0f), old = knob[scene][i];
            if (v - old > 0.004f || old - v > 0.004f) { knob[scene][i] = v; changed = true; }
        }
        if (changed) apply();
        jt_filter((uint16_t)(knob[COLOUR][0] * 1000.0f), (uint16_t)(knob[COLOUR][1] * 1000.0f));

        /* keyboard: two agreeing readings to change, a few empty ones to let go */
        uint16_t bits = jt_keyboard();
        if (bits && bits == last_bits) { stable = bits; quiet = 0; }
        else if (!bits && ++quiet >= 3) stable = 0;
        last_bits = bits;
        for (int k = 0; k < JT_KEYBOARD_KEYS; k++) {
            uint16_t b = JT_KEYBOARD_BIT(k);
            int key = 47 + k;                       /* the lowest key is B2 */
            if ((stable & b) && !(playing & b)) { note_on(key, 100); P.home = (uint8_t)key; jt_midi_send(0x90, (uint8_t)key, 100); }
            if (!(stable & b) && (playing & b)) { if (!P.hold) note_off(key); jt_midi_send(0x80, (uint8_t)key, 0); }
        }
        playing = stable;
        while (jt_midi_get(ev)) {
            int kind = ev[0] & 0x0F;
            if (kind == 9 && ev[3]) note_on(ev[2], ev[3]);
            else if ((kind == 8 || kind == 9) && !P.hold) note_off(ev[2]);
        }

        /* the picture */
        jt_oled_clear();
        if (t - scale_shown < 1200 && scale_shown) {
            scale_boxes(13);
        } else switch (scene) {
            case TONE:   draw_tone(t); break;
            case MOTION: draw_motion(dt); break;
            case COLOUR: draw_colour(); break;
            case SHAPE:  draw_shape(t); break;
            case LIFE:   draw_life(); break;
            default:     draw_space();
        }
        if (P.hold) block(124, 0, 4, 4);
        jt_oled_flush();
    }
}
