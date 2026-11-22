/* Acid: a bassline machine in the manner of the TB-303, with a kick and a hat to play against.
 *
 * One bass line, 16 steps on the 16 keys. The six buttons choose which part of a step the
 * keys edit:
 *
 *   NOTE (OSC)      touch: a note here or not.  Hold the step and turn SELECT: its pitch
 *   ACCENT (LFO)    touch: accent on / off
 *   SLIDE (FILTER)  touch: slide into the next note on / off
 *   OCTAVE (AMP)    touch: normal, up, down
 *   KICK (ARP)      touch: a kick here or not.  Hold and turn: soft, normal, accent
 *   HAT (SETTINGS)  touch: a hat here or not.   Hold and turn: soft, normal, open
 *
 *   knobs              CUTOFF and RESONANCE of the bass filter
 *   hold a button and  turn a knob: OSC tune, wave.  LFO env mod, decay.  FILTER accent, slide time.
 *                      AMP drive, echo.  ARP kick tune, length.  SETTINGS hat tone, length.
 *                      turn SELECT: right changes that part of the line at random, left undoes it
 *                      hold SELECT for a second: clear that part
 *   hold OSC and       touch a key: one of 16 bass sounds
 *   SELECT             tap: play / stop.   Turn: tempo.
 *   hold SELECT and    touch a key: go to that pattern at the end of the bar
 *                      turn: swing
 *                      press FILTER: analog filter / digital filter.  ARP, SETTINGS: mute kick, hat
 *
 * Two filters: a digital four-pole ladder with the envelope applied sample by sample, or
 * the synth's own analog filter, driven by the same envelope (everything passes through
 * it then, drums included).
 */
#include "jt4000.h"
#include "jt_dsp.h"

enum { NOTE, ACC, SLD, OCT, KICK, HAT, LAYERS };
#define STEPS 16
#define PATTERNS 16
#define SR ((float)JT_SAMPLE_RATE)
#define TAPE 8192                           /* 0.51 s of echo at 16 kHz */

/* a step of the line */
#define GATE 0x10                           /* flags: a note sounds here */
#define ACCENT 0x20
#define SLIDE 0x40
#define UP 0x01                             /* octave: up, down */
#define DOWN 0x02
typedef struct { uint8_t note, flags, octave, kick, hat; } step_t;     /* note 0..12 semitones above C */
static step_t pat[PATTERNS][STEPS];

/* ---- factory lines: pitch letter per step (c d e f g a b, upper case = sharp, C' = top C as 'x',
 *      . = rest), then accents, slides and octaves as marks under the same steps ---- */
static const struct { const char *note, *accent, *slide, *octave, *kick, *hat; } FACTORY[] = {
    {"cc.cDc.ccGc.Acd.", "..x...x..x....x.", "....x....x......", "..^....^..^.....", "x...x...x...x...", "..x...x...x...X."},
    {"c.cc.Dcc.cgc.fDc", "x.....x...x....x", "...x.....x..x...", ".^..v......^....", "x...x...x...x...", "oxoxoxoXoxoxoxoX"},
    {"aaaa.aga.aaC.aea", "...x....x..x....", "..x...x......x..", "....^......^..v.", "x...x...x...x.o.", "..X...X...X...X."},
    {"c.D.f.c.c.A.g.D.", "x.......x.x.....", "..x.......x...x.", "......^.....v...", "x..x..x...x..x..", "x.x.x.x.x.x.x.xX"},
    {"ccccDDffggffDDcc", "x...x...x...x...", ".x.x.x.x.x.x.x.x", "........^^......", "x...x...x...x...", "..x...x...x...x."},
    {"e.eE.eb.e.ed.egA", "..x....x...x...x", "...x......x...x.", ".^....v...^.....", "x.....x...x.....", "x.o.x.o.x.o.x.oX"},
    {"f.ff.fGf.fCf.fDf", "x..x..x..x..x..x", ".....x.....x....", "..^.....^.....v.", "x...x...x...x...", "oxoXoxoXoxoXoxoX"},
    {"d.d.d.dd.d.dFdad", "......x.....x.x.", "......x.......x.", "..v...^...v.....", "x...x...x...x..x", "..x...x...x.X.x."},
};

