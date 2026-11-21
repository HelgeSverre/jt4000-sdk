/* Sound building blocks; see jt_dsp.h. */
#include "jt_dsp.h"

/* ---- maths ---- */
float jt_exp2(float x) {
    if (x > 30.0f) x = 30.0f;
    if (x < -30.0f) x = -30.0f;
    int i = (int)x;
    if (x < (float)i) i--;
    float f = x - (float)i;                         /* 0..1: series for e^(f ln 2) */
    float r = 1.0f + f * (0.69314718f + f * (0.24022651f + f * (0.05550411f + f * (0.00961813f + f * 0.00133336f))));
    for (; i > 0; i--) r *= 2.0f;
    for (; i < 0; i++) r *= 0.5f;
    return r;
}

float jt_note_hz(float note) { return 440.0f * jt_exp2((note - 69.0f) * (1.0f / 12.0f)); }

float jt_sin(float phase) {
    float x = 2.0f * phase - 1.0f;                  /* -1..1 */
    float ax = x < 0 ? -x : x;
    float y = 4.0f * x * (1.0f - ax);               /* parabola through the sine's peaks */
    float ay = y < 0 ? -y : y;
    y = 0.225f * (y * ay - y) + y;                  /* correction: error under 0.1 % */
    return -y;
}

float jt_rand(void) {
    static uint32_t s = 0x1234ABCD;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return (float)(int32_t)s * (1.0f / 2147483648.0f);
}

float jt_softclip(float x) {
    if (x > 1.0f) return 1.0f;
    if (x < -1.0f) return -1.0f;
    return x * (1.5f - 0.5f * x * x);
}

/* ---- oscillator ---- */
/* Rounds off the step of a saw or square over the two samples around it. */
static float blep(float t, float dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}

float jt_osc_next(jt_osc_t *o) {
    float p = o->phase, dt = o->inc, y;
    switch (o->wave) {
    case JT_WAVE_SAW:
        y = 2.0f * p - 1.0f - blep(p, dt);
        break;
    case JT_WAVE_SQUARE: {
        float p2 = p + 0.5f;
        if (p2 >= 1.0f) p2 -= 1.0f;
        y = (p < 0.5f ? 1.0f : -1.0f) + blep(p, dt) - blep(p2, dt);
        break;
    }
    case JT_WAVE_TRIANGLE:
        y = p < 0.5f ? 4.0f * p - 1.0f : 3.0f - 4.0f * p;
        break;
    case JT_WAVE_SINE:
        y = jt_sin(p);
        break;
    default:
        y = jt_rand();
    }
    p += dt;
    if (p >= 1.0f) p -= 1.0f;
    o->phase = p;
    return y;
}

/* ---- envelope ---- */
/* Each stage glides towards its target; "ms" is the time to get about 95 % of the way. */
static float glide(float rate, float ms) { return 1.0f / (1.0f + ms * rate * (1.0f / 3000.0f)); }

void jt_env_set(jt_env_t *e, float rate, float attack_ms, float decay_ms, float sustain, float release_ms) {
    e->a = glide(rate, attack_ms);
    e->d = glide(rate, decay_ms);
    e->r = glide(rate, release_ms);
    e->s = sustain;
}

void jt_env_gate(jt_env_t *e, bool on) {
    if (on) e->stage = 1;
    else if (e->stage) e->stage = 3;
}

float jt_env_next(jt_env_t *e) {
    switch (e->stage) {
    case 1:
        e->level += (1.2f - e->level) * e->a;       /* aim past the top so the attack ends */
        if (e->level >= 1.0f) { e->level = 1.0f; e->stage = 2; }
        break;
    case 2:
        e->level += (e->s - e->level) * e->d;
        break;
    case 3:
        e->level -= e->level * e->r;
        if (e->level < 0.0003f) { e->level = 0.0f; e->stage = 0; }
        break;
    }
    return e->level;
}

/* ---- low-frequency oscillator ---- */
float jt_lfo_next(jt_lfo_t *l) {
    float p = l->phase, y;
    switch (l->wave) {
    case JT_LFO_SINE:     y = jt_sin(p); break;
    case JT_LFO_TRIANGLE: y = p < 0.5f ? 4.0f * p - 1.0f : 3.0f - 4.0f * p; break;
    case JT_LFO_SQUARE:   y = p < 0.5f ? 1.0f : -1.0f; break;
    default:              y = l->held;
    }
    p += l->inc;
    if (p >= 1.0f) { p -= 1.0f; l->held = jt_rand(); }
    l->phase = p;
    return y;
}

/* ---- digital filter ---- */
void jt_svf_set(jt_svf_t *f, float hz, float resonance, float rate) {
    float x = 3.14159265f * hz / rate;
    if (x > 1.2f) x = 1.2f;                         /* about 0.38 of the sample rate */
    if (x < 0.0005f) x = 0.0005f;
    float x2 = x * x;
    float g = x * (1.0f + x2 * (0.33333333f + x2 * (0.13333333f + x2 * 0.05396825f)));   /* tan(x) */
    if (resonance < 0.0f) resonance = 0.0f;
    if (resonance > 1.0f) resonance = 1.0f;
    f->k = 2.0f - 1.96f * resonance;
    f->a1 = 1.0f / (1.0f + g * (g + f->k));
    f->a2 = g * f->a1;
    f->a3 = g * f->a2;
}

