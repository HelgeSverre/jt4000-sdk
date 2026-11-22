/* Demo: a minute and a half of music and pictures, then a synth to play.
 *
 * In the tradition of the demoscene: nothing is stored that can be computed. The music is a
 * small tracker (seven synthesized voices and a speaking one, arranged bar by bar), the
 * pictures are drawn from arithmetic, and both run off one clock, the count of audio
 * samples, so a flash lands on its kick however long a frame takes to draw.
 *
 * You can interfere while it plays:
 *   keys               shift the whole song to another key (the A key is home)
 *   CUTOFF, RESONANCE  the analog filter, once you move them
 *   SELECT turn        how the picture is shown: plain, negative, scan lines, mesh, mirrored, upside down
 *   SELECT press       negative on / off
 *   LFO FILTER AMP ARP held: no drums / no bass / no arpeggio and lead / no pad
 *   OSC                skip to the end
 * Afterwards the keys play (four voices), the knobs are the filter, and SELECT press
 * starts the demo again.
 */
#include "jt4000.h"
#include "jt_dsp.h"
#include "jt_lpc.h"
#include "words.h"

#define SR ((float)JT_SAMPLE_RATE)
#define STEP 3750                           /* samples per 16th note: 128 beats a minute, exactly */
#define BAR_MS 1875
#define END_BAR 56                          /* the demo is 56 bars; then you play */
#define LIFT 44                             /* from this bar, for eight, the song is two semitones higher */
#define TAPE 8192

/* ---- the song ---- */
enum { KICK = 1, CLAP = 2, HAT = 4, HAT16 = 8, BASS = 16, ARP = 32, LEAD = 64, PAD = 128, ROLL = 256, HIGH = 512, SOLO = 1024 };
#define DRUMS (KICK | CLAP | HAT | HAT16 | ROLL | SOLO)

static int parts(int bar) {                 /* what plays in each bar */
    if (bar < 2) return PAD | ARP;
    if (bar < 4) return PAD | ARP | HAT16 | (bar == 3 ? ROLL : 0);
    if (bar < 12) return KICK | HAT | BASS | ARP | PAD;
    if (bar < 20) return KICK | CLAP | HAT | BASS | ARP | PAD | LEAD;
    if (bar < 24) return PAD | (bar >= 22 ? ARP : 0) | (bar == 23 ? ROLL : 0);
    if (bar < 32) return KICK | CLAP | HAT | HAT16 | BASS | ARP | PAD | LEAD;
    if (bar < 36) return SOLO | PAD;                        /* the drums alone, played by falling balls */
    if (bar < 43) return KICK | CLAP | HAT | HAT16 | BASS | ARP | PAD | LEAD | HIGH;
    if (bar < 44) return PAD | ARP | ROLL | HIGH;           /* a bar of held breath before the lift */
    if (bar < 52) return KICK | CLAP | HAT | HAT16 | BASS | ARP | PAD | LEAD | HIGH;
    if (bar < END_BAR) return PAD | ARP;
    return 0;
}

/* four bars of harmony, round and round: A minor, F, C, G */
static const int8_t ROOT[4] = {0, -4, 3, -2};
static const int8_t THIRD[4] = {3, 4, 4, 4};
static const int8_t MELODY[64] = {          /* semitones above A4; -1 holds */
    12, -1, -1, 12, -1, 10, -1, 7, -1, -1, 7, -1, 8, -1, 7, -1,
    8, -1, -1, 8, -1, 7, -1, 5, -1, -1, 5, -1, 3, -1, 5, -1,
    7, -1, -1, 7, -1, 3, -1, 7, -1, -1, 10, -1, 12, -1, 10, -1,
    10, -1, -1, 10, -1, 7, -1, 5, -1, -1, 2, -1, 5, -1, 7, -1,
};
static const int8_t BASSLINE[16] = {0, -1, 12, 0, -1, 0, 12, -1, 0, -1, 12, 0, -1, 0, 12, 0};
/* The arpeggio falls from the top of the chord and leaves gaps: chord tone 0..3, -1 rests. */
static const int8_t ARP_LOW[16] = {3, -1, 2, 3, -1, 1, 2, -1, 3, -1, 1, 2, -1, 0, 1, -1};
static const int8_t ARP_HIGH[16] = {3, 2, -1, 3, 1, -1, 2, 0, 3, 2, -1, 1, 3, -1, 2, 1};
static const struct { uint8_t bar, step; const char *word; } SAY[] = {
    {1, 0, "HELLO"}, {1, 8, "HUMAN"},
    {20, 0, "THIS"}, {20, 6, "IS"}, {21, 0, "CUSTOM"}, {22, 0, "FIRMWARE"},
    {28, 0, "WEE"}, {29, 0, "WEE"}, {30, 0, "WEE"}, {31, 0, "WEE"},     /* stretched and bent: see render */
    {36, 0, "FREE"}, {36, 5, "AND"}, {36, 8, "OPEN"}, {36, 12, "SOURCE"},
    {40, 0, "GET"}, {40, 4, "THE"}, {40, 7, "CODE"},
    {52, 0, "NOW"}, {52, 6, "YOU"}, {53, 0, "TOUCH"}, {53, 5, "THE"}, {53, 8, "KEYS"},
};
/* The drum solo, two bars that alternate: kick, snare, hat, tom. The pictures read this
 * too, to drop a ball on each drum exactly when it sounds. */