static void factory(void) {
    static const char LETTERS[] = "cCdDefFgGaAbx";
    for (unsigned p = 0; p < sizeof FACTORY / sizeof FACTORY[0]; p++)
        for (int s = 0; s < STEPS; s++) {
            step_t *st = &pat[p][s];
            for (int i = 0; i < 13; i++)
                if (FACTORY[p].note[s] == LETTERS[i]) { st->note = (uint8_t)i; st->flags = GATE; }
            if (FACTORY[p].accent[s] == 'x') st->flags |= ACCENT;
            if (FACTORY[p].slide[s] == 'x') st->flags |= SLIDE;
            st->octave = FACTORY[p].octave[s] == '^' ? UP : FACTORY[p].octave[s] == 'v' ? DOWN : 0;
            char k = FACTORY[p].kick[s], h = FACTORY[p].hat[s];
            st->kick = k == 'o' ? 1 : k == 'x' ? 2 : k == 'X' ? 3 : 0;
            st->hat = h == 'o' ? 1 : h == 'x' ? 2 : h == 'X' ? 3 : 0;
        }
}

/* ---- settings: 0..1 each. The first ten are a bass sound. ---- */
enum { TUNE, WAVE, CUTOFF, RESO, ENVMOD, DECAY, ACCENT_AMT, SLIDE_TIME, DRIVE, ECHO,
       KICK_TUNE, KICK_LEN, HAT_TONE, HAT_LEN, PARAMS };
#define SOUND_PARAMS 10
static volatile float P[PARAMS] = {[KICK_TUNE] = 0.4f, [KICK_LEN] = 0.4f, [HAT_TONE] = 0.5f, [HAT_LEN] = 0.4f};
/* which two settings the knobs turn: with nothing held, then with each button held */
static const uint8_t KNOBS[LAYERS + 1][2] = {{CUTOFF, RESO}, {TUNE, WAVE}, {ENVMOD, DECAY}, {ACCENT_AMT, SLIDE_TIME},
                                             {DRIVE, ECHO}, {KICK_TUNE, KICK_LEN}, {HAT_TONE, HAT_LEN}};
static const char *const KNOB_NAMES[LAYERS][2] = {{"TUNE", "WAVE"}, {"ENV MOD", "DECAY"}, {"ACCENT", "SLIDE TIME"},
                                                  {"DRIVE", "ECHO"}, {"KICK TUNE", "KICK LENGTH"}, {"HAT TONE", "HAT LENGTH"}};

#define SOUNDS 16
static const uint8_t SOUND[SOUNDS][SOUND_PARAMS] = {
    /* tune wave cutoff reso envmod decay accent slide drive echo */
    {128, 0, 70, 190, 150, 90, 180, 110, 90, 60},           /* 1  the classic: saw, squelchy */
    {128, 255, 60, 200, 160, 70, 190, 110, 100, 60},        /* 2  square, hollow */
    {128, 0, 40, 235, 200, 140, 220, 140, 60, 90},          /* 3  screaming resonance */
    {128, 0, 110, 120, 90, 50, 120, 90, 230, 30},           /* 4  distorted and open */
    {128, 255, 30, 170, 230, 30, 200, 110, 150, 80},        /* 5  short square blips */
    {128, 0, 20, 220, 120, 220, 255, 200, 40, 120},         /* 6  long slow sweeps */
    {68, 0, 90, 150, 130, 100, 160, 110, 180, 40},          /* 7  an octave down, heavy */
    {188, 255, 80, 210, 170, 60, 200, 70, 70, 150},         /* 8  an octave up, echoing */
    {128, 0, 150, 60, 60, 80, 100, 110, 20, 40},            /* 9  mellow, barely filtered */
    {128, 0, 55, 250, 255, 45, 255, 110, 130, 70},          /* 10 everything turned up */
    {128, 255, 100, 100, 110, 170, 140, 240, 200, 50},      /* 11 rubbery slides */
    {128, 0, 30, 140, 255, 15, 180, 60, 255, 20},           /* 12 hard plucks, fuzz */
    {128, 0, 80, 230, 60, 255, 120, 150, 0, 180},           /* 13 whistling, clean, wet */
    {98, 255, 45, 180, 190, 110, 230, 130, 160, 70},        /* 14 low square growl */
    {128, 0, 200, 180, 40, 100, 255, 110, 110, 60},         /* 15 bright, accents bite */
    {128, 255, 10, 255, 140, 255, 150, 255, 30, 200},       /* 16 the filter sings by itself */
};

