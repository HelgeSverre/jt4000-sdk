/* Groove: a six-track groovebox. The 16 touch keys are the 16 steps of a bar.
 *
 *   tracks (buttons)   KICK (OSC), SNARE (LFO), HAT (FILTER), BASS (AMP), LEAD (ARP), VOICE (SETTINGS)
 *   track button       tap: show that track's steps on the keys
 *   touch a key        add a step, or remove the one that is there
 *   hold a step and    turn SELECT: its value (drums: soft, normal, accent / open hat;
 *                      bass and lead: the note, in A minor; voice: the word)
 *                      press SELECT: accent and slide (bass and lead)
 *   knobs              the analog filter over everything: cutoff, resonance
 *   hold a track and   touch a key: one of 16 sounds for that track
 *                      turn a knob: adjust the sound (two settings each, see README)
 *                      turn SELECT: right evolves the track at random, left undoes it
 *                      hold SELECT for a second: clear the track (the display counts down)
 *   SELECT             tap: play / stop.   Turn: tempo.
 *   hold SELECT and    touch a key: go to that pattern at the end of the bar
 *                      press a track button: mute / unmute it
 *                      turn: swing
 */
#include "jt4000.h"
#include "jt_dsp.h"
#include "jt_lpc.h"
#include "words.h"

enum { KICK, SNARE, HAT, BASS, LEAD, VOX, TRACKS };
#define STEPS 16
#define PATTERNS 16
#define SR ((float)JT_SAMPLE_RATE)
#define TAPE 8192                           /* 0.51 s of echo at 16 kHz */
#define ACCENT 0x40                         /* bass and lead steps: louder, more filter */
#define SLIDE 0x80                          /* ...and: glide into the next note without a new attack */
#define DEGREE 0x1F                         /* ...and: the note, 1..21 = three octaves of A minor */
#define DEGREES 21

static uint8_t pat[PATTERNS][TRACKS][STEPS];    /* 0 = no step */

/* ---- factory patterns: drums as text (o soft, x normal, X accent), notes as degrees ---- */
#define A ACCENT +
#define S SLIDE +
static const struct {
    const char *drum[3];
    uint8_t bass[STEPS], lead[STEPS];
    struct { uint8_t step; const char *word; } vox[4];
} FACTORY[] = {
    {{"x...x...x...x...", "....x.......x...", "..x...x...x...X."},                  /* acid house */
     {1, 1, A 8, 1, 0, 3, S 1, 8, 1, 0, A 5, S 1, 8, 1, A 3, 0},
     {0, 0, 0, 0, 0, 0, 12, 0, 0, 0, 0, 0, 0, 0, 10, 0},
     {{12, "ACID"}}},
    {{"x...x...x...x.o.", "....o..o....x...", "oxoXoxoXoxoXoxoX"},                  /* techno */
     {0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, A 3, 1, 0},
     {0},
     {{0, "TECHNO"}}},
    {{"x..x..x...x..x..", "....x.......x.o.", "x.x.x.x.x.x.x.xX"},                  /* electro */
     {A 1, 0, 0, 1, 0, 0, S 3, 0, 1, 0, 0, 5, 0, S 4, 0, 0},
     {15, 0, 0, 12, 0, 0, 10, 0, 0, 0, 12, 0, 0, 0, 0, 0},
     {{4, "BODY"}, {12, "MOVE"}}},
    {{"x...x...x...x...", "....x.......x...", "..X...X...X...X."},                  /* trance */
     {0, 1, 1, 1, 0, 1, 1, 1, 0, 6, 6, 6, 0, 5, 5, 5},
     {8, 10, 12, 10, 15, 12, 10, 12, 13, 10, 12, 10, 12, 10, 8, 10},
     {{14, "UP"}}},
    {{"x.....x...x.....", "........x.......", "x.o.x.o.x.o.x.oX"},                  /* slow burn */
     {A 1, S 1, 3, 0, A 8, 0, S 7, 5, 0, 1, 0, A 10, S 8, 0, 5, 3},
     {0},
     {{7, "JACK"}, {15, "HOUSE"}}},
    {{"x.x...x..x..x...", "....x..o.o..x..x", "oxoxoxoXoxoxoxoX"},                  /* count it in */
     {0},
     {0},
     {{0, "ONE"}, {4, "TWO"}, {8, "THREE"}, {12, "FOUR"}}},
};
#undef A
#undef S

static int word_number(const char *name) {
    for (int w = 0; w < WORD_COUNT; w++) {
        const char *a = WORD_NAMES[w], *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (!*a && !*b) return w;
    }
    return 0;
}