static const char *const SOLO_BEATS[2][4] = {
    {"x..x..x...x..x..", "....x.......x...", "x.x.x.x.x.x.x.x.", ".........x.x...x"},
    {"x..x..x..x..x...", "....x..x....x.xx", "x.x.x.x.x.xxx.x.", ".x........x....."},
};
static bool solo_hit(int drum, int n) {
    int bar = n / 16;
    return n >= 0 && bar >= 32 && bar < 36 && SOLO_BEATS[bar & 1][drum][n & 15] == 'x';
}

/* ---- shared between the sound, the pictures and your hands ---- */
static volatile uint32_t samples;           /* the one clock */
static volatile int32_t song_step = -1;     /* 16ths since the start */
static volatile uint32_t step_at;           /* the sample count when that step began */
static volatile float kick_level;
static volatile int spoken = -1;            /* the word being said, for the display */
static volatile bool over;                  /* the demo has finished: the keys play */
static volatile int key_shift;              /* semitones, from the touch keys */
static volatile int muted;                  /* parts silenced by held buttons */
static volatile int8_t scope[128];

/* ---- voices ---- */
static struct { float ph, bend, amp; } kick, tom;
static struct { float amp, last; int hits, wait; } clap;
static struct { float amp, last, last2, decay; } hat;
static struct { jt_osc_t osc; jt_svf_t f; float env, amp; int gate; } bass = {.osc = {.wave = JT_WAVE_SAW}};
static struct { jt_osc_t a, b; float env, lp; } arp = {.a = {.wave = JT_WAVE_SQUARE}, .b = {.wave = JT_WAVE_SAW}};
static struct { jt_osc_t osc; float hz, goal, env, lfo; } lead = {.osc = {.wave = JT_WAVE_SAW}};
static struct { jt_osc_t osc[6]; float level, lp; int tuned; } pad = {.tuned = -99};
static struct { jt_osc_t a, b; float env, goal; int8_t note; } key[4];     /* for playing afterwards */
static jt_lpc_t vox;
static int16_t tape_buf[TAPE];
static jt_tape_t tape;

static int find(const char *name) {
    for (int w = 0; w < WORD_COUNT; w++) {
        const char *a = WORD_NAMES[w], *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (!*a && !*b) return w;
    }
    return 0;
}

static void step(void) {
    int n = ++song_step, bar = n / 16, s = n & 15, p = parts(bar) & ~muted, chord = bar & 3;
    step_at = samples;
    if (bar >= END_BAR) { over = true; return; }
    int shift = key_shift + (bar >= LIFT && bar < LIFT + 8 ? 2 : 0);
    float root = 45.0f + (float)(ROOT[chord] + shift);      /* A2 and its relatives */

    if ((p & KICK) && !(s & 3)) { kick.amp = 1.0f; kick.bend = 1.0f; kick.ph = 0.0f; }
    if ((p & CLAP) && (s == 4 || s == 12)) { clap.hits = 3; clap.wait = 0; }
    if ((p & ROLL) && s >= 8) { clap.hits = 1; clap.wait = 0; clap.amp = 0.3f + 0.08f * (float)(s - 8); }
    if (p & SOLO) {
        if (solo_hit(0, n)) { kick.amp = 1.0f; kick.bend = 1.0f; kick.ph = 0.0f; }
        if (solo_hit(1, n)) { clap.hits = 1; clap.wait = 0; clap.amp = 0.0f; }
        if (solo_hit(2, n)) { hat.amp = 0.8f; hat.decay = 0.997f; }
        if (solo_hit(3, n)) { tom.amp = 0.9f; tom.bend = 1.0f; tom.ph = 0.0f; }
    }
    if ((p & HAT) && (s & 3) == 2) { hat.amp = 0.9f; hat.decay = 0.9994f; }
    else if ((p & HAT16)) { hat.amp = s & 1 ? 0.35f : 0.5f; hat.decay = 0.996f; }
    if ((p & BASS) && BASSLINE[s] >= 0) {
        jt_osc_hz(&bass.osc, jt_note_hz(root - 12.0f + (float)BASSLINE[s]), SR);
        bass.env = 1.0f;
        bass.gate = STEP * 6 / 10;
    }
    int tone = p & ARP ? (p & HIGH ? ARP_HIGH[s] : ARP_LOW[s]) : -1;
    if (tone >= 0) {
        int up = tone == 0 ? 0 : tone == 1 ? THIRD[chord] : tone == 2 ? 7 : 12;
        float hz = jt_note_hz(root + 24.0f + (float)up + (p & HIGH ? 12.0f : 0.0f));
        jt_osc_hz(&arp.a, hz, SR);
        jt_osc_hz(&arp.b, hz * 1.006f, SR);
        arp.env = 1.0f;
    }
    if ((p & LEAD) && MELODY[(bar & 3) * 16 + s] >= 0) {
        lead.goal = jt_note_hz(69.0f + (float)(MELODY[(bar & 3) * 16 + s] + shift));
        if (lead.env < 0.1f) lead.hz = lead.goal;
        lead.env = 1.0f;
    }
    if (s == 0 || pad.tuned != shift) {                     /* the pad moves to the bar's chord, or to a new key */
        for (int i = 0; i < 3; i++) {
            float note = root + 12.0f + (i == 0 ? 0.0f : i == 1 ? (float)THIRD[chord] : 7.0f);
            jt_osc_hz(&pad.osc[2 * i], jt_note_hz(note), SR);
            jt_osc_hz(&pad.osc[2 * i + 1], jt_note_hz(note + 0.12f), SR);
        }
        pad.tuned = shift;
    }
    for (unsigned i = 0; i < sizeof SAY / sizeof SAY[0]; i++)
        if (SAY[i].bar == bar && SAY[i].step == s) {
            spoken = find(SAY[i].word);
            jt_lpc_say(&vox, &WORD_DATA[WORD_AT[spoken]]);
        }
}