/* ---- sound: everything from here to "editing" runs in the audio interrupt ---- */
static volatile uint32_t audio_blocks;
static volatile uint8_t fired;              /* bit 0 bass, 1 accent, 4 kick, 5 hat: for the lights */
static volatile uint8_t mute;               /* bit 4 kick, bit 5 hat */
static volatile bool playing = true, analog;
static volatile int pattern, queued = -1;
static volatile float step_samples = 3809.0f, swing;

static struct {
    jt_osc_t osc;
    float hz, goal, env, amp, level, wow, env_fall;
    float z[4], g, g_goal;                  /* the ladder filter */
    int gate;
    bool tied, accent;
} bass = {.osc = {.wave = JT_WAVE_SAW}};
static struct { float ph, bend, amp, decay; } kick;
static struct { float amp, last, last2, decay; } hat;
static struct { float left; int step; } seq = {.step = -1};
static int16_t tape_buf[TAPE];
static jt_tape_t tape;

static float fall(float seconds) { return jt_exp2(-10.0f / (seconds * SR)); }  /* per-sample factor: 60 dB in that time */

static void next_step(void) {
    static const float LEVEL[4] = {0.0f, 0.45f, 0.8f, 1.0f};
    seq.step = (seq.step + 1) & (STEPS - 1);
    if (seq.step == 0 && queued >= 0) { pattern = queued; queued = -1; }
    float len = step_samples * (seq.step & 1 ? 1.0f - swing : 1.0f + swing);
    seq.left += len;
    const step_t *st = &pat[pattern][seq.step];

    if (st->flags & GATE) {
        int note = 36 + st->note + (st->octave & UP ? 12 : 0) - (st->octave & DOWN ? 12 : 0);
        bass.goal = jt_note_hz((float)note + (P[TUNE] - 0.5f) * 24.0f);
        bass.accent = st->flags & ACCENT;
        bass.level = bass.accent ? 1.0f : 0.62f;
        if (!bass.tied) {                                   /* a new attack: the envelope starts again */
            bass.hz = bass.goal;
            bass.env = 1.0f;
            /* an accented note has a short, fixed decay, as in the original circuit */
            bass.env_fall = fall(bass.accent ? 0.2f : 0.2f + 2.3f * P[DECAY] * P[DECAY]);
        }
        if (bass.accent) bass.wow += (1.0f - bass.wow) * 0.6f;      /* accents in a row pile up */
        bass.tied = st->flags & SLIDE;
        bass.gate = (int)(len * (bass.tied ? 1.4f : 0.55f));
        fired |= bass.accent ? 3 : 1;
    }
    if (st->kick && !(mute & 16)) {
        kick.amp = LEVEL[st->kick & 3];
        kick.bend = 1.0f;
        kick.ph = 0.0f;
        kick.decay = fall(0.06f + 0.6f * P[KICK_LEN]);
        fired |= 16;
    }
    if (st->hat && !(mute & 32)) {
        hat.amp = LEVEL[st->hat & 3];
        hat.decay = fall(st->hat == 3 ? 0.1f + 0.5f * P[HAT_LEN] : 0.015f + 0.1f * P[HAT_LEN]);
        fired |= 32;
    }
}

/* tan(x) for 0..1.1: the filter's frequency warping */
static float tangent(float x) {
    float x2 = x * x;
    return x * (1.0f + x2 * (0.33333f + x2 * (0.13333f + x2 * (0.05397f + x2 * 0.02187f))));
}