static void factory(void) {
    for (unsigned p = 0; p < sizeof FACTORY / sizeof FACTORY[0]; p++) {
        for (int t = 0; t < 3; t++)
            for (int s = 0; s < STEPS; s++) {
                char c = FACTORY[p].drum[t][s];
                pat[p][t][s] = c == 'o' ? 1 : c == 'x' ? 2 : c == 'X' ? 3 : 0;
            }
        for (int s = 0; s < STEPS; s++) {
            pat[p][BASS][s] = FACTORY[p].bass[s];
            pat[p][LEAD][s] = FACTORY[p].lead[s];
        }
        for (int i = 0; i < 4; i++)
            if (FACTORY[p].vox[i].word)
                pat[p][VOX][FACTORY[p].vox[i].step] = (uint8_t)(word_number(FACTORY[p].vox[i].word) + 1);
    }
}

/* ---- sounds: 16 for every track. Six settings each, 0..255; the two knobs are the first two ---- */
#define SOUNDS 16
#define SETTINGS_N 6
static const uint8_t SOUND[TRACKS][SOUNDS][SETTINGS_N] = {
    /* kick: tune, length, pitch drop, drop time, drive, click */
    {{102, 128, 128, 60, 40, 40}, {70, 220, 90, 90, 10, 10}, {130, 100, 170, 40, 90, 110}, {40, 200, 50, 120, 0, 0},
     {120, 110, 200, 50, 230, 90}, {150, 50, 220, 20, 60, 220}, {220, 120, 60, 100, 20, 30}, {160, 70, 255, 110, 30, 0},
     {90, 40, 110, 30, 50, 60}, {60, 255, 120, 70, 60, 20}, {140, 160, 230, 80, 255, 120}, {100, 110, 70, 80, 0, 0},
     {190, 45, 140, 25, 100, 160}, {50, 150, 255, 160, 40, 0}, {115, 70, 150, 35, 70, 80}, {30, 255, 40, 200, 150, 0}},
    /* snare: body to noise, length, pitch, brightness of the noise, length of the body */
    {{128, 128, 110, 150, 100, 0}, {255, 90, 0, 220, 0, 0}, {90, 170, 60, 90, 160, 0}, {150, 60, 150, 180, 60, 0},
     {20, 40, 255, 100, 40, 0}, {230, 255, 80, 120, 80, 0}, {40, 150, 30, 60, 220, 0}, {200, 110, 180, 255, 70, 0},
     {160, 140, 90, 20, 120, 0}, {255, 35, 0, 200, 0, 0}, {110, 120, 130, 170, 140, 0}, {120, 100, 70, 40, 90, 0},
     {180, 70, 220, 230, 30, 0}, {10, 120, 100, 0, 200, 0}, {255, 200, 0, 60, 0, 0}, {60, 20, 255, 255, 20, 0}},
    /* hat: tone, length, length when open, metal */
    {{128, 100, 128, 0, 0, 0}, {255, 60, 100, 0, 0, 0}, {128, 100, 128, 200, 0, 0}, {200, 40, 220, 120, 0, 0},
     {60, 140, 160, 0, 0, 0}, {255, 20, 60, 255, 0, 0}, {100, 200, 255, 80, 0, 0}, {180, 80, 90, 160, 0, 0},
     {0, 120, 120, 0, 0, 0}, {220, 150, 200, 220, 0, 0}, {140, 30, 255, 40, 0, 0}, {255, 255, 255, 0, 0, 0},
     {90, 60, 60, 255, 0, 0}, {160, 110, 140, 100, 0, 0}, {40, 40, 200, 180, 0, 0}, {230, 90, 30, 60, 0, 0}},
    /* bass: cutoff, acid, wave (saw, square, saw + sub), envelope length, drive, glide */
    {{90, 150, 0, 110, 110, 110}, {80, 170, 128, 90, 120, 110}, {60, 60, 255, 140, 40, 80}, {110, 235, 0, 160, 220, 140},
     {70, 40, 128, 60, 0, 60}, {130, 120, 0, 30, 60, 40}, {100, 90, 255, 200, 160, 200}, {150, 200, 128, 50, 30, 110},
     {40, 130, 0, 180, 90, 160}, {180, 110, 0, 90, 70, 90}, {75, 190, 128, 25, 140, 230}, {120, 100, 255, 120, 255, 100},
     {50, 90, 128, 220, 60, 120}, {160, 220, 0, 20, 100, 60}, {85, 70, 255, 100, 10, 150}, {230, 60, 0, 128, 90, 100}},
    /* lead: length, echo, wave (saw + square, squares, saws), detune, octave, brightness */
    {{128, 100, 0, 60, 128, 128}, {60, 140, 128, 30, 128, 200}, {200, 120, 255, 200, 128, 180}, {150, 200, 128, 0, 255, 255},
     {80, 60, 255, 120, 0, 110}, {255, 160, 0, 160, 128, 90}, {20, 220, 128, 10, 255, 255}, {180, 80, 255, 255, 0, 220},
     {140, 120, 0, 40, 128, 50}, {70, 90, 255, 90, 128, 255}, {160, 180, 128, 120, 128, 120}, {40, 255, 0, 50, 255, 200},
     {255, 40, 128, 0, 128, 160}, {220, 100, 255, 255, 0, 140}, {30, 60, 128, 0, 128, 255}, {255, 255, 0, 220, 255, 100}},
    /* voice: pitch, size of the mouth, slowness */
    {{128, 128, 128, 0, 0, 0}, {60, 40, 150, 0, 0, 0}, {200, 230, 110, 0, 0, 0}, {128, 128, 40, 0, 0, 0},
     {90, 100, 230, 0, 0, 0}, {255, 200, 128, 0, 0, 0}, {20, 128, 128, 0, 0, 0}, {180, 128, 70, 0, 0, 0},
     {40, 0, 200, 0, 0, 0}, {230, 255, 90, 0, 0, 0}, {128, 60, 128, 0, 0, 0}, {128, 200, 128, 0, 0, 0},
     {200, 128, 220, 0, 0, 0}, {70, 128, 50, 0, 0, 0}, {160, 255, 60, 0, 0, 0}, {110, 80, 255, 0, 0, 0}},
};