static void render(int16_t *out, int n) {
    static float wet_now, wet_prev, half_send, duck;
    static int alt, scope_w, left;
    int bar = song_step / 16, p = song_step < 0 ? 0 : parts(bar) & ~muted;

    float cut = 120.0f * jt_exp2(bass.env * 4.5f + (bar >= 24 ? 1.0f : 0.0f));
    jt_svf_set(&bass.f, cut, 0.75f, SR);
    float pad_goal = (p & PAD) ? (bar >= 52 ? 0.5f : 1.0f) : 0.0f;
    vox.hz = 110.0f * jt_exp2((float)key_shift * (1.0f / 12.0f));
    vox.speed = vox.ratio = 1.0f;
    if (bar >= 28 && bar < 32) {            /* "wee": one syllable dragged out, its pitch thrown around on every 16th */
        int n16 = song_step;
        vox.speed = 7.0f;
        vox.hz *= jt_exp2((float)((n16 * 7) % 12 + (n16 & 3) * 5 + (bar - 28) * 4) * (1.0f / 12.0f));
        vox.ratio = 0.8f + 0.12f * (float)((n16 * 5) & 7);
    }
    jt_lpc_block(&vox);
    lead.lfo += 5.5f * (float)n / SR;
    if (lead.lfo >= 1.0f) lead.lfo -= 1.0f;
    float vibrato = 1.0f + 0.006f * jt_sin(lead.lfo);
    /* while the voice speaks the band steps back, so the words come through */
    duck += ((jt_lpc_busy(&vox) ? 1.0f : 0.0f) - duck) * 0.12f;
    float band = 1.0f - (bar >= 28 && bar < 32 ? 0.2f : 0.6f) * duck;      /* less so under the long "wee" */

    for (int i = 0; i < n; i++) {
        if (!over && --left <= 0) { left = STEP; step(); }
        samples++;
        float white = jt_rand();

        kick.bend *= 0.9985f;
        kick.ph += 48.0f * (1.0f + 4.0f * kick.bend) / SR;
        if (kick.ph >= 1.0f) kick.ph -= 1.0f;
        float k = jt_softclip(jt_sin(kick.ph) * kick.amp * 1.8f);
        kick.amp *= 0.9996f;
        tom.bend *= 0.9992f;
        tom.ph += 130.0f * (1.0f + 1.2f * tom.bend) / SR;
        if (tom.ph >= 1.0f) tom.ph -= 1.0f;
        k += 0.8f * jt_sin(tom.ph) * tom.amp;
        tom.amp *= 0.9994f;

        if (clap.hits && --clap.wait <= 0) { clap.hits--; clap.wait = 320; if (clap.amp < 0.3f || clap.hits) clap.amp = 0.9f; }
        float c = (white - 0.7f * clap.last) * clap.amp;
        clap.last = white;
        clap.amp *= 0.9993f;

        float h1 = white - hat.last, h2 = h1 - hat.last2;
        hat.last = white;
        hat.last2 = h1;
        float h = h2 * hat.amp;
        hat.amp *= hat.decay;

        if (bass.gate > 0) bass.gate--;
        bass.amp += ((bass.gate > 0 ? 1.0f : 0.0f) - bass.amp) * 0.01f;
        bass.env *= 0.9997f;
        float b = jt_softclip(jt_svf_next(&bass.f, jt_osc_next(&bass.osc)) * 2.0f) * bass.amp * (1.0f - 0.5f * kick.amp);

        float a = 0.5f * (jt_osc_next(&arp.a) + jt_osc_next(&arp.b));
        arp.lp += (a - arp.lp) * (0.03f + 0.4f * arp.env);
        a = arp.lp * arp.env;
        arp.env *= 0.99965f;

        lead.hz += (lead.goal - lead.hz) * 0.002f;
        lead.osc.inc = lead.hz * vibrato / SR;
        float l = jt_osc_next(&lead.osc) * lead.env;
        lead.env *= (p & LEAD) ? 0.99996f : 0.999f;

        float pd = 0.0f;
        pad.level += (pad_goal - pad.level) * 0.00004f;
        if (pad.level > 0.001f) {
            for (int o = 0; o < 6; o++) pd += jt_osc_next(&pad.osc[o]);
            pad.lp += (pd * 0.16f - pad.lp) * 0.06f;
            pd = pad.lp * pad.level;
        }

        float pl = 0.0f;                                    /* your own notes, once the demo is over */
        for (int v = 0; v < 4; v++) {
            key[v].env += (key[v].goal - key[v].env) * (key[v].goal > key[v].env ? 0.01f : 0.0006f);
            if (key[v].env > 0.001f) pl += 0.5f * (jt_osc_next(&key[v].a) + jt_osc_next(&key[v].b)) * key[v].env;
        }

        float v = jt_lpc_next(&vox) * 2.2f;
        float music = 0.75f * k + 0.5f * c + 0.2f * h + 0.5f * b + 0.13f * a + 0.2f * l + 0.5f * pd;
        float dry = music * band + 0.3f * pl + v;
        float send = (0.3f * a + 0.25f * l) * band + 0.3f * v + 0.2f * pl + 0.1f * c, y;
        if (alt ^= 1) {
            half_send = send;
            y = dry + 0.5f * (wet_prev + wet_now);
        } else {
            wet_prev = wet_now;
            float d = jt_tape_read(&tape, (float)(STEP * 3 / 2));       /* three 16ths, at the echo's 16 kHz */
            jt_tape_write(&tape, 0.5f * (half_send + send) + d * 0.5f);
            wet_now = d * 0.8f;
            y = dry + wet_prev;
        }
        y = jt_softclip(y * 0.38f);
        out[i] = (int16_t)(y * 30000.0f);
        if ((i & 3) == 0) scope[scope_w++ & 127] = (int8_t)(y * 120.0f);
    }
    kick_level = kick.amp;
}