static void render(int16_t *out, int n) {
    static float wet_now, wet_prev, half_send;
    static int alt;
    audio_blocks++;

    bass.osc.wave = P[WAVE] < 0.5f ? JT_WAVE_SAW : JT_WAVE_SQUARE;
    float glide = 1.0f - fall(0.03f + 0.25f * P[SLIDE_TIME]);
    float reso = P[RESO], k = 4.1f * reso;                  /* the ladder whistles by itself near 4 */
    float drive = 1.0f + 7.0f * P[DRIVE] * P[DRIVE], send = P[ECHO];
    float kick_hz = 34.0f + 50.0f * P[KICK_TUNE], hat_tone = P[HAT_TONE];
    float echo = step_samples * 1.5f;
    if (echo > (float)(TAPE - 2)) echo = step_samples;
    if (echo > (float)(TAPE - 2)) echo = (float)(TAPE - 2);

    /* the envelope opens the filter; an accent opens it further, and more with each one in a row */
    float open = bass.env * P[ENVMOD] * 4.5f + bass.wow * P[ACCENT_AMT] * 2.2f;
    if (analog) {
        float c = P[CUTOFF] + open * 0.13f;
        jt_filter((uint16_t)((c > 1.0f ? 1.0f : c) * 1000.0f), (uint16_t)(reso * 1000.0f));
        bass.g_goal = 0.95f;                                /* the digital filter steps aside */
        k = 0.0f;
    } else {
        float hz = 45.0f * jt_exp2(6.2f * P[CUTOFF] + open);
        float g = tangent(3.14159f * (hz > 10500.0f ? 10500.0f : hz) / SR);
        bass.g_goal = g / (1.0f + g);
    }
    float g_step = (bass.g_goal - bass.g) / (float)n;

    for (int i = 0; i < n; i++) {
        if (playing) {
            seq.left -= 1.0f;
            if (seq.left <= 0.0f) next_step();
        }
        float white = jt_rand();

        /* bass: wave, four-pole ladder with the resonance fed back around it, drive */
        if (bass.gate > 0) bass.gate--;
        bass.hz += (bass.goal - bass.hz) * (bass.tied || bass.hz != bass.goal ? glide : 1.0f);
        bass.osc.inc = bass.hz * (1.0f / SR);
        bass.amp += ((bass.gate > 0 ? bass.level : 0.0f) - bass.amp) * (bass.gate > 0 ? 0.02f : 0.004f);
        bass.env *= bass.env_fall;
        bass.wow *= 0.99994f;
        bass.g += g_step;
        float G = bass.g, x = jt_osc_next(&bass.osc) * 0.6f;
        float back = (1.0f - G) * (G * G * G * bass.z[0] + G * G * bass.z[1] + G * bass.z[2] + bass.z[3]);
        float u = jt_softclip((x * (1.0f + 0.5f * k) - k * back) / (1.0f + k * G * G * G * G));
        for (int s = 0; s < 4; s++) {
            float v = G * (u - bass.z[s]);
            u = v + bass.z[s];
            bass.z[s] = u + v;
        }
        float b = jt_softclip(u * drive * (bass.accent ? 1.0f + P[ACCENT_AMT] : 1.0f)) * bass.amp;

        /* kick and hat, as in examples/groove */
        kick.bend *= 0.9985f;
        kick.ph += kick_hz * (1.0f + 4.0f * kick.bend) * (1.0f / SR);
        if (kick.ph >= 1.0f) kick.ph -= 1.0f;
        float kk = jt_softclip(jt_sin(kick.ph) * kick.amp * 1.6f);
        kick.amp *= kick.decay;
        float h1 = white - hat.last, h2 = h1 - hat.last2;
        hat.last = white;
        hat.last2 = h1;
        float h = (h1 + (h2 - h1) * hat_tone) * hat.amp;
        hat.amp *= hat.decay;

        float dry = 0.7f * kk + 0.26f * h + 0.6f * b * (1.0f - 0.4f * (kick.amp > 1.0f ? 1.0f : kick.amp));
        float y;
        if (alt ^= 1) {                                     /* the echo runs on every second sample */
            half_send = b * send;
            y = dry + 0.5f * (wet_prev + wet_now);
        } else {
            wet_prev = wet_now;
            float d = jt_tape_read(&tape, echo);
            jt_tape_write(&tape, 0.5f * (half_send + b * send) + d * 0.5f);
            wet_now = d * 0.7f;
            y = dry + wet_prev;
        }
        out[i] = (int16_t)(jt_softclip(y * 0.8f) * 30000.0f);
    }
}

/* ---- editing ---- */
static struct { uint8_t pattern, step; step_t old; } undo[32];
static int undo_n;

static step_t *edit(int step) {             /* remember a step, then hand it out for changing */
    if (undo_n == 32) { for (int i = 1; i < 32; i++) undo[i - 1] = undo[i]; undo_n--; }
    undo[undo_n].pattern = (uint8_t)pattern;
    undo[undo_n].step = (uint8_t)step;
    undo[undo_n].old = pat[pattern][step];
    undo_n++;
    return &pat[pattern][step];
}

static void revert(void) {
    if (!undo_n) return;
    undo_n--;
    pat[undo[undo_n].pattern][undo[undo_n].step] = undo[undo_n].old;
}

