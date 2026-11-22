/* LPC speech player. The frame format is tools/lpcwords.py's: 8 kHz, 20 ms frames, a
 * count byte, then per frame a voiced bit + 7-bit gain code and (unless the gain is 0)
 * ten reflection coefficients when voiced, six when not. */
#include "jt_lpc.h"
#include "jt4000.h"
#include "jt_dsp.h"

#define RATE 8000.0f
#define FRAME 160.0f
#define ORDER JT_LPC_ORDER
#define ORDER_UNVOICED 6
#define PRE 0.9f                            /* the encoder's pre-emphasis, undone here */

void jt_lpc_init(jt_lpc_t *l, const float *sin_tab, const float *gain_tab) {
    l->sin_tab = sin_tab;
    l->gain_tab = gain_tab;
    if (l->hz <= 0.0f) l->hz = 110.0f;
    if (l->ratio <= 0.0f) l->ratio = 1.0f;
    if (l->speed <= 0.0f) l->speed = 1.0f;
    jt_lpc_block(l);
}

float jt_lpc_progress(const jt_lpc_t *l) {
    return l->frames ? 1.0f - (float)l->frames_left / (float)l->frames : 0.0f;
}

static void next_frame(jt_lpc_t *l) {
    float tk[ORDER], tg = 0.0f;
    bool v = l->voiced;
    if (l->pending) {
        l->p = l->pending + 1;
        l->frames = l->frames_left = l->pending[0];
        l->pending = 0;
        l->active = true;
        l->tail = false;
        l->cut = false;
    }
    if (l->cut) { l->frames_left = 0; l->cut = false; }
    l->ticks_left = (int)(FRAME * l->speed * l->ratio);
    if (l->ticks_left < 16) l->ticks_left = 16;
    for (int i = 0; i < ORDER; i++) tk[i] = l->k[i];
    if (!l->active) {
        l->g = l->dg = 0.0f;
        for (int i = 0; i < ORDER; i++) l->dk[i] = 0.0f;
        return;
    }
    if (l->frames_left == 0) {              /* one more frame to fade out, then rest */
        if (l->tail) { l->active = false; l->g = l->dg = 0.0f; return; }
        l->tail = true;
    } else {
        uint8_t head = *l->p++;
        int gain = head & 0x7F;
        v = head & 0x80;
        l->frames_left--;
        if (gain) {
            int n = v ? ORDER : ORDER_UNVOICED;
            for (int i = 0; i < ORDER; i++) {
                int8_t c = i < n ? (int8_t)*l->p++ : 0;
                tk[i] = c < 0 ? -l->sin_tab[-c > 127 ? 127 : -c] : l->sin_tab[c];
            }
            tg = l->gain_tab[gain];
        }
    }
    if (v != l->voiced || l->g < 1e-7f)     /* no gliding between a hiss and a vowel */
        for (int i = 0; i < ORDER; i++) l->k[i] = tk[i];
    l->voiced = v;
    float per_tick = 1.0f / (float)l->ticks_left;
    for (int i = 0; i < ORDER; i++) l->dk[i] = (tk[i] - l->k[i]) * per_tick;
    l->dg = (tg - l->g) * per_tick;
}

/* one sample at the speech clock (8 kHz times the mouth-size ratio) */
static float tick(jt_lpc_t *l) {
    if (--l->ticks_left <= 0) next_frame(l);
    l->g += l->dg;
    for (int i = 0; i < ORDER; i++) l->k[i] += l->dk[i];

    float e = l->carry;
    l->carry = 0.0f;
    if (l->voiced) {
        l->phase += l->inc;
        if (l->phase >= 1.0f) {             /* a glottal pulse, placed between two samples */
            l->phase -= 1.0f;
            float late = l->phase / l->inc;
            e += l->amp * late;
            l->carry = l->amp * (1.0f - late);
        }
    } else {
        e += 1.732f * jt_rand();
    }
    float s = l->g * e;
    for (int i = ORDER - 1; i >= 0; i--) {
        s -= l->k[i] * l->b[i];
        l->b[i + 1] = l->b[i] + l->k[i] * s;
    }
    l->b[0] = s;
    l->y1 = s + PRE * l->y1;
    return l->y1;
}

int jt_lpc_frames(const uint8_t *word) { return word[0]; }

void jt_lpc_frame(const uint8_t *word, int index, const float *sin_tab, const float *gain_tab,
                  float k[ORDER], float *level, bool *voiced) {
    const uint8_t *p = word + 1;
    if (index < 0) index = 0;
    if (index >= word[0]) index = word[0] - 1;
    for (int i = 0; i < index; i++) {       /* frames differ in length: walk to the one wanted */
        uint8_t head = *p++;
        if (head & 0x7F) p += head & 0x80 ? ORDER : ORDER_UNVOICED;
    }
    uint8_t head = *p++;
    *voiced = head & 0x80;
    *level = 0.0f;
    if (!(head & 0x7F)) return;
    *level = gain_tab[head & 0x7F];
    int n = *voiced ? ORDER : ORDER_UNVOICED;
    for (int i = 0; i < ORDER; i++) {
        int8_t c = i < n ? (int8_t)p[i] : 0;
        k[i] = c < 0 ? -sin_tab[-c > 127 ? 127 : -c] : sin_tab[c];
    }
}

float jt_lattice_next(jt_lattice_t *f, const float k[ORDER], float x) {
    for (int i = ORDER - 1; i >= 0; i--) {
        x -= k[i] * f->b[i];
        f->b[i + 1] = f->b[i] + k[i] * x;
    }
    f->b[0] = x;
    f->y1 = x + PRE * f->y1;
    return f->y1;
}

void jt_lpc_block(jt_lpc_t *l) {
    float ratio = l->ratio;
    l->step = ratio * (RATE / (float)JT_SAMPLE_RATE);
    l->inc = l->hz / (RATE * ratio);
    if (l->inc > 0.45f) l->inc = 0.45f;
    if (l->inc < 0.002f) l->inc = 0.002f;
    l->amp = jt_sqrt(1.0f / l->inc);        /* keeps the loudness the same at every pitch */
}

float jt_lpc_next(jt_lpc_t *l) {
    l->acc += l->step;
    if (l->acc >= 1.0f) {
        l->acc -= 1.0f;
        l->prev = l->cur;
        l->cur = tick(l);
    }
    return l->prev + (l->cur - l->prev) * l->acc;
}