float jt_svf_next(jt_svf_t *f, float x) {
    float v3 = x - f->s2;
    float v1 = f->a1 * f->s1 + f->a2 * v3;
    float v2 = f->s2 + f->a2 * f->s1 + f->a3 * v3;
    f->s1 = 2.0f * v1 - f->s1;
    f->s2 = 2.0f * v2 - f->s2;
    f->low = v2;
    f->band = v1;
    f->high = x - f->k * v1 - v2;
    return v2;
}

/* ---- tape ---- */
static int16_t to_i16(float x) {
    x *= 32767.0f;
    if (x > 32767.0f) x = 32767.0f;
    if (x < -32767.0f) x = -32767.0f;
    return (int16_t)x;
}

void jt_tape_init(jt_tape_t *t, int16_t *buf, uint32_t size) {
    t->buf = buf;
    t->mask = size - 1;
    t->w = 0;
    for (uint32_t i = 0; i < size; i++) buf[i] = 0;
}

void jt_tape_write(jt_tape_t *t, float x) {
    t->buf[t->w] = to_i16(x);
    t->w = (t->w + 1) & t->mask;
}

float jt_tape_read(const jt_tape_t *t, float delay) {
    float max = (float)(t->mask - 1);
    if (delay < 1.0f) delay = 1.0f;
    if (delay > max) delay = max;
    uint32_t whole = (uint32_t)delay;
    float frac = delay - (float)whole;
    uint32_t i = (t->w - whole) & t->mask;
    float a = t->buf[i], b = t->buf[(i - 1) & t->mask];
    return (a + (b - a) * frac) * (1.0f / 32767.0f);
}

/* ---- grains ---- */
float jt_grains_next(jt_grains_t *c, const jt_tape_t *t) {
    float out = 0.0f;
    if (--c->wait <= 0.0f) {
        float len = c->size < 32.0f ? 32.0f : c->size;
        float drift = (1.0f - c->pitch) * len;      /* how far the read point moves during a grain */
        float room = (float)t->mask - 4.0f - (drift < 0 ? -drift : drift);
        float start = 2.0f + (drift < 0 ? -drift : 0.0f);
        float jitter = c->spread * (0.5f + 0.5f * jt_rand());
        if (jitter > room) jitter = room > 0 ? room : 0;
        c->wait = len * (1.0f / (JT_GRAINS - 1)) * (1.0f + 0.4f * jt_rand());
        for (int i = 0; i < JT_GRAINS; i++) {
            if (c->g[i].len > 0.0f) continue;
            c->g[i].len = len;
            c->g[i].age = 0.0f;
            c->g[i].delay = start + jitter;
            break;
        }
    }
    for (int i = 0; i < JT_GRAINS; i++) {
        if (c->g[i].len <= 0.0f) continue;
        float s = jt_sin(0.5f * c->g[i].age / c->g[i].len);     /* half a sine cycle: fade in and out */
        out += s * s * jt_tape_read(t, c->g[i].delay);
        c->g[i].delay += 1.0f - c->pitch;
        if (++c->g[i].age >= c->g[i].len) c->g[i].len = 0.0f;
    }
    return out * 0.6f;
}

/* ---- reverb ---- */
#define AP1 89
#define AP2 131
static const uint16_t LINE[4] = {443, 577, 691, 809};          /* 28..51 ms at 16 kHz */
static int16_t ap1[AP1], ap2[AP2], line0[443], line1[577], line2[691], line3[809];
static int16_t *const LINES[4] = {line0, line1, line2, line3};

static float allpass(int16_t *buf, uint16_t *w, int len, float x) {
    float z = buf[*w] * (1.0f / 32767.0f);
    float v = x + 0.6f * z;
    buf[*w] = to_i16(v);
    if (++*w >= len) *w = 0;
    return z - 0.6f * v;
}

float jt_reverb_next(jt_reverb_t *r, float x) {
    float o[4], sum = 0.0f;
    x = allpass(ap1, &r->w[4], AP1, x * 0.3f);
    x = allpass(ap2, &r->w[5], AP2, x);

    r->lfo += 0.00004f;                             /* 0.6 Hz: a slow wobble on two lines, to blur the tail */
    if (r->lfo >= 1.0f) r->lfo -= 1.0f;
    float wob = 4.0f + 3.5f * jt_sin(r->lfo);

    for (int i = 0; i < 4; i++) {
        int len = LINE[i], w = r->w[i];
        if (i & 1) {
            o[i] = LINES[i][w] * (1.0f / 32767.0f);             /* oldest sample: full length */
        } else {
            float d = i ? wob : 8.0f - wob;                     /* read a little early, by a moving amount */
            int whole = (int)d;
            float frac = d - (float)whole;
            int a = w + whole, b;
            if (a >= len) a -= len;
            b = a + 1;
            if (b >= len) b -= len;
            o[i] = (LINES[i][a] + (LINES[i][b] - LINES[i][a]) * frac) * (1.0f / 32767.0f);
        }
        sum += o[i];
    }
    sum *= 0.5f;
    float g = 0.6f + 0.385f * r->decay, keep = 1.0f - 0.85f * r->damp;
    for (int i = 0; i < 4; i++) {
        r->lp[i] += (o[i] - sum - r->lp[i]) * keep;             /* each line hears the others, minus itself */
        LINES[i][r->w[i]] = to_i16(x + r->lp[i] * g);
        if (++r->w[i] >= LINE[i]) r->w[i] = 0;
    }
    return (o[0] - o[1] + o[2] - o[3]) * 0.5f;
}