static int dice(int n) { return (int)((jt_rand() + 1.0f) * 0.4999f * (float)n); }   /* 0..n-1 */

static bool has(int layer, const step_t *st) {
    switch (layer) {
    case NOTE: return st->flags & GATE;
    case ACC:  return st->flags & ACCENT;
    case SLD:  return st->flags & SLIDE;
    case OCT:  return st->octave;
    case KICK: return st->kick;
    default:   return st->hat;
    }
}

/* a touch on a step: what it does depends on the layer */
static void toggle(int layer, int step) {
    step_t *st = edit(step);
    switch (layer) {
    case NOTE: st->flags ^= GATE; break;
    case ACC:  st->flags ^= ACCENT; break;
    case SLD:  st->flags ^= SLIDE; break;
    case OCT:  st->octave = st->octave == 0 ? UP : st->octave == UP ? DOWN : 0; break;
    case KICK: st->kick = st->kick ? 0 : 2; break;
    default:   st->hat = st->hat ? 0 : 2;
    }
}

/* one small random change to a layer */
static void evolve(int layer) {
    static const uint8_t NICE[10] = {0, 0, 0, 3, 5, 7, 7, 10, 12, 1};
    int s = dice(STEPS);
    step_t *st = &pat[pattern][s];
    if (layer == NOTE) {
        st = edit(s);
        if (!(st->flags & GATE) || dice(3)) { st->flags |= GATE; st->note = NICE[dice(10)]; }
        else st->flags &= (uint8_t)~GATE;
    } else if (layer == KICK) {
        if ((s & 3) || !st->kick) toggle(layer, s);         /* the four on the floor stays */
    } else if (layer == HAT || (st->flags & GATE)) {
        toggle(layer, s);
    }
}

static void clear(int layer) {
    for (int s = 0; s < STEPS; s++)
        if (has(layer, &pat[pattern][s])) {
            step_t *st = edit(s);
            if (layer == OCT) st->octave = 0;
            else if (layer == KICK) st->kick = 0;
            else if (layer == HAT) st->hat = 0;
            else st->flags &= (uint8_t)~(layer == NOTE ? GATE : layer == ACC ? ACCENT : SLIDE);
        }
}

/* ---- display ---- */
static const char *const LAYER_NAMES[LAYERS] = {"NOTES", "ACCENTS", "SLIDES", "OCTAVES", "KICK", "HAT"};
#define CLEAR_MS 1000

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
static char *text(char *p, const char *a) { while (*a) *p++ = *a++; return p; }

/* The line: accents and slides on top, the notes as bars (height is pitch), kick and hat
 * below. The layer the keys are editing shows a dot on its empty steps. */
static void draw(int layer, uint16_t touching) {
    for (int s = 0; s < STEPS; s++) {
        const step_t *st = &pat[pattern][s];
        int x = s * 8;
        if (st->flags & ACCENT) block(x + 1, 0, 6, 2);
        else if (layer == ACC) jt_oled_pixel(x + 3, 1, true);
        if (st->flags & SLIDE) block(x + 3, 3, 8, 1);
        else if (layer == SLD) jt_oled_pixel(x + 3, 3, true);
        if (st->flags & GATE) {
            int pitch = st->note + (st->octave & UP ? 12 : 0) - (st->octave & DOWN ? 12 : 0) + 12;   /* 0..36 */
            int h = 2 + pitch * 14 / 36;
            block(x + 1, 22 - h, 6, h);
        } else if (layer == NOTE || layer == OCT) {
            jt_oled_pixel(x + 3, 21, true);
        }
        if (layer == OCT && st->octave) block(x + 3, st->octave & UP ? 5 : 22, 2, 1);
        if (st->kick) block(x + 1, 24, 6, st->kick);
        else if (layer == KICK) jt_oled_pixel(x + 3, 25, true);
        if (st->hat) block(x + 1, 28, st->hat == 3 ? 6 : 3, st->hat == 1 ? 1 : 2);
        else if (layer == HAT) jt_oled_pixel(x + 3, 29, true);
        if (touching >> (15 - s) & 1) frame(x, 5, 8, 18);
    }
    if (playing && seq.step >= 0) block(seq.step * 8 + 1, 31, 6, 1);
}