/* ---- pictures ---- */
static int8_t wave[256];                    /* a sine, -127..127, for everything that swings */
#define SIN(a) wave[(a) & 255]
#define COS(a) wave[((a) + 64) & 255]
static const uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
/* a pixel in one of 17 shades, by dithering: 0 dark .. 16 lit */
#define SHADE(x, y, level) jt_oled_pixel((x), (y), (level) > BAYER[(y) & 3][(x) & 3])

static void block(int x, int y, int w, int h) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) jt_oled_pixel(i, j, true);
}
static void wipe(int x, int y, int w, int h) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) jt_oled_pixel(i, j, false);
}

static void centred(int y, const char *s, int scale) {
    int n = 0;
    while (s[n]) n++;
    if (scale > 1) jt_oled_text_big((JT_OLED_W - 8 * scale * n) / 2, y, s, scale);
    else jt_oled_text((JT_OLED_W - 8 * n) / 2, y, s);
}

/* stars rushing past */
static void stars(uint32_t t, int speed) {
    static struct { int16_t x, y, z; } star[40];
    static uint32_t seed = 7, last;
    int dt = (int)(t - last);
    last = t;
    if (dt > 100 || dt < 0) dt = 100;
    for (int i = 0; i < 40; i++) {
        star[i].z = (int16_t)(star[i].z - dt * speed / 8);
        if (star[i].z <= 8) {
            seed = seed * 1103515245u + 12345u;
            star[i].x = (int16_t)((int)(seed >> 16 & 1023) - 512);
            star[i].y = (int16_t)((int)(seed >> 6 & 255) - 128);
            star[i].z = 300;
        }
        int x = 64 + star[i].x * 40 / star[i].z, y = 16 + star[i].y * 40 / star[i].z;
        jt_oled_pixel(x, y, true);
        if (star[i].z < 90) jt_oled_pixel(x + 1, y, true);
    }
}

/* a filled four-sided shape (corners in order), in a shade */
static void quad(const int *px, const int *py, int level) {
    int top = py[0], bottom = py[0];
    for (int i = 1; i < 4; i++) { if (py[i] < top) top = py[i]; if (py[i] > bottom) bottom = py[i]; }
    if (top < 0) top = 0;
    if (bottom > JT_OLED_H - 1) bottom = JT_OLED_H - 1;
    for (int y = top; y <= bottom; y++) {
        int lo = 9999, hi = -9999;
        for (int i = 0; i < 4; i++) {
            int x0 = px[i], y0 = py[i], x1 = px[(i + 1) & 3], y1 = py[(i + 1) & 3];
            if (y0 == y1) { if (y == y0) { if (x0 < lo) lo = x0; if (x0 > hi) hi = x0; if (x1 < lo) lo = x1; if (x1 > hi) hi = x1; } continue; }
            if ((y < y0 && y < y1) || (y > y0 && y > y1)) continue;
            int x = x0 + (x1 - x0) * (y - y0) / (y1 - y0);
            if (x < lo) lo = x;
            if (x > hi) hi = x;
        }
        if (lo < 0) lo = 0;
        if (hi > JT_OLED_W - 1) hi = JT_OLED_W - 1;
        for (int x = lo; x <= hi; x++) SHADE(x, y, level);
    }
}

/* a solid cube, turning, each face lit by how far it faces the light */
static void cube(int cx, int cy, int size, int ax, int ay) {
    static const int8_t V[8][3] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1}, {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}};
    static const uint8_t FACE[6][4] = {{0, 1, 2, 3}, {5, 4, 7, 6}, {4, 0, 3, 7}, {1, 5, 6, 2}, {4, 5, 1, 0}, {3, 2, 6, 7}};
    int px[8], py[8];
    for (int i = 0; i < 8; i++) {
        int x = V[i][0] * size, y = V[i][1] * size, z = V[i][2] * size;
        int x1 = (x * COS(ay) + z * SIN(ay)) / 127, z1 = (z * COS(ay) - x * SIN(ay)) / 127;
        int y1 = (y * COS(ax) - z1 * SIN(ax)) / 127, z2 = (z1 * COS(ax) + y * SIN(ax)) / 127;
        int d = 64 + z2;
        px[i] = cx + x1 * 64 / d;
        py[i] = cy + y1 * 64 / d;
    }
    for (int f = 0; f < 6; f++) {
        int qx[4], qy[4];
        for (int i = 0; i < 4; i++) { qx[i] = px[FACE[f][i]]; qy[i] = py[FACE[f][i]]; }
        int turn = (qx[1] - qx[0]) * (qy[2] - qy[1]) - (qy[1] - qy[0]) * (qx[2] - qx[1]);
        if (turn <= 0) continue;                            /* facing away */
        int level = 3 + turn * 13 / (size * size * 5 + 1);  /* the more it faces us, the brighter */
        quad(qx, qy, level > 16 ? 16 : level);
        for (int i = 0; i < 4; i++) jt_oled_line(qx[i], qy[i], qx[(i + 1) & 3], qy[(i + 1) & 3], true);
    }
}