/* ---- sound: everything from here to "editing" runs in the audio interrupt ---- */
static volatile float param[TRACKS][SETTINGS_N];           /* the sounds in use, 0..1 */
static uint8_t sound_of[TRACKS];                            /* which one each track started from */
static volatile uint32_t audio_blocks;
static volatile uint8_t fired;              /* bit per track: it just played (for the lights) */
static volatile uint8_t mute;
static volatile bool playing = true;
static volatile int pattern, queued = -1;
static volatile float step_samples = 3809.0f, swing;

static void use_sound(int track, int n) {
    sound_of[track] = (uint8_t)n;
    for (int i = 0; i < SETTINGS_N; i++) param[track][i] = (float)SOUND[track][n][i] * (1.0f / 255.0f);
}

static struct { float ph, bend, amp, decay, click; } kick;
static struct { float ph, noise, tone, last, decay; } snare;
static struct { float amp, last, last2, decay, m1, m2; } hat;
static struct {
    jt_osc_t osc, sub;
    jt_svf_t filter;
    float hz, goal, env, amp, level;
    int gate;                               /* samples until the note is let go */
    bool tied;
} bass = {.osc = {.wave = JT_WAVE_SAW}, .sub = {.wave = JT_WAVE_SQUARE}};
static struct { jt_osc_t a, b; float env, decay, lp, level; } lead = {.a = {.wave = JT_WAVE_SAW}, .b = {.wave = JT_WAVE_SQUARE}};
static jt_lpc_t vox;
static struct { float left; int step; } seq = {.step = -1};
static int16_t tape_buf[TAPE];
static jt_tape_t tape;

static float note_of(int degree, int root) {
    static const uint8_t MINOR[7] = {0, 2, 3, 5, 7, 8, 10};
    int d = degree - 1;
    return (float)(root + 12 * (d / 7) + MINOR[d % 7]);
}

static float fall(float seconds) { return jt_exp2(-10.0f / (seconds * SR)); }  /* per-sample factor: 60 dB in that time */

static void trigger(int track, uint8_t v, float step_len) {
    static const float LEVEL[4] = {0.0f, 0.45f, 0.8f, 1.0f};
    switch (track) {
    case KICK:
        kick.amp = LEVEL[v & 3];
        kick.bend = 1.0f;
        kick.ph = 0.0f;
        kick.click = param[KICK][5] * LEVEL[v & 3];
        kick.decay = fall(0.05f + 0.85f * param[KICK][1]);
        break;
    case SNARE:
        snare.noise = snare.tone = LEVEL[v & 3];
        snare.ph = 0.0f;
        snare.decay = fall(0.04f + 0.5f * param[SNARE][1]);
        break;
    case HAT:
        hat.amp = LEVEL[v & 3];
        hat.decay = fall((v & 3) == 3 ? 0.08f + 0.7f * param[HAT][2] : 0.015f + 0.1f * param[HAT][1]);
        break;
    case BASS:
        bass.goal = jt_note_hz(note_of(v & DEGREE, 33));
        bass.level = v & ACCENT ? 1.0f : 0.7f;
        if (!bass.tied) { bass.hz = bass.goal; bass.env = v & ACCENT ? 1.0f : 0.65f; }
        bass.tied = v & SLIDE;
        bass.gate = (int)(step_len * (bass.tied ? 1.3f : 0.6f));
        break;
    case LEAD: {
        float octave = param[LEAD][4];
        float hz = jt_note_hz(note_of(v & DEGREE, octave < 0.33f ? 45 : octave < 0.66f ? 57 : 69));
        float detune = 0.012f * param[LEAD][3];
        jt_osc_hz(&lead.a, hz * (1.0f + detune), SR);
        jt_osc_hz(&lead.b, hz * (1.0f - detune), SR);
        lead.env = 1.0f;
        lead.level = v & ACCENT ? 1.0f : 0.7f;
        lead.decay = fall((v & SLIDE ? 0.5f : 0.08f) + 0.6f * param[LEAD][0]);
        break;
    }
    default:
        jt_lpc_say(&vox, &WORD_DATA[WORD_AT[(v - 1) % WORD_COUNT]]);
    }
    fired |= (uint8_t)(1u << track);
}

