/* Drone: an additive drone you sculpt with your fingers. It always sounds.
 *
 * The sound is up to four notes, each built from 16 sine partials. The 16 touch keys are
 * those 16 partials, or 16 of something else, depending on the open page. Six buttons open
 * six pages; the two knobs shape the open page; the display draws what you are changing.
 *
 *   page (button)      keys                       CUTOFF knob    RESONANCE knob   SELECT turn
 *   PARTIALS (OSC)     partial on / off;          tilt:          odd / even       transpose
 *                      hold + SELECT: its level   dark..bright   balance
 *   MOTION (LFO)       which partials breathe     rate           depth            transpose
 *   FILTER             16 steps that pulse the    cutoff         resonance        pulse speed
 *                      analog filter
 *   WARP (AMP)         which partials beat        stretch:       fold             transpose
 *                                                 bell..harmonic
 *   CHORD (ARP)        the notes (up to four)     spread         drift            transpose
 *   SPACE (SETTINGS)   16 scenes: tap to morph    reverb         echo             morph time
 *                      to one, hold to store
 *
 *   SELECT press       silence / sound
 *   USB MIDI note      sets the root note
 */
#include "jt4000.h"
#include "jt_dsp.h"

#define PARTIALS_N 16
#define VOICES 4
#define ROOT 35                             /* the lowest key is B1; the second, C2, is the usual root */
#define TAPE 4096                           /* 0.26 s of echo at 16 kHz */
#define SINE 512
#ifndef START_PAGE
#define START_PAGE 0                        /* build with DEFS=-DSTART_PAGE=2 to open on another page */
#endif

enum { PARTIALS, MOTION, FILTER, WARP, CHORD, SPACE, PAGES };

/* A scene: everything about the sound. Levels 0..8, knobs 0..255. */
typedef struct {
    uint8_t level[PARTIALS_N];
    uint16_t breathe, beat, chord, steps;
    uint8_t knob[PAGES][2];
    int8_t transpose;
} scene_t;