/* four sines added up and cut into shades */
static void plasma(uint32_t t, int push) {
    int a = (int)(t / 9), b = (int)(t / 13), c = (int)(t / 7);
    for (int y = 0; y < JT_OLED_H; y++)
        for (int x = 0; x < JT_OLED_W; x++) {
            int v = SIN(x * 2 + a) + SIN(y * 5 - b) + SIN((x + y) * 2 + c) + SIN(((x - 64) * (x - 64) + (y - 16) * (y - 16) * 4) / 48 - a);
            SHADE(x, y, ((v * 2 + 508) >> 6) + push);
        }
}

/* a chequerboard, turning and breathing */
static void rotozoom(uint32_t t) {
    int angle = (int)(t / 14), zoom = 90 + SIN((int)(t / 9)) / 2;
    int c = COS(angle) * zoom / 64, s = SIN(angle) * zoom / 64;
    for (int y = 0; y < JT_OLED_H; y++) {
        int u = (-64 * c - (y - 16) * 2 * s) + (int)(t << 2), v = (-64 * s + (y - 16) * 2 * c);
        for (int x = 0; x < JT_OLED_W; x++, u += c, v += s)
            if (((u >> 11) ^ (v >> 11)) & 1) jt_oled_pixel(x, y, true);
    }
}

/* Flying down a tube. For each 2 x 2 patch of screen: which way round the tube it looks,
 * and how far down. Worked out once; the frame just slides a pattern along those. */
static uint8_t tube_angle[16][64], tube_depth[16][64];

static void tube_init(void) {
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 64; x++) {
            float dx = (float)(x - 32) + 0.5f, dy = ((float)(y - 8) + 0.5f) * 2.0f;
            float ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
            float r = ay > ax ? ax / ay : ay / ax;          /* arctangent, within a degree */
            float a = r * (0.1553f - 0.0303f * r);          /* in turns: 0 .. 1/8 for r 0 .. 1 */
            if (ay > ax) a = 0.25f - a;
            if (dx < 0) a = 0.5f - a;
            if (dy < 0) a = 1.0f - a;
            float dist = jt_sqrt(dx * dx + dy * dy);
            int depth = (int)(420.0f / (dist + 1.0f));
            tube_angle[y][x] = (uint8_t)(a * 256.0f);
            tube_depth[y][x] = (uint8_t)(depth > 255 ? 255 : depth);
        }
}

static void tube(uint32_t t, int boost) {
    int along = (int)(t / 6), round = (int)(t / 21);
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 64; x++) {
            int d = tube_depth[y][x];
            bool tile = (((tube_angle[y][x] + round) >> 5) ^ ((d + along) >> 5)) & 1;
            int level = tile ? 16 - d / 9 + boost : 0;      /* the far end fades into the dark */
            for (int j = 0; j < 2; j++)
                for (int i = 0; i < 2; i++) SHADE(2 * x + i, 2 * y + j, level);
        }
}

/* a square bar, twisted along its length */
static void twister(uint32_t t, int boost) {
    for (int x = 0; x < JT_OLED_W; x++) {
        int a = (int)(t / 7) + SIN(x + (int)(t / 11)) * 2 / 3 + SIN(x * 2 - (int)(t / 5)) / 4;
        for (int f = 0; f < 4; f++) {
            int y0 = 16 + 14 * SIN(a + 64 * f) / 127, y1 = 16 + 14 * SIN(a + 64 * (f + 1)) / 127;
            if (y1 <= y0) continue;                         /* this side faces away */
            int level = 3 + (y1 - y0) * 12 / 20 + boost;
            for (int y = y0; y <= y1; y++) SHADE(x, y, f & 1 ? level : level + 3);
            jt_oled_pixel(x, y0, true);
        }
    }
}

/* fire along the bottom: each cell is the average of those below it, a little cooler */
static void fire(int fuel) {
    static uint8_t heat[9][64];
    for (int x = 0; x < 64; x++) heat[8][x] = (uint8_t)(jt_rand() > 0.25f ? fuel : 0);      /* embers, here and there */
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 64; x++) {
            int sum = heat[y + 1][(x + 63) & 63] + heat[y + 1][x] + heat[y + 1][(x + 1) & 63] + heat[y + (y < 7 ? 2 : 1)][x];
            sum = sum * 15 / 64 - 3;
            heat[y][x] = (uint8_t)(sum < 0 ? 0 : sum);
        }
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 64; x++)
            for (int j = 0; j < 2; j++)
                for (int i = 0; i < 2; i++) SHADE(2 * x + i, 16 + 2 * y + j, heat[y][x] / 7);
}

/* Text that crosses the screen exactly once in `duration` ms, however long it is: the last
 * letter leaves as the scene ends. */
static void scroller(const char *text, int y, uint32_t t, uint32_t duration, int scale, int swing) {
    int n = 0, w = 8 * scale;
    while (text[n]) n++;
    if (t > duration) t = duration;
    int x = JT_OLED_W - (int)((uint64_t)t * (uint32_t)(n * w + JT_OLED_W) / duration);
    char one[2] = {0, 0};
    for (int i = 0; i < n; i++, x += w) {
        if (x <= -w || x >= JT_OLED_W) continue;
        one[0] = text[i];
        int dy = swing ? SIN(x * 3 + (int)(t / 4)) * swing / 127 : 0;
        if (scale > 1) jt_oled_text_big(x, y + dy, one, scale);
        else jt_oled_text(x, y + dy, one);
    }
}