static void next_step(void) {
    seq.step = (seq.step + 1) & (STEPS - 1);
    if (seq.step == 0 && queued >= 0) { pattern = queued; queued = -1; }
    float len = step_samples * (seq.step & 1 ? 1.0f - swing : 1.0f + swing);
    seq.left += len;
    for (int t = 0; t < TRACKS; t++) {
        uint8_t v = pat[pattern][t][seq.step];
        if (v && !(mute >> t & 1)) trigger(t, v, len);
    }
}

static void render(int16_t *out, int n) {
    static float wet_now, wet_prev, half_send;
    static int alt;
    audio_blocks++;

    /* once per block: settings that move slowly */
    float kick_hz = 30.0f + 70.0f * param[KICK][0], kick_drop = 8.0f * param[KICK][2];
    float kick_fall = fall(0.004f + 0.08f * param[KICK][3]), kick_drive = 1.0f + 4.0f * param[KICK][4];
    float snare_mix = param[SNARE][0], snare_hz = 140.0f + 200.0f * param[SNARE][2];
    float snare_bright = param[SNARE][3], snare_snap = fall(0.02f + 0.2f * param[SNARE][4]);
    float hat_tone = param[HAT][0], hat_metal = param[HAT][3];
    float wave = param[BASS][2];
    bass.osc.wave = wave >= 0.33f && wave < 0.66f ? JT_WAVE_SQUARE : JT_WAVE_SAW;
    float sub = wave >= 0.66f ? 0.6f : 0.0f;
    float cutoff = 70.0f * jt_exp2(5.0f * param[BASS][0] + bass.env * (1.0f + 4.0f * param[BASS][1]));
    jt_svf_set(&bass.filter, cutoff > 9000.0f ? 9000.0f : cutoff, 0.25f + 0.7f * param[BASS][1], SR);
    float bass_fall = fall(0.05f + 0.6f * param[BASS][3]), bass_drive = 1.0f + 3.0f * param[BASS][4];
    float glide = 0.0005f + 0.006f * (1.0f - param[BASS][5]);
    wave = param[LEAD][2];
    lead.a.wave = wave >= 0.33f && wave < 0.66f ? JT_WAVE_SQUARE : JT_WAVE_SAW;
    lead.b.wave = wave >= 0.66f ? JT_WAVE_SAW : JT_WAVE_SQUARE;
    float send = param[LEAD][1], lead_open = 0.02f + 0.3f * param[LEAD][5];
    vox.hz = jt_note_hz(45.0f + (param[VOX][0] - 0.5f) * 24.0f);
    vox.ratio = jt_exp2((param[VOX][1] - 0.5f) * 1.2f);
    vox.speed = jt_exp2((param[VOX][2] - 0.5f) * 2.0f);
    jt_lpc_block(&vox);
    float echo = step_samples * 1.5f;                       /* three steps, at the effects' 16 kHz */
    if (echo > (float)(TAPE - 2)) echo = step_samples;
    if (echo > (float)(TAPE - 2)) echo = (float)(TAPE - 2);

    for (int i = 0; i < n; i++) {
        if (playing) {
            seq.left -= 1.0f;
            if (seq.left <= 0.0f) next_step();
        }
        float white = jt_rand();

        /* kick: a sine that drops in pitch, a click on top, driven */
        kick.bend *= kick_fall;
        kick.ph += kick_hz * (1.0f + kick_drop * kick.bend) * (1.0f / SR);
        if (kick.ph >= 1.0f) kick.ph -= 1.0f;
        float k = jt_softclip(jt_sin(kick.ph) * kick.amp * kick_drive) + white * kick.click * 0.4f;
        kick.amp *= kick.decay;
        kick.click *= 0.985f;

        /* snare: noise and a short tone */
        snare.ph += snare_hz * (1.0f + snare.tone) * (1.0f / SR);
        if (snare.ph >= 1.0f) snare.ph -= 1.0f;
        float sn = (white - snare_bright * snare.last) * snare.noise * (0.35f + 0.65f * snare_mix)
                 + jt_sin(snare.ph) * snare.tone * (1.0f - 0.7f * snare_mix);
        snare.last = white;
        snare.noise *= snare.decay;
        snare.tone *= snare_snap;

        /* hat: noise, or two clashing square waves for metal, with the low end taken out twice */
        hat.m1 += 5417.0f / SR;
        if (hat.m1 >= 1.0f) hat.m1 -= 1.0f;
        hat.m2 += 8161.0f / SR;
        if (hat.m2 >= 1.0f) hat.m2 -= 1.0f;
        float src = (hat.m1 < 0.5f) == (hat.m2 < 0.5f) ? 0.7f : -0.7f;
        src = white + (src - white) * hat_metal;
        float h1 = src - hat.last, h2 = h1 - hat.last2;
        hat.last = src;
        hat.last2 = h1;
        float h = (h1 + (h2 - h1) * hat_tone) * hat.amp;
        hat.amp *= hat.decay;

        /* bass: a wave into a resonant filter that the envelope opens */
        if (bass.gate > 0) bass.gate--;
        bass.hz += (bass.goal - bass.hz) * glide;
        bass.osc.inc = bass.hz * (1.0f / SR);
        bass.sub.inc = bass.osc.inc * 0.5f;
        bass.amp += ((bass.gate > 0 ? bass.level : 0.0f) - bass.amp) * 0.006f;
        bass.env *= bass_fall;
        float raw = jt_osc_next(&bass.osc) + sub * jt_osc_next(&bass.sub);
        float b = jt_softclip(jt_svf_next(&bass.filter, raw) * bass_drive) * bass.amp;
        b *= 1.0f - 0.55f * (kick.amp > 1.0f ? 1.0f : kick.amp);       /* the kick pushes the bass down */

        /* lead: two detuned waves, a pluck */
        float l = 0.5f * (jt_osc_next(&lead.a) + jt_osc_next(&lead.b));
        lead.lp += (l - lead.lp) * (lead_open + 0.5f * lead.env);
        l = lead.lp * lead.env * lead.level;
        lead.env *= lead.decay;

        float v = jt_lpc_next(&vox) * 1.6f;

        float dry = 0.7f * k + 0.7f * sn + 0.3f * h + 0.45f * b + 0.4f * l + v;
        float to_echo = l * send + v * 0.35f + sn * 0.08f;
        float y;
        if (alt ^= 1) {                                     /* the echo runs on every second sample */
            half_send = to_echo;
            y = dry + 0.5f * (wet_prev + wet_now);
        } else {
            wet_prev = wet_now;
            float d = jt_tape_read(&tape, echo);
            jt_tape_write(&tape, 0.5f * (half_send + to_echo) + d * 0.45f);
            wet_now = d * 0.6f;
            y = dry + wet_prev;
        }
        out[i] = (int16_t)(jt_softclip(y * 0.75f) * 30000.0f);
    }
}