#define K(a, b) {a, b}
static scene_t scenes[16] = {
    /* warm fifth */
    {{8, 6, 5, 4, 3, 3, 2, 2, 1, 1, 1, 0, 0, 0, 0, 0}, 0x3FC0, 0x0000, 0x4080, 0x0000,
     {K(110, 128), K(70, 130), K(170, 40), K(128, 0), K(60, 50), K(120, 70)}, 0},
    /* hollow: odd partials only */
    {{8, 0, 6, 0, 5, 0, 4, 0, 3, 0, 2, 0, 2, 0, 1, 0}, 0x2AA8, 0x0000, 0x4004, 0x0000,
     {K(140, 128), K(50, 160), K(150, 60), K(128, 0), K(90, 40), K(150, 90)}, 0},
    /* bell: stretched partials that beat */
    {{8, 5, 0, 6, 0, 4, 3, 0, 5, 0, 0, 3, 0, 2, 0, 2}, 0x1248, 0x5B25, 0x4000, 0x0000,
     {K(170, 128), K(40, 120), K(210, 30), K(215, 0), K(40, 30), K(200, 120)}, 12},
    /* organ: octaves */
    {{8, 8, 0, 8, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 8}, 0x0101, 0x0000, 0x4480, 0x0000,
     {K(200, 128), K(90, 60), K(190, 20), K(128, 0), K(110, 20), K(90, 0)}, 0},
    /* glass: only the high partials, all moving */
    {{0, 0, 0, 2, 0, 4, 0, 6, 7, 8, 6, 5, 4, 3, 2, 2}, 0x1FFF, 0x0550, 0x4081, 0x0000,
     {K(200, 128), K(120, 220), K(230, 30), K(128, 0), K(140, 90), K(210, 150)}, 0},
    /* growl: everything, folded, pulsing through a tight filter */
    {{8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8}, 0x0000, 0x0000, 0x4000, 0x8888,
     {K(150, 128), K(60, 80), K(70, 170), K(128, 150), K(180, 60), K(60, 110)}, -12},
    /* swarm: a cluster, every partial beating and breathing */
    {{5, 5, 5, 5, 5, 5, 5, 5, 4, 4, 4, 3, 3, 2, 2, 1}, 0xFFFF, 0xFFFF, 0x6000, 0x0000,
     {K(120, 128), K(190, 200), K(140, 70), K(128, 20), K(255, 255), K(170, 100)}, 0},
    /* whistle: one note and a filter that sings by itself */
    {{8, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 0x0000, 0x0000, 0x4000, 0xA0A0,
     {K(128, 128), K(40, 40), K(110, 250), K(128, 0), K(20, 120), K(130, 160)}, 0},
    /* minor: three notes, slow */
    {{8, 5, 4, 2, 3, 1, 2, 0, 1, 0, 0, 0, 0, 0, 0, 0}, 0x7E00, 0x0000, 0x4880, 0x0000,
     {K(120, 128), K(30, 170), K(160, 50), K(128, 0), K(80, 110), K(190, 60)}, 0},
    /* buzz: bright, even partials */
    {{4, 8, 0, 8, 0, 7, 0, 6, 0, 5, 0, 4, 0, 3, 0, 2}, 0x0554, 0x1110, 0x4000, 0x8080,
     {K(220, 200), K(150, 100), K(120, 120), K(128, 60), K(120, 40), K(80, 140)}, -5},
    /* choir: mid partials like vowel formants */
    {{6, 3, 2, 7, 8, 4, 1, 1, 2, 5, 4, 1, 0, 0, 0, 0}, 0x1E7C, 0x0000, 0x4880, 0x0000,
     {K(128, 128), K(80, 190), K(180, 30), K(128, 0), K(160, 130), K(200, 40)}, 7},
    /* metal: squeezed partials */
    {{8, 7, 6, 6, 5, 5, 4, 4, 3, 3, 2, 2, 2, 1, 1, 1}, 0x0F0F, 0xAAAA, 0x4000, 0x0000,
     {K(180, 128), K(100, 110), K(160, 90), K(40, 30), K(70, 60), K(150, 130)}, 0},
    /* sub: weight and little else */
    {{8, 4, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 0x4000, 0x0000, 0x4004, 0x0000,
     {K(60, 128), K(20, 90), K(90, 100), K(128, 40), K(30, 20), K(40, 0)}, -12},
    /* pulse: a rhythm on the filter */
    {{8, 6, 6, 4, 4, 3, 3, 2, 2, 2, 1, 1, 1, 1, 0, 0}, 0x0000, 0x0000, 0x4080, 0x9292,
     {K(160, 128), K(60, 60), K(60, 200), K(128, 90), K(100, 40), K(60, 180)}, 0},
    /* shimmer: fifths stacked high */
    {{2, 3, 8, 2, 3, 8, 1, 2, 7, 1, 1, 6, 1, 0, 0, 4}, 0xFFFF, 0x2492, 0x4081, 0x0000,
     {K(190, 128), K(170, 180), K(220, 40), K(128, 0), K(200, 100), K(230, 170)}, 0},
    /* one sine */
    {{8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 0x0000, 0x0000, 0x4000, 0x0000,
     {K(128, 128), K(60, 0), K(255, 0), K(128, 0), K(0, 0), K(0, 0)}, 12},
};

/* ---- the live sound: what the scene is gliding to, and where it is now ---- */
static scene_t goal;                        /* masks and notes are used from here directly */
static float level[PARTIALS_N];             /* 0..1, gliding to goal.level */
static float knob[PAGES][2];                /* 0..1, gliding to goal.knob */
static float morph = 0.05f;                 /* how far to glide every 8 ms */

/* ---- shared with the audio interrupt (single words: safe to write from the main loop) ---- */
static volatile uint32_t audio_blocks;      /* 2 ms each */
static uint32_t inc[VOICES][PARTIALS_N];    /* phase step of every partial; 0 = silent */
static float amp_goal[PARTIALS_N];
static float voice_goal[VOICES];            /* 1 while the note sounds */
static volatile float gain_goal, fold, echo_mix, echo_fb, reverb_mix;
static volatile int8_t scope[128];

static float sine[SINE + 1];
static int16_t tape_buf[TAPE];
static jt_tape_t tape;
static jt_reverb_t reverb = {.decay = 0.86f, .damp = 0.35f};

static float fx(float in) {
    static float tone;
    float d = jt_tape_read(&tape, (float)(TAPE - 40));
    tone += (d - tone) * 0.5f;
    jt_tape_write(&tape, in + tone * echo_fb);
    float wet = d * echo_mix;
    return wet + jt_reverb_next(&reverb, in + wet) * reverb_mix;
}

static void render(int16_t *out, int n) {
    static float buf[JT_AUDIO_BLOCK], amp[PARTIALS_N], voice_gain[VOICES], gain;
    static float wet_now, wet_prev, half;
    static uint32_t phase[VOICES][PARTIALS_N];
    static int alt, scope_w;
    float amp_step[PARTIALS_N], per = 1.0f / (float)n;

    audio_blocks++;
    for (int i = 0; i < n; i++) buf[i] = 0.0f;
    for (int p = 0; p < PARTIALS_N; p++) amp_step[p] = (amp_goal[p] - amp[p]) * 0.25f * per;

    for (int v = 0; v < VOICES; v++) {
        float g0 = voice_gain[v], g1 = g0 + (voice_goal[v] - g0) * 0.02f, dg = (g1 - g0) * per;
        voice_gain[v] = g1;
        if (g0 < 1e-4f && g1 < 1e-4f) continue;
        for (int p = 0; p < PARTIALS_N; p++) {
            uint32_t step = inc[v][p], ph = phase[v][p];
            float a = amp[p], da = amp_step[p], g = g0;
            if (!step || (a < 1e-5f && da <= 0.0f)) continue;
            for (int i = 0; i < n; i++) {
                uint32_t at = ph >> 23;
                float s = sine[at] + (sine[at + 1] - sine[at]) * ((float)((ph >> 7) & 0xFFFF) * (1.0f / 65536.0f));
                buf[i] += s * a * g;
                a += da;
                g += dg;
                ph += step;
            }
            phase[v][p] = ph;
        }
    }
    for (int p = 0; p < PARTIALS_N; p++) amp[p] += amp_step[p] * (float)n;

    float gain_step = (gain_goal - gain) * 0.05f * per, f = fold;
    for (int i = 0; i < n; i++) {
        float x = buf[i] * gain;
        gain += gain_step;
        if (f > 0.01f) {                            /* fold: the wave bends back on itself */
            float ph = x * (1.0f + 6.0f * f) * 0.25f + 8.0f;
            float folded = jt_sin(ph - (float)(int)ph);
            float mix = f * 3.0f > 1.0f ? 1.0f : f * 3.0f;
            x += (folded - x) * mix;
        }
        float y;
        if (alt ^= 1) {                             /* echo and reverb on every second sample */
            half = x;
            y = jt_softclip(x + 0.5f * (wet_prev + wet_now));
        } else {
            wet_prev = wet_now;
            wet_now = fx(0.5f * (half + x));
            y = jt_softclip(x + wet_prev);
        }
        out[i] = (int16_t)(y * 30000.0f);
        if ((i & 3) == 0) scope[scope_w++ & 127] = (int8_t)(y * 120.0f);
    }
}

/* ---- control: every 8 ms, turn the scene into numbers for the interrupt ---- */
static const float LOG2[PARTIALS_N] = {0.0f, 1.0f, 1.585f, 2.0f, 2.3219f, 2.585f, 2.8074f, 3.0f,
                                       3.1699f, 3.3219f, 3.4594f, 3.585f, 3.7004f, 3.8074f, 3.9069f, 4.0f};
static int8_t voice_key[VOICES] = {-1, -1, -1, -1};
static float breath_phase[PARTIALS_N], drift[VOICES], drift_goal[VOICES];
static float pulse;                         /* the filter's pulse envelope, 0..1 */
static int step_now;
static float step_ms = 220.0f;
static bool muted;

static void load(const scene_t *s) { goal = *s; }

static void control(uint32_t dt) {
    /* glide */
    for (int p = 0; p < PARTIALS_N; p++) level[p] += ((float)goal.level[p] * 0.125f - level[p]) * morph;
    for (int g = 0; g < PAGES; g++)
        for (int i = 0; i < 2; i++) knob[g][i] += ((float)goal.knob[g][i] * (1.0f / 255.0f) - knob[g][i]) * morph;

    /* notes: keep the voices that still have their key, give free ones to new keys */
    for (int v = 0; v < VOICES; v++)
        if (voice_key[v] >= 0 && !(goal.chord & JT_KEYBOARD_BIT(voice_key[v]))) { voice_goal[v] = 0.0f; voice_key[v] = -1; }
    int notes = 0;
    for (int k = 0; k < JT_KEYBOARD_KEYS; k++) {
        if (!(goal.chord & JT_KEYBOARD_BIT(k))) continue;
        int have = -1, spare = -1;
        for (int v = 0; v < VOICES; v++) {
            if (voice_key[v] == k) have = v;
            if (voice_key[v] < 0 && spare < 0) spare = v;
        }
        if (have < 0 && spare >= 0) { voice_key[spare] = (int8_t)k; have = spare; }
        if (have >= 0) { voice_goal[have] = 1.0f; notes++; }
    }

    /* partial levels: tilt, odd/even balance, breathing */
    float tilt = 2.0f * (1.0f - knob[PARTIALS][0]);         /* each octave up loses 0..12 dB */
    float balance = knob[PARTIALS][1], rate = 0.02f + 1.2f * knob[MOTION][0] * knob[MOTION][0];
    float depth = knob[MOTION][1], power = 0.0f;
    for (int p = 0; p < PARTIALS_N; p++) {
        float a = level[p] * jt_exp2(-LOG2[p] * tilt);
        bool even = p & 1;                                   /* partial 2, 4, ... */
        float side = even ? balance : 1.0f - balance;
        a *= side >= 0.5f ? 1.0f : 2.0f * side;
        power += a * a;
        if (goal.breathe >> (15 - p) & 1) {
            breath_phase[p] += rate * (0.6f + 0.11f * (float)((p * 7) % 9)) * (float)dt * 0.001f;
            if (breath_phase[p] >= 1.0f) breath_phase[p] -= 1.0f;
            a *= 1.0f - depth * (0.5f + 0.5f * jt_sin(breath_phase[p]));
        }
        amp_goal[p] = a;
    }
    gain_goal = muted || power < 1e-6f || !notes ? 0.0f : 0.28f / jt_sqrt(power * (float)notes);

    /* pitches: stretch moves the partials apart or together; beat and spread detune them */
    float stretch = 1.0f + (knob[WARP][0] - 0.5f) * 0.7f;
    float spread = knob[CHORD][0] * 0.006f, wander = knob[CHORD][1];
    for (int v = 0; v < VOICES; v++) {
        if (voice_key[v] < 0) continue;
        if (jt_rand() > 0.96f) drift_goal[v] = jt_rand();
        drift[v] += (drift_goal[v] - drift[v]) * 0.01f;
        float hz = jt_note_hz((float)(ROOT + voice_key[v] + goal.transpose) + 0.35f * wander * drift[v]);
        for (int p = 0; p < PARTIALS_N; p++) {
            float f = hz * jt_exp2(LOG2[p] * stretch);
            float sign = p & 1 ? -1.0f : 1.0f;
            f *= 1.0f + sign * spread * (float)(v + 1) * 0.5f;
            if (goal.beat >> (15 - p) & 1) f *= 1.0f + 0.004f * sign;
            inc[v][p] = f < 14000.0f ? (uint32_t)(f * (4294967296.0f / (float)JT_SAMPLE_RATE)) : 0;
        }
    }

    /* the filter: the knobs, plus a pulse on every lit step */
    static float step_left;
    step_left -= (float)dt;
    if (step_left <= 0.0f) {
        step_left += step_ms;
        if (step_left < 0.0f) step_left = step_ms;
        step_now = (step_now + 1) & 15;
        if (goal.steps >> (15 - step_now) & 1) pulse = 1.0f;
    }
    pulse *= 0.88f;
    float cut = knob[FILTER][0] + pulse * (1.0f - knob[FILTER][0]) * 0.7f;
    jt_filter((uint16_t)(cut * 1000.0f), (uint16_t)(knob[FILTER][1] * 1000.0f));

    fold = knob[WARP][1];
    reverb_mix = knob[SPACE][0] * 1.3f;
    echo_mix = knob[SPACE][1] * 0.8f;
    echo_fb = 0.25f + 0.55f * knob[SPACE][1];
}

/* ---- pictures ---- */
static void block(int x, int y, int w, int h) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) jt_oled_pixel(i, j, true);
}
static void frame(int x, int y, int w, int h) {
    for (int i = x; i < x + w; i++) { jt_oled_pixel(i, y, true); jt_oled_pixel(i, y + h - 1, true); }
    for (int j = y; j < y + h; j++) { jt_oled_pixel(x, j, true); jt_oled_pixel(x + w - 1, j, true); }
}

/* 16 bars on the left half; `dots` marks some of them, `held` are the touched keys */
static void bars(const float *v, uint16_t dots, uint16_t held) {
    for (int p = 0; p < PARTIALS_N; p++) {
        int h = (int)(v[p] * 26.0f + 0.5f), x = p * 4;
        if (h > 26) h = 26;
        if (h > 0) block(x, 31 - h, 3, h);
        else jt_oled_pixel(x + 1, 31, true);
        if (dots >> (15 - p) & 1) block(x, 0, 3, 2);
        if (held >> (15 - p) & 1) frame(x - 1 < 0 ? 0 : x - 1, 3, 5, 29);
    }
}

/* one cycle of the wave these levels make, on the right half */
static void wave(const float *a) {
    float total = 0.0f;
    int last = 16;
    for (int p = 0; p < PARTIALS_N; p++) total += a[p];
    if (total < 0.01f) total = 0.01f;
    for (int x = 0; x < 60; x++) {
        float y = 0.0f;
        for (int p = 0; p < PARTIALS_N; p++)
            if (a[p] > 0.004f) y += a[p] * sine[((p + 1) * x * SINE / 60) & (SINE - 1)];
        int py = 16 - (int)(y / total * 26.0f);
        if (py < 0) py = 0;
        if (py > 31) py = 31;
        if (x) jt_oled_line(67 + x - 1, last, 67 + x, py, true);
        last = py;
    }
}

static void draw_filter(void) {
    int cx = 6 + (int)((knob[FILTER][0] + pulse * (1.0f - knob[FILTER][0]) * 0.7f) * 112.0f);
    int peak = (int)(knob[FILTER][1] * 11.0f), last = 12;
    for (int x = 0; x < JT_OLED_W; x++) {
        int y = 12;
        if (x > cx) y = 12 + (x - cx) * 2;
        else if (cx - x < 8) y = 12 - peak * (8 - (cx - x)) / 8;
        if (y > 20) y = 20;
        if (x) jt_oled_line(x - 1, last, x, y, true);
        last = y;
    }
    for (int s = 0; s < 16; s++) {
        if (goal.steps >> (15 - s) & 1) block(s * 8 + 1, 25, 6, 6);
        else frame(s * 8 + 1, 25, 6, 6);
        if (s == step_now) block(s * 8 + 2, 22, 4, 2);
    }
}

static void draw_warp(uint32_t t) {
    float stretch = 1.0f + (knob[WARP][0] - 0.5f) * 0.7f;
    for (int p = 0; p < PARTIALS_N; p++) {
        int x = 2 + (int)(LOG2[p] * stretch * 23.0f);
        int h = 3 + (int)(level[p] * 24.0f);
        if (x > 127) continue;
        if ((goal.beat >> (15 - p) & 1) && ((t >> 7) + (uint32_t)p) & 1) x++;     /* beating partials shiver */
        for (int y = 31 - h; y < 32; y++) jt_oled_pixel(x, y, true);
    }
    int f = (int)(knob[WARP][1] * 30.0f);                    /* fold: a wave bending back */
    for (int i = 0; i < f; i++) jt_oled_pixel(127 - i, i % 4 < 2 ? 0 : 1, true);
}

static void draw_chord(void) {
    static const uint8_t SHARPS[12] = {0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};     /* from C: C# D# F# G# A# */
    for (int k = 0; k < JT_KEYBOARD_KEYS; k++) {
        bool sharp = SHARPS[(k + 11) % 12];                  /* key 0 is a B */
        int x = k * 8, top = sharp ? 6 : 12;
        frame(x, top, 7, 32 - top);
        if (sharp) block(x, top, 7, 4);
        if (goal.chord & JT_KEYBOARD_BIT(k)) block(x + 2, 22, 3, 6);
    }
    int tr = goal.transpose, ax = tr < 0 ? -tr : tr;         /* transpose: a row of ticks from the centre */
    for (int i = 0; i < ax; i++) jt_oled_pixel(64 + (tr < 0 ? -1 - 2 * i : 2 * i), 1, true);
    block(63, 0, 1, 3);
}

static void draw_space(int current, uint32_t flash) {
    int last = 9;
    for (int x = 0; x < JT_OLED_W; x++) {
        int y = 9 - scope[x] / 14;
        if (y < 0) y = 0;
        if (y > 19) y = 19;
        if (x) jt_oled_line(x - 1, last, x, y, true);
        last = y;
    }
    for (int s = 0; s < 16; s++) {
        if (s == current) block(s * 8 + 1, 24, 6, 7);
        else frame(s * 8 + 1, 24, 6, 7);
    }
    if (flash) frame(0, 22, 128, 10);
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

int main(void) {
    int page = START_PAGE, current = 0, quiet = 0;
    int anchor[2], owns[2] = {0, 0};
    uint16_t last_bits = 0, stable = 0, touched = 0, turned = 0;
    uint8_t held = 0, ev[4], remembered[PARTIALS_N];
    uint32_t t_last, key_since = 0, stored_at = 0;
    int32_t enc_last;

    jt_init();
    for (int i = 0; i <= SINE; i++) sine[i] = jt_sin((float)i * (1.0f / (float)SINE));
    for (int p = 0; p < PARTIALS_N; p++) remembered[p] = 8;
    jt_tape_init(&tape, tape_buf, TAPE);
    load(&scenes[0]);
    morph = 1.0f;                           /* jump to the first scene, then glide from there */
    control(8);
    morph = 0.05f;
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

        /* buttons: a page each; SELECT silences */
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
        if (pressed & 64) muted = !muted;

        /* keyboard: two agreeing readings to change, a few empty ones to let go */
        uint16_t bits = jt_keyboard();
        if (bits && bits == last_bits) { stable = bits; quiet = 0; }
        else if (!bits && ++quiet >= 3) stable = 0;
        last_bits = bits;
        uint16_t down = (uint16_t)(stable & ~touched), up = (uint16_t)(touched & ~stable);
        touched = stable;
        if (down) { key_since = t; stored_at = 0; }
        if (down && !(touched & ~down)) turned = 0;

        /* SELECT turn: the held partials' level, or the page's own setting */
        int steps = (jt_encoder() - enc_last) / 8;
        if (steps) {
            enc_last += steps * 8;
            if (page == PARTIALS && stable) {
                turned |= stable;
                for (int p = 0; p < PARTIALS_N; p++) {
                    if (!(stable >> (15 - p) & 1)) continue;
                    int v = goal.level[p] + steps;
                    goal.level[p] = (uint8_t)(v < 0 ? 0 : v > 8 ? 8 : v);
                    level[p] = (float)goal.level[p] * 0.125f;
                }
            } else if (page == FILTER) {
                step_ms *= jt_exp2(-0.12f * (float)steps);
                if (step_ms < 40.0f) step_ms = 40.0f;
                if (step_ms > 2000.0f) step_ms = 2000.0f;
            } else if (page == SPACE) {
                morph *= jt_exp2(-0.4f * (float)steps);     /* right: slower */
                if (morph < 0.002f) morph = 0.002f;
                if (morph > 0.5f) morph = 0.5f;
            } else {
                int v = goal.transpose + steps;
                goal.transpose = (int8_t)(v < -24 ? -24 : v > 24 ? 24 : v);
            }
        }

        /* keys: what a tap means depends on the page */
        for (int k = 0; k < JT_KEYBOARD_KEYS; k++) {
            uint16_t b = JT_KEYBOARD_BIT(k);
            if (page == SPACE) {
                if ((stable & b) && key_since && t - key_since > 900 && !stored_at) {    /* hold: store */
                    scenes[k] = goal;
                    for (int p = 0; p < PARTIALS_N; p++) scenes[k].level[p] = goal.level[p];
                    current = k;
                    stored_at = t;
                    turned |= b;
                }
                if ((up & b) && !(turned & b)) {                                         /* tap: morph to it */
                    load(&scenes[k]);
                    current = k;
                    anchor[0] = jt_pot(JT_ADC_CUTOFF);
                    anchor[1] = jt_pot(JT_ADC_RESONANCE);
                    owns[0] = owns[1] = 0;
                }
                continue;
            }
            if (!(up & b) || (turned & b)) continue;
            switch (page) {
            case PARTIALS:
                if (goal.level[k]) { remembered[k] = goal.level[k]; goal.level[k] = 0; }
                else goal.level[k] = remembered[k];
                level[k] = (float)goal.level[k] * 0.125f;
                break;
            case MOTION: goal.breathe ^= b; break;
            case FILTER: goal.steps ^= b; break;
            case WARP:   goal.beat ^= b; break;
            default: {                                       /* CHORD: at most four notes */
                int count = 0;
                for (int i = 0; i < 16; i++) count += goal.chord >> i & 1;
                if ((goal.chord & b) || count < VOICES) goal.chord ^= b;
            }
            }
        }
        if (!stable) turned = 0;
        if (stored_at && t - stored_at > 500 && !stable) stored_at = 0;

        /* knobs: each takes over the open page's control once it has been moved */
        for (int i = 0; i < 2; i++) {
            int raw = jt_pot(i == 0 ? JT_ADC_CUTOFF : JT_ADC_RESONANCE), d = raw - anchor[i];
            if (!owns[i] && (d > 90 || d < -90)) owns[i] = 1;
            if (!owns[i]) continue;
            int v = raw >= 4000 ? 255 : raw * 255 / 4000, old = goal.knob[page][i];
            if (v - old > 1 || old - v > 1) {
                goal.knob[page][i] = (uint8_t)v;
                knob[page][i] = (float)v * (1.0f / 255.0f);
            }
        }

        while (jt_midi_get(ev))
            if ((ev[0] & 0x0F) == 9 && ev[3]) {
                int v = ev[2] - (ROOT + 1);
                goal.transpose = (int8_t)(v < -24 ? -24 : v > 24 ? 24 : v);
            }

        control(dt);

        jt_oled_clear();
        switch (page) {
        case PARTIALS: bars(level, 0, stable); wave(amp_goal); break;
        case MOTION:   bars(amp_goal, goal.breathe, stable); wave(amp_goal); break;
        case FILTER:   draw_filter(); break;
        case WARP:     draw_warp(t); break;
        case CHORD:    draw_chord(); break;
        default:       draw_space(current, stored_at);
        }
        if (muted) { block(120, 0, 2, 6); block(124, 0, 2, 6); }
        jt_oled_flush();
    }
}