static void trace(int mid, int height) {
    int last = mid;
    for (int x = 0; x < JT_OLED_W; x++) {
        int y = mid - scope[x] * height / 127;
        if (x) jt_oled_line(x - 1, last, x, y, true);
        last = y;
    }
}

/* A ship shoots a word to bits, a letter at a time. Nothing is remembered between frames:
 * where every bullet and fragment is follows from the time alone. */
static void shooter(const char *word, uint32_t t, uint32_t per_letter) {
    int n = 0;
    while (word[n]) n++;
    int ship_y = 16 + 10 * SIN((int)(t / 5)) / 127, x0 = 122 - n * 10;
    jt_oled_line(2, ship_y - 3, 10, ship_y, true);          /* the ship */
    jt_oled_line(2, ship_y + 3, 10, ship_y, true);
    jt_oled_line(2, ship_y - 3, 4, ship_y, true);
    jt_oled_line(2, ship_y + 3, 4, ship_y, true);
    char one[2] = {0, 0};
    for (int i = 0; i < n; i++) {
        uint32_t dies = (uint32_t)(i + 1) * per_letter;
        int x = x0 + i * 10 + SIN((int)(t / 9)) / 30, y = 12 + 7 * SIN((int)(t / 6) + i * 40) / 127;
        if (t < dies) {
            one[0] = word[i];
            jt_oled_text(x, y, one);
            if (dies - t < per_letter) {                    /* the shot on its way to this letter */
                int along = (int)((per_letter - (dies - t)) * 256 / per_letter);
                int bx = 10 + (x - 10) * along / 256, by = ship_y + (y + 4 - ship_y) * along / 256;
                jt_oled_line(bx, by, bx + 5, by, true);
            }
        } else if (t - dies < 450) {                        /* the letter in pieces, flying apart */
            int age = (int)(t - dies);
            for (int k = 0; k < 10; k++) {
                int a = k * 26 + i * 37;
                jt_oled_pixel(x + 4 + COS(a) * age / 1100, y + 4 + SIN(a) * age / 1500 + age * age / 9000, true);
                if (age < 150) jt_oled_pixel(x + 4 + COS(a + 13) * age / 600, y + 4 + SIN(a + 13) * age / 800, true);
            }
            if (age < 60) block(x - 1, y - 1, 10, 10);      /* the flash */
        }
    }
}

/* Four drums along the bottom, and a ball dropping onto each one in time to hit it as it sounds. */
static void drummers(void) {
    static const uint8_t AT[4] = {18, 49, 80, 111}, TOP[4] = {25, 23, 20, 24}, WIDE[4] = {13, 9, 8, 7};
    int32_t now = song_step;
    int32_t into = (int32_t)(samples - step_at);            /* samples into the current step */
    for (int d = 0; d < 4; d++) {
        int struck = 0;
        for (int32_t m = now - 5; m <= now + 9; m++) {
            if (!solo_hit(d, m)) continue;
            int ms = ((m - now) * STEP - into) / 32;        /* until it lands; negative once it has */
            if (ms > 0 && ms < 520) {                       /* falling: slow at the top, fast at the drum */
                int y = TOP[d] - 3 - (TOP[d] + 4) * ms * ms / (520 * 520);
                block(AT[d] - 1, y, 3, 3);
            } else if (ms <= 0 && ms > -260) {              /* bouncing off, to one side */
                int age = -ms, y = TOP[d] - 3 - age * (260 - age) / 1400;
                block(AT[d] - 1 + (d & 1 ? -1 : 1) * age / 16, y, 2, 2);
                if (age < 70) struck = 1;
            }
        }
        int top = TOP[d] + struck;                          /* the skin gives under the hit */
        block(AT[d] - WIDE[d], top, 2 * WIDE[d] + 1, 1 + struck);
        if (d == 2) {                                       /* the hat: a plate on a stand */
            jt_oled_line(AT[d], top, AT[d], 31, true);
        } else {
            jt_oled_line(AT[d] - WIDE[d], top, AT[d] - WIDE[d] + 2, 31, true);
            jt_oled_line(AT[d] + WIDE[d], top, AT[d] + WIDE[d] - 2, 31, true);
            if (d == 0) block(AT[d] - WIDE[d] + 2, 31, 2 * WIDE[d] - 3, 1);
        }
    }
}

/* SELECT turn: the finished frame, shown another way */
enum { PLAIN, NEGATIVE, LINES, MESH, MIRROR, FLIP, LOOKS };
static void look(int how) {
    for (int y = 0; y < JT_OLED_H; y++)
        for (int x = 0; x < JT_OLED_W; x++) {
            switch (how) {
            case LINES:  if (y & 1) jt_oled_pixel(x, y, false); break;
            case MESH:   if ((x ^ y) & 1) jt_oled_pixel(x, y, false); break;
            case MIRROR: if (x >= JT_OLED_W / 2) jt_oled_pixel(x, y, jt_oled_get(JT_OLED_W - 1 - x, y)); break;
            case FLIP:
                if (y < JT_OLED_H / 2) {
                    bool top = jt_oled_get(x, y), low = jt_oled_get(x, JT_OLED_H - 1 - y);
                    jt_oled_pixel(x, y, low);
                    jt_oled_pixel(x, JT_OLED_H - 1 - y, top);
                }
                break;
            default: break;
            }
        }
}