/* ---- editing ---- */
static struct { uint8_t pattern, track, step, old; } undo[32];
static int undo_n;

static void change(int track, int step, uint8_t v) {
    if (undo_n == 32) { for (int i = 1; i < 32; i++) undo[i - 1] = undo[i]; undo_n--; }
    undo[undo_n].pattern = (uint8_t)pattern;
    undo[undo_n].track = (uint8_t)track;
    undo[undo_n].step = (uint8_t)step;
    undo[undo_n].old = pat[pattern][track][step];
    undo_n++;
    pat[pattern][track][step] = v;
}

static int dice(int n) { return (int)((jt_rand() + 1.0f) * 0.4999f * (float)n); }   /* 0..n-1 */

/* one small random change to a track */
static void evolve(int track) {
    static const uint8_t NICE[6] = {1, 3, 5, 8, 10, 12};
    int s = dice(STEPS);
    uint8_t v = pat[pattern][track][s];
    if (track <= HAT) {
        if (track == KICK && !(s & 3) && v) return;         /* the four on the floor stays */
        change(track, s, v ? 0 : (uint8_t)(1 + dice(3)));
    } else if (track == VOX) {
        change(track, s, v ? 0 : (uint8_t)(1 + dice(WORD_COUNT)));
    } else if (!v) {
        change(track, s, NICE[dice(6)]);
    } else {
        int kind = dice(5), d = v & DEGREE;
        if (kind == 0) change(track, s, 0);
        else if (kind == 1) change(track, s, v ^ ACCENT);
        else if (kind == 2) change(track, s, v ^ SLIDE);
        else {
            d += dice(5) - 2;
            change(track, s, (uint8_t)((v & ~DEGREE) | (d < 1 ? 1 : d > DEGREES ? DEGREES : d)));
        }
    }
}