static void draw_info(int bpm) {
    char line[17], *p = number(text(line, "PATTERN "), (queued >= 0 ? queued : pattern) + 1);
    *p = 0;
    jt_oled_text(0, 0, line);
    p = number(text(number(line, bpm), " BPM SWING "), (int)(swing * 200.0f + 0.5f));
    *p = 0;
    jt_oled_text(0, 12, line);
    p = text(line, analog ? "ANALOG FILTER" : "DIGITAL FILTER");
    *p = 0;
    jt_oled_text(0, 24, line);
}

/* While a button is held: the two settings the knobs now turn, and for OSC the sound's number. */
static void draw_knobs(int layer, int sound) {
    for (int i = 0; i < 2; i++) {
        jt_oled_text(0, 16 * i, KNOB_NAMES[layer][i]);
        frame(0, 9 + 16 * i, JT_OLED_W, 6);
        block(2, 11 + 16 * i, (int)(P[KNOBS[layer + 1][i]] * (float)(JT_OLED_W - 4)), 2);
    }
    if (layer == NOTE) {
        char num[9], *p = number(text(num, "SOUND "), sound + 1);
        *p = 0;
        jt_oled_text(JT_OLED_W - 8 * (int)(p - num), 0, num);
    }
}

static void draw_clear(int layer, int held_ms, bool done) {
    char line[17], *p = text(line, done ? LAYER_NAMES[layer] : "CLEAR ");
    p = text(p, done ? " CLEARED" : LAYER_NAMES[layer]);
    if (!done) *p++ = '?';
    *p = 0;
    jt_oled_text((JT_OLED_W - 8 * (int)(p - line)) / 2, done ? 12 : 2, line);
    if (done) return;
    jt_oled_text(16, 13, "KEEP HOLDING");
    frame(0, 24, JT_OLED_W, 8);
    block(2, 26, (JT_OLED_W - 4) * (CLEAR_MS - held_ms) / CLEAR_MS, 4);
}

/* bit n = button n, bit 6 = SELECT pressed; debounced as in examples/speak */
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

static void use_sound(int n) {
    for (int i = 0; i < SOUND_PARAMS; i++) P[i] = (float)SOUND[n][i] * (1.0f / 255.0f);
}