/* One frame of the demo. `ms` is time since it began; the bar decides the scene. */
static void picture(uint32_t ms, int how) {
    int n = song_step < 0 ? 0 : song_step, bar = n / 16;
    uint32_t in_bar = ms % BAR_MS;
    int beat = kick_level > 0.5f, thump = (int)(kick_level * 4.0f);
    jt_oled_clear();
    if (bar < 4) {                          /* the name arrives out of the stars */
        static const char NAME[] = "JT-4000";
        char shown[8] = {0};
        stars(ms, 2 + bar * 2);
        int letters = (int)(ms / 420);
        for (int i = 0; i < 7 && i < letters; i++) shown[i] = NAME[i];
        if (bar >= 1) {
            wipe(8, 0, 112, 17);
            jt_oled_text_big(8, 0, shown, 2);
        }
        if (bar >= 1 && bar < 3 && spoken >= 0) centred(22, WORD_NAMES[spoken], 1);
        if (bar == 3) centred(22, "CUSTOM FIRMWARE", 1);
    } else if (bar < 12) {                  /* a lit cube, and the facts scrolling past underneath */
        cube(64 + SIN((int)(ms / 20)) / 3, 11, 9 + (beat ? 2 : 0), (int)(ms / 11), (int)(ms / 7));
        scroller("THIS IS NOT THE FACTORY FIRMWARE ... A BEHRINGER JT-4000 MICRO, REPROGRAMMED ... 170 MHZ ... 96 KB OF FLASH ... 32 KB OF RAM ... 128 X 32 PIXELS ... EVERY SOUND IS COMPUTED, NOTHING IS A SAMPLE ...",
                 24, ms - 4 * BAR_MS, 8 * BAR_MS, 1, 0);
    } else if (bar < 20) {                  /* plasma; the kick pushes it brighter */
        plasma(ms, thump - 1);
    } else if (bar < 24) {                  /* the break: the words as they are said, over a fire */
        static const char *const LINE[4] = {"THIS IS", "CUSTOM", "FIRMWARE", "FIRMWARE"};
        fire(bar == 23 ? 120 + (int)(in_bar / 14) : jt_lpc_busy(&vox) ? 230 : 90);
        if (bar < 23 || (in_bar / 117) & 1) centred(0, LINE[bar - 20], 2);
    } else if (bar < 28) {                  /* the floor turns; the sound is drawn across it */
        rotozoom(ms);
        wipe(0, 12, JT_OLED_W, 9);
        trace(16, 4);
    } else if (bar < 32) {                  /* a ship takes two words apart: 16ths for one, 8ths for the other */
        stars(ms, 9);
        if (bar < 30) shooter("BOOTLOADER", ms - 28 * BAR_MS, 2 * BAR_MS / 11);
        else shooter("SYNTH", ms - 30 * BAR_MS, 2 * BAR_MS / 6);
    } else if (bar < 36) {                  /* the drums, played by what falls on them */
        drummers();
    } else if (bar < 44) {                  /* where to get it, on a wave */
        stars(ms, 6);
        scroller("GITHUB.COM/HELGESVERRE/JT4000-SDK", 8, (ms - 36 * BAR_MS) % (2 * BAR_MS), 2 * BAR_MS, 2, 7);
    } else if (bar < 48) {                  /* the lift: two semitones up, and down the tube */
        tube(ms, thump);
    } else if (bar < 52) {                  /* a twisted bar */
        twister(ms, thump);
    } else {                                /* the handover */
        stars(ms, 12 - (bar - 52) * 3);
        centred(2, "NOW YOU", 2);
        if (bar >= 53) centred(24, "TOUCH THE KEYS", 1);
    }
    if (beat && bar >= 4 && bar < 44 && (bar < 20 || bar >= 24) && (bar < 32 || bar >= 36)) {      /* every kick: a tick in the corners */
        block(0, 0, 3, 3);
        block(125, 0, 3, 3);
    }
    look(how);
    jt_oled_flush();
}

/* After the demo: the notes you hold, and the sound as a line. */
static void play_screen(uint16_t keys) {
    jt_oled_clear();
    trace(12, 11);
    for (int k = 0; k < JT_KEYBOARD_KEYS; k++) {
        if (keys & JT_KEYBOARD_BIT(k)) block(k * 8 + 1, 26, 6, 6);
        else jt_oled_pixel(k * 8 + 3, 30, true);
    }
    jt_oled_flush();
}

/* bit 0 OSC, 1 LFO, 2 FILTER, 3 AMP, 4 ARP, 6 SELECT; a button counts after two readings */
static uint8_t buttons(void) {
    static uint8_t state, last;
    uint8_t k = jt_keys_raw(0), raw = 0;
    if (jt_button_osc()) raw |= 1;
    if (k & JT_KEY_LFO) raw |= 2;
    if (k & JT_KEY_FILTER) raw |= 4;
    if (k & JT_KEY_AMP) raw |= 8;
    if (k & JT_KEY_ARP) raw |= 16;
    if (k & JT_KEY_SELECT) raw |= 64;
    state = (uint8_t)((state & (raw | last)) | (raw & last));       /* changes when two readings agree */
    last = raw;
    return state;
}

static void restart(void) {
    jt_irq_off();
    song_step = -1;
    spoken = -1;
    over = false;
    jt_irq_on();
}