static void revert(void) {
    if (!undo_n) return;
    undo_n--;
    pat[undo[undo_n].pattern][undo[undo_n].track][undo[undo_n].step] = undo[undo_n].old;
}

/* ---- display ---- */
static void block(int x, int y, int w, int h) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) jt_oled_pixel(i, j, true);
}
static void frame(int x, int y, int w, int h) {
    for (int i = x; i < x + w; i++) { jt_oled_pixel(i, y, true); jt_oled_pixel(i, y + h - 1, true); }
    for (int j = y; j < y + h; j++) { jt_oled_pixel(x, j, true); jt_oled_pixel(x + w - 1, j, true); }
}
static char *number(char *p, int v) {
    if (v >= 100) *p++ = (char)('0' + v / 100);
    if (v >= 10) *p++ = (char)('0' + v / 10 % 10);
    *p++ = (char)('0' + v % 10);
    return p;
}

/* Top: all six tracks, a row each. Bottom: the selected track, a box per step. */
static void draw(int sel, uint16_t touching) {
    for (int t = 0; t < TRACKS; t++)
        for (int s = 0; s < STEPS; s++) {
            uint8_t v = pat[pattern][t][s];
            if (v) block(s * 8 + 1, t * 3, 6, mute >> t & 1 ? 1 : 2);
            else if (t == sel) jt_oled_pixel(s * 8 + 4, t * 3, true);
        }
    if (playing && seq.step >= 0) block(seq.step * 8 + 1, 18, 6, 2);
    for (int s = 0; s < STEPS; s++) {
        uint8_t v = pat[pattern][sel][s];
        int x = s * 8;
        if (sel == VOX) {
            char one[2] = {v ? WORD_NAMES[(v - 1) % WORD_COUNT][0] : 0, 0};
            if (v) jt_oled_text(x, 23, one);
            else jt_oled_pixel(x + 3, 31, true);
        } else {
            frame(x, 21, 7, 11);
            if (sel <= HAT) {
                if (v) block(x + 1, 31 - 3 * (v & 3), 5, 3 * (v & 3));
            } else if (v) {
                int h = 1 + ((v & DEGREE) - 1) * 8 / (DEGREES - 1);
                block(x + 1, 31 - h, 5, h);
                if (v & ACCENT) block(x + 1, 22, 2, 2);
                if (v & SLIDE) block(x + 5, 22, 3, 1);
            }
        }
        if (touching >> (15 - s) & 1) block(x, 20, 7, 1);
    }
}

static void draw_info(int bpm) {
    char line[17], *p = line;
    const char *a = "PATTERN ";
    while (*a) *p++ = *a++;
    p = number(p, (queued >= 0 ? queued : pattern) + 1);
    *p = 0;
    jt_oled_text(0, 0, line);
    p = number(line, bpm);
    a = " BPM SWING ";
    while (*a) *p++ = *a++;
    p = number(p, (int)(swing * 200.0f + 0.5f));
    *p = 0;
    jt_oled_text(0, 12, line);
    static const char NAMES[TRACKS] = {'K', 'S', 'H', 'B', 'L', 'V'};
    for (int t = 0; t < TRACKS; t++) {
        char one[2] = {NAMES[t], 0};
        if (mute >> t & 1) jt_oled_pixel(t * 20 + 3, 28, true);
        else jt_oled_text(t * 20, 24, one);
    }
}

static const char *const NAMES[TRACKS] = {"KICK", "SNARE", "HAT", "BASS", "LEAD", "VOICE"};
#define CLEAR_MS 1000

/* Clearing a track: "CLEAR KICK?" and a bar that empties while SELECT stays down, then "KICK CLEARED". */
static void draw_clear(int track, int held_ms, bool done) {
    char line[17], *p = line;
    const char *a = done ? NAMES[track] : "CLEAR ";
    while (*a) *p++ = *a++;
    a = done ? " CLEARED" : NAMES[track];
    while (*a) *p++ = *a++;
    if (!done) *p++ = '?';
    *p = 0;
    jt_oled_text((JT_OLED_W - 8 * (int)(p - line)) / 2, done ? 12 : 2, line);
    if (done) return;
    jt_oled_text(20, 13, "KEEP HOLDING");
    frame(0, 24, JT_OLED_W, 8);
    block(2, 26, (JT_OLED_W - 4) * (CLEAR_MS - held_ms) / CLEAR_MS, 4);
}