int main(void) {
    int layer = NOTE, bpm = 126, quiet = 0, last_mod = -1, sound = 0;
    int anchor[2], owns[2] = {0, 0};
    uint16_t last_bits = 0, stable = 0, touched = 0, made = 0, edited = 0;
    uint8_t held = 0, leds = 0xFF, as_shift = 0, fresh_note = 0;
    uint32_t led_until[LAYERS] = {0}, info_until = 0, select_since = 0, clear_since = 0, cleared_until = 0;
    int select_mode = 0;                    /* 0 up, 1 held as shift (a tap if nothing follows), 2 used up */
    int clearing = -1, cleared = -1;
    bool shift_acted = false;
    int32_t enc_last;

    jt_init();
    factory();
    use_sound(0);
    jt_tape_init(&tape, tape_buf, TAPE);
    jt_filter(1000, 0);
    jt_audio_init(render);
    jt_usb_init();

    for (int i = 0; i < 6; i++) { held = buttons(); jt_delay_ms(8); }
    enc_last = jt_encoder();
    anchor[0] = jt_pot(JT_ADC_CUTOFF);
    anchor[1] = jt_pot(JT_ADC_RESONANCE);

    for (;;) {
        jt_delay_ms(8);
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

        /* A held button turns the knobs and SELECT into that layer's. Held SELECT is a shift
         * key; a button pressed during it is a switch (filter type, mutes), not a hold. */
        bool shift = select_mode == 1 && (now & 64);
        if (!escape)
            for (int i = 0; i < LAYERS; i++)
                if (pressed >> i & 1) {
                    if (shift) {
                        if (i == SLD) { analog = !analog; if (!analog) jt_filter(1000, 0); }
                        else if (i == KICK || i == HAT) mute ^= (uint8_t)(1u << i);
                        as_shift |= (uint8_t)(1u << i);
                        shift_acted = true;
                        info_until = t + 900;
                    } else layer = i;
                }
        as_shift &= now;
        int mod = -1;
        for (int i = LAYERS - 1; i >= 0; i--)
            if ((now >> i & 1) && !(as_shift >> i & 1) && !escape) mod = i;

        if (pressed & 64) {
            select_mode = 2;
            if (mod >= 0) { clearing = mod; clear_since = t; }
            else if (!stable) { select_mode = 1; select_since = t; shift_acted = false; }
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

        if (clearing >= 0) {                                /* SELECT must stay down for a second */
            if (!(now & 64) || mod != clearing) clearing = -1;
            else if (t - clear_since >= CLEAR_MS) {
                clear(clearing);
                cleared = clearing;
                cleared_until = t + 800;
                clearing = -1;
            }
        }

        /* SELECT turn */
        int steps = (jt_encoder() - enc_last) / 8;
        if (steps) {
            enc_last += steps * 8;
            if (stable && !shift && mod < 0) {              /* held steps: pitch, or drum level */
                for (int s = 0; s < STEPS; s++) {
                    step_t *st = &pat[pattern][s];
                    if (!(stable >> (15 - s) & 1)) continue;
                    if (layer == KICK && st->kick) { int v = st->kick + steps; st->kick = (uint8_t)(v < 1 ? 1 : v > 3 ? 3 : v); }
                    else if (layer == HAT && st->hat) { int v = st->hat + steps; st->hat = (uint8_t)(v < 1 ? 1 : v > 3 ? 3 : v); }
                    else if (layer <= OCT && (st->flags & GATE)) {
                        int v = st->note + steps;
                        st->note = fresh_note = (uint8_t)(v < 0 ? 0 : v > 12 ? 12 : v);
                    }
                }
                edited |= stable;
            } else if (mod >= 0) {
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

        /* keys: with SELECT held, a pattern; with OSC held, a sound; otherwise the steps */
        if (shift) {
            int k = jt_keyboard_lowest(down);
            if (k >= 0) { queued = k; shift_acted = true; info_until = t + 900; }
            edited |= stable;
        } else if (mod >= 0) {
            int k = jt_keyboard_lowest(down);
            if (k >= 0 && mod == NOTE) { sound = k; use_sound(k); last_mod = -2; }
            edited |= stable;
        } else {
            for (int s = 0; s < STEPS; s++) {
                uint16_t b = JT_KEYBOARD_BIT(s);
                step_t *st = &pat[pattern][s];
                bool there = has(layer, st);
                if (layer == NOTE || layer == KICK || layer == HAT) {       /* add at once, remove on letting go */
                    if ((down & b) && !there) {
                        toggle(layer, s);
                        if (layer == NOTE) st->note = fresh_note;
                        made |= b;
                    }
                    if ((up & b) && !(made & b) && !(edited & b) && there) toggle(layer, s);
                } else if (down & b) {
                    toggle(layer, s);
                }
            }
        }
        made &= stable;
        edited &= stable;

        /* knobs: cutoff and resonance, or the held button's pair; each waits until it is moved */
        if (mod != last_mod) {
            last_mod = mod;
            anchor[0] = jt_pot(JT_ADC_CUTOFF);
            anchor[1] = jt_pot(JT_ADC_RESONANCE);
            owns[0] = owns[1] = 0;
        }
        for (int i = 0; i < 2; i++) {
            int raw = jt_pot(i == 0 ? JT_ADC_CUTOFF : JT_ADC_RESONANCE), d = raw - anchor[i];
            if (!owns[i] && (d > 90 || d < -90)) owns[i] = 1;
            if (owns[i]) P[KNOBS[mod + 1][i]] = raw >= 4000 ? 1.0f : (float)raw * (1.0f / 4000.0f);
        }

        /* lights: the open layer, and flashes for accents, kick and hat */
        uint8_t f = fired, want = (uint8_t)(1u << layer);
        fired = 0;
        static const uint8_t FLASH[3] = {ACC, KICK, HAT};
        static const uint8_t BIT[3] = {2, 16, 32};
        for (int i = 0; i < 3; i++) {
            if (f & BIT[i]) led_until[FLASH[i]] = t + 60;
            if (t < led_until[FLASH[i]]) want ^= (uint8_t)(1u << FLASH[i]);
        }
        if (want != leds) { leds = want; jt_leds(leds); }

        jt_oled_clear();
        if (clearing >= 0) draw_clear(clearing, (int)(t - clear_since), false);
        else if (t < cleared_until) draw_clear(cleared, 0, true);
        else if (mod >= 0) draw_knobs(mod, sound);
        else if (t < info_until) draw_info(bpm);
        else draw(layer, stable);
        jt_oled_flush();
    }
}