int main(void) {
    uint32_t began = 0, polled = 0;
    uint16_t last_bits = 0, stable = 0;
    int quiet = 0, how = PLAIN, anchor[2], owns[2] = {0, 0};
    bool negative = false;
    uint8_t held = 0, ev[4];
    int32_t enc_last;

    jt_init();
    for (int i = 0; i < 256; i++) wave[i] = (int8_t)(jt_sin((float)i * (1.0f / 256.0f)) * 127.0f);
    tube_init();
    for (int o = 0; o < 6; o++) pad.osc[o].wave = JT_WAVE_SAW;
    for (int v = 0; v < 4; v++) { key[v].a.wave = JT_WAVE_SAW; key[v].b.wave = JT_WAVE_SQUARE; key[v].note = -1; }
    jt_tape_init(&tape, tape_buf, TAPE);
    jt_lpc_init(&vox, LPC_SIN, LPC_GAIN);
    jt_filter(1000, 0);
    jt_audio_init(render);
    jt_usb_init();
    enc_last = jt_encoder();
    anchor[0] = jt_pot(JT_ADC_CUTOFF);
    anchor[1] = jt_pot(JT_ADC_RESONANCE);

    for (;;) {
        jt_poll_escape();
        uint32_t ms = (samples - began) / 32;

        /* hands: read about every 8 ms, however fast the frames come */
        if (samples - polled >= 256) {
            polled = samples;
            uint8_t now = buttons(), pressed = (uint8_t)(now & ~held);
            held = now;
            uint16_t bits = jt_keyboard();
            if (bits && bits == last_bits) { stable = bits; quiet = 0; }
            else if (!bits && ++quiet >= 3) stable = 0;
            last_bits = bits;
            int steps = (jt_encoder() - enc_last) / 8;
            enc_last += steps * 8;
            for (int i = 0; i < 2; i++) {
                int d = jt_pot(i ? JT_ADC_RESONANCE : JT_ADC_CUTOFF) - anchor[i];
                if (d > 90 || d < -90) owns[i] = 1;         /* a knob is yours from the moment you move it */
            }
            if (over) {
                if (pressed & 64) {
                    restart();
                    began = samples;
                    owns[0] = owns[1] = 0;
                    anchor[0] = jt_pot(JT_ADC_CUTOFF);
                    anchor[1] = jt_pot(JT_ADC_RESONANCE);
                    continue;
                }
            } else {
                if (pressed & 1) {                          /* OSC: skip. The next step ends it */
                    jt_irq_off();
                    song_step = END_BAR * 16 - 1;
                    jt_irq_on();
                }
                if (pressed & 64) negative = !negative;
                if (steps) how = ((how + steps) % LOOKS + LOOKS) % LOOKS;
                muted = (now & 2 ? DRUMS : 0) | (now & 4 ? BASS : 0) | (now & 8 ? ARP | LEAD : 0) | (now & 16 ? PAD : 0);
                int k = jt_keyboard_lowest(stable);
                if (k >= 0) key_shift = k - 10;             /* the A key is home; it stays where you leave it */
            }
        }

        if (!over) {
            int bar = song_step < 0 ? 0 : song_step / 16;
            /* the analog filter: closes through the break and opens on the drop; closes at the end */
            int cutoff = 1000, resonance = bar >= 20 && bar < 24 ? 450 : 0;
            if (bar >= 20 && bar < 24) cutoff = 350 + (int)((ms - 20 * BAR_MS) * 650 / (4 * BAR_MS));
            if (bar >= 52) cutoff = 1000 - (int)((ms - 52 * BAR_MS) * 400 / (4 * BAR_MS));
            if (owns[0]) cutoff = jt_pot(JT_ADC_CUTOFF) / 4;
            if (owns[1]) resonance = jt_pot(JT_ADC_RESONANCE) / 4;
            jt_filter((uint16_t)(cutoff < 0 ? 0 : cutoff > 1000 ? 1000 : cutoff), (uint16_t)(resonance > 1000 ? 1000 : resonance));
            jt_leds((uint8_t)(kick_level > 0.5f ? 0x3F : 1u << ((song_step >> 1) % 6)));
            jt_oled_invert(negative != (how == NEGATIVE));
            picture(ms, how);
            continue;
        }

        /* the synth at the end: four voices, the knobs are the filter */
        jt_delay_ms(6);
        jt_oled_invert(false);
        key_shift = 0;
        muted = 0;
        jt_filter((uint16_t)(jt_pot(JT_ADC_CUTOFF) / 4), (uint16_t)(jt_pot(JT_ADC_RESONANCE) / 4));
        int8_t want[4] = {-1, -1, -1, -1};
        int count = 0;
        for (int k = 0; k < JT_KEYBOARD_KEYS && count < 4; k++)
            if (stable & JT_KEYBOARD_BIT(k)) want[count++] = (int8_t)(47 + k);
        while (jt_midi_get(ev)) { }
        for (int v = 0; v < 4; v++) {
            bool keep = false;
            for (int i = 0; i < count; i++) keep |= key[v].note == want[i] && key[v].goal > 0.0f;
            if (!keep) key[v].goal = 0.0f;
        }
        for (int i = 0; i < count; i++) {
            int spare = -1;
            bool have = false;
            for (int v = 0; v < 4; v++) {
                if (key[v].goal > 0.0f && key[v].note == want[i]) have = true;
                if (key[v].goal <= 0.0f && (spare < 0 || key[v].env < key[spare].env)) spare = v;
            }
            if (have || spare < 0) continue;
            float hz = jt_note_hz((float)want[i]);
            jt_osc_hz(&key[spare].a, hz, SR);
            jt_osc_hz(&key[spare].b, hz * 0.501f, SR);
            key[spare].note = want[i];
            key[spare].goal = 1.0f;
        }
        jt_leds((uint8_t)(stable ? 0x3F : 0));
        play_screen(stable);
    }
}