/* While a track button is held: its name, which of its sounds, and the two knob settings. */
static void draw_sound(int track) {
    char num[3], *p = number(num, sound_of[track] + 1);
    *p = 0;
    jt_oled_text_big(0, 0, NAMES[track], 2);
    jt_oled_text_big(JT_OLED_W - 16 * (int)(p - num), 0, num, 2);
    for (int i = 0; i < 2; i++) {
        frame(0, 19 + 7 * i, JT_OLED_W, 6);
        block(2, 21 + 7 * i, (int)(param[track][i] * (float)(JT_OLED_W - 4)), 2);
    }
}

/* bit n = track button n, bit 6 = SELECT pressed; debounced as in examples/speak */
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
    int sel = BASS, bpm = 126, quiet = 0, last_mod = -1;
    int anchor[2], owns[2] = {1, 1};        /* at power-on the knobs are the filter, as they stand */
    float filter[2] = {1.0f, 0.0f};
    uint16_t last_bits = 0, stable = 0, touched = 0, made = 0, edited = 0;
    uint8_t held = 0, leds = 0xFF, fresh[TRACKS] = {2, 2, 2, 1, 8, 1};
    uint32_t led_until[TRACKS] = {0}, info_until = 0, select_since = 0;
    int select_mode = 0;                    /* 0 up, 1 held as shift (a tap if nothing follows), 2 used up */
    bool shift_acted = false;
    uint8_t as_mute = 0;
    int clearing = -1, cleared = -1;        /* the track being counted down, and the one just cleared */
    uint32_t clear_since = 0, cleared_until = 0;
    int32_t enc_last;

    jt_init();
    factory();
    for (int i = 0; i < TRACKS; i++) use_sound(i, 0);
    jt_tape_init(&tape, tape_buf, TAPE);
    jt_lpc_init(&vox, LPC_SIN, LPC_GAIN);
    jt_audio_init(render);
    jt_usb_init();

    for (int i = 0; i < 6; i++) { held = buttons(); jt_delay_ms(8); }
    enc_last = jt_encoder();
    anchor[0] = jt_pot(JT_ADC_CUTOFF);
    anchor[1] = jt_pot(JT_ADC_RESONANCE);

    for (;;) {
        jt_delay_ms(8);
        jt_poll_escape();
        uint32_t t = audio_blocks * 2;
        uint8_t now = buttons(), pressed = (uint8_t)(now & ~held), released = (uint8_t)(held & ~now);
        held = now;
        bool escape = (now & 1) && (now & 32);

        /* keyboard: two agreeing readings to change, a few empty ones to let go */
        uint16_t bits = jt_keyboard();
        if (bits && bits == last_bits) { stable = bits; quiet = 0; }
        else if (!bits && ++quiet >= 3) stable = 0;
        last_bits = bits;
        uint16_t down = (uint16_t)(stable & ~touched), up = (uint16_t)(touched & ~stable);
        touched = stable;

        /* A held track button turns the knobs and SELECT into that track's. Held SELECT is
         * a shift key; a track button pressed during it is a mute, not a hold. */
        bool shift = select_mode == 1 && (now & 64);
        if (!escape)
            for (int i = 0; i < TRACKS; i++)
                if (pressed >> i & 1) {
                    if (shift) { mute ^= (uint8_t)(1u << i); as_mute |= (uint8_t)(1u << i); shift_acted = true; info_until = t + 900; }
                    else sel = i;
                }
        as_mute &= now;
        int mod = -1;
        for (int i = TRACKS - 1; i >= 0; i--)
            if ((now >> i & 1) && !(as_mute >> i & 1) && !escape) mod = i;

        if (pressed & 64) {
            select_mode = 2;                                /* used up, unless nothing else is held */
            if (mod >= 0) {                                 /* a track held: start the countdown to clear it */
                clearing = mod;
                clear_since = t;
            } else if (stable) {                            /* steps held: accent, slide, both, neither */
                for (int s = 0; s < STEPS; s++) {
                    uint8_t v = pat[pattern][sel][s];
                    if (!(stable >> (15 - s) & 1) || !v || sel < BASS || sel > LEAD) continue;
                    pat[pattern][sel][s] = (uint8_t)((v & DEGREE) | (((v >> 6) + 1) & 3) << 6);
                }
                edited |= stable;
            } else {
                select_mode = 1;
                select_since = t;
                shift_acted = false;
            }
        }
        if (released & 64) {
            if (select_mode == 1 && !shift_acted && t - select_since < 500) {
                playing = !playing;
                if (playing) { seq.step = -1; seq.left = 0.0f; }
            }
            select_mode = 0;
        }
        shift = select_mode == 1 && (now & 64);
        if (shift && t - select_since > 250) info_until = t + 200;

        /* clearing a track: SELECT has to stay down for a second, with the track still held */
        if (clearing >= 0) {
            if (!(now & 64) || mod != clearing) clearing = -1;
            else if (t - clear_since >= CLEAR_MS) {
                for (int s = 0; s < STEPS; s++) if (pat[pattern][clearing][s]) change(clearing, s, 0);
                cleared = clearing;
                cleared_until = t + 800;
                clearing = -1;
            }
        }

        /* SELECT turn */
        int steps = (jt_encoder() - enc_last) / 8;
        if (steps) {
            enc_last += steps * 8;
            if (stable && !shift) {                         /* held steps: their value */
                for (int s = 0; s < STEPS; s++) {
                    uint8_t v = pat[pattern][sel][s];
                    if (!(stable >> (15 - s) & 1) || !v) continue;
                    if (sel <= HAT) { int n = v + steps; v = (uint8_t)(n < 1 ? 1 : n > 3 ? 3 : n); }
                    else if (sel == VOX) v = (uint8_t)(((v - 1 + steps) % WORD_COUNT + WORD_COUNT) % WORD_COUNT + 1);
                    else { int d = (v & DEGREE) + steps; v = (uint8_t)((v & ~DEGREE) | (d < 1 ? 1 : d > DEGREES ? DEGREES : d)); }
                    pat[pattern][sel][s] = v;
                    fresh[sel] = sel >= BASS && sel <= LEAD ? v & DEGREE : v;
                }
                edited |= stable;
            } else if (mod >= 0) {                          /* a track held: evolve it, or take it back */
                for (int i = 0; i < steps; i++) evolve(mod);
                for (int i = 0; i > steps; i--) revert();
            } else if (shift) {
                float w = swing + 0.025f * (float)steps;
                swing = w < 0.0f ? 0.0f : w > 0.4f ? 0.4f : w;
                shift_acted = true;
                info_until = t + 900;
            } else {
                bpm += steps;
                bpm = bpm < 60 ? 60 : bpm > 200 ? 200 : bpm;
                step_samples = SR * 15.0f / (float)bpm;
                info_until = t + 900;
            }
        }

        /* keys: with SELECT held, a pattern; with a track held, a sound; otherwise the track's steps */
        if (shift) {
            int k = jt_keyboard_lowest(down);
            if (k >= 0) { queued = k; shift_acted = true; info_until = t + 900; }
            edited |= stable;                               /* letting go must not remove a step */
        } else if (mod >= 0) {                              /* a track held: one of its 16 sounds */
            int k = jt_keyboard_lowest(down);
            if (k >= 0) { use_sound(mod, k); last_mod = -2; }   /* -2: the knobs wait to be moved again */
            edited |= stable;
        } else {
            for (int s = 0; s < STEPS; s++) {
                uint16_t b = JT_KEYBOARD_BIT(s);
                if ((down & b) && !pat[pattern][sel][s]) { change(sel, s, fresh[sel]); made |= b; }
                if ((up & b) && !(made & b) && !(edited & b) && pat[pattern][sel][s]) change(sel, s, 0);
            }
        }
        made &= stable;
        edited &= stable;

        /* knobs: the filter, or the held track's sound; each waits until it is moved */
        if (mod != last_mod) {
            last_mod = mod;
            anchor[0] = jt_pot(JT_ADC_CUTOFF);
            anchor[1] = jt_pot(JT_ADC_RESONANCE);
            owns[0] = owns[1] = 0;
        }
        for (int i = 0; i < 2; i++) {
            int raw = jt_pot(i == 0 ? JT_ADC_CUTOFF : JT_ADC_RESONANCE), d = raw - anchor[i];
            if (!owns[i] && (d > 90 || d < -90)) owns[i] = 1;
            if (!owns[i]) continue;
            float v = raw >= 4000 ? 1.0f : (float)raw * (1.0f / 4000.0f);
            if (mod >= 0) param[mod][i] = v;
            else filter[i] = v;
        }
        jt_filter((uint16_t)(filter[0] * 1000.0f), (uint16_t)(filter[1] * 1000.0f));

        /* lights: each track's button flashes when it plays */
        uint8_t f = fired, want = 0;
        fired = 0;
        for (int i = 0; i < TRACKS; i++) {
            if (f >> i & 1) led_until[i] = t + 60;
            if (t < led_until[i]) want |= (uint8_t)(1u << i);
        }
        if (want != leds) { leds = want; jt_leds(leds); }

        jt_oled_clear();
        if (clearing >= 0) draw_clear(clearing, (int)(t - clear_since), false);
        else if (t < cleared_until) draw_clear(cleared, 0, true);
        else if (mod >= 0) draw_sound(mod);
        else if (t < info_until) draw_info(bpm);
        else draw(sel, stable);
        jt_oled_flush();
    }
}
