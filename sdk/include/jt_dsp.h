/* JT-4000 SDK: sound building blocks. All single-precision float (the chip has an FPU),
 * no C library needed. Signals are -1..1. Each block is a small struct you own plus a
 * "next" function that produces one sample, so they combine freely.
 *
 * Memory is the tight resource: 32 KiB of RAM in total. The tape (delay memory) and the
 * reverb store 16-bit samples, and are meant to run at half the sample rate (16 kHz). */
#ifndef JT_DSP_H
#define JT_DSP_H

#include <stdbool.h>
#include <stdint.h>

/* ---- maths ---- */
float jt_exp2(float x);                    /* 2^x */
float jt_note_hz(float note);              /* MIDI note number (69 = 440 Hz), fractions allowed */
float jt_sin(float phase);                 /* sine of a 0..1 phase (one cycle) */
float jt_rand(void);                       /* white noise, -1..1 */
float jt_softclip(float x);                /* smooth limit to -1..1 */

/* ---- oscillator ---- */
enum { JT_WAVE_SAW, JT_WAVE_SQUARE, JT_WAVE_TRIANGLE, JT_WAVE_SINE, JT_WAVE_NOISE, JT_WAVES };
typedef struct {
    float phase, inc;                      /* 0..1, and the step per sample */
    uint8_t wave;
} jt_osc_t;
static inline void jt_osc_hz(jt_osc_t *o, float hz, float rate) { o->inc = hz / rate; }
float jt_osc_next(jt_osc_t *o);            /* saw and square are band-limited */

/* ---- envelope: attack, decay, sustain, release ---- */
typedef struct {
    float a, d, s, r, level;
    uint8_t stage;                         /* 0 idle, 1 attack, 2 decay/sustain, 3 release */
} jt_env_t;
void jt_env_set(jt_env_t *e, float rate, float attack_ms, float decay_ms, float sustain, float release_ms);
void jt_env_gate(jt_env_t *e, bool on);
float jt_env_next(jt_env_t *e);            /* 0..1 */

/* ---- low-frequency oscillator ---- */
enum { JT_LFO_SINE, JT_LFO_TRIANGLE, JT_LFO_SQUARE, JT_LFO_RANDOM, JT_LFOS };
typedef struct {
    float phase, inc, held;
    uint8_t wave;
} jt_lfo_t;
static inline void jt_lfo_hz(jt_lfo_t *l, float hz, float rate) { l->inc = hz / rate; }
float jt_lfo_next(jt_lfo_t *l);            /* -1..1 */

/* ---- digital filter (state variable): stays stable while swept ---- */
typedef struct {
    float a1, a2, a3, k, s1, s2;
    float low, band, high;                 /* the three outputs of the last sample */
} jt_svf_t;
void jt_svf_set(jt_svf_t *f, float hz, float resonance, float rate);   /* resonance 0..1 */
float jt_svf_next(jt_svf_t *f, float x);   /* returns the low-pass output */

/* ---- tape: a loop of recent audio, for delays and grains ---- */
typedef struct {
    int16_t *buf;
    uint32_t mask, w;
} jt_tape_t;
void jt_tape_init(jt_tape_t *t, int16_t *buf, uint32_t size);   /* size: a power of two */
void jt_tape_write(jt_tape_t *t, float x);
float jt_tape_read(const jt_tape_t *t, float delay);            /* samples back, fractions allowed */

/* ---- grains: short overlapping slices of the tape, replayed at another pitch ---- */
#define JT_GRAINS 4
typedef struct {
    struct { float delay, age, len; } g[JT_GRAINS];   /* len 0 = free */
    float size, pitch, spread;             /* samples, speed ratio (2 = octave up), samples */
    float wait;
} jt_grains_t;
float jt_grains_next(jt_grains_t *c, const jt_tape_t *t);

/* ---- reverb: four cross-fed delay lines, 5 KiB of its own memory, one instance ----
 * Lengths are tuned for a 16 kHz call rate. */
typedef struct {
    float decay, damp;                     /* 0..1 each: tail length, high-frequency loss */
    float lp[4], lfo;
    uint16_t w[6];
} jt_reverb_t;
float jt_reverb_next(jt_reverb_t *r, float x);

#endif
