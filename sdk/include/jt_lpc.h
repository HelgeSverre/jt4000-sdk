/* JT-4000 SDK: LPC speech, the method of the Speak & Spell.
 * Plays words made by tools/lpcwords.py (a words.h per firmware). The pitch is not part of
 * a word, so every word can be spoken at any note. examples/speak has the full story. */
#ifndef JT_LPC_H
#define JT_LPC_H

#include <stdbool.h>
#include <stdint.h>

#define JT_LPC_ORDER 10

typedef struct {
    /* set these any time */
    float hz;                              /* pitch */
    float ratio;                           /* size of the mouth: 0.66 giant, 1 as recorded, 1.5 child */
    float speed;                           /* 1 as recorded, 2 half speed, 0.5 double */
    /* read these */
    volatile bool active;                  /* a word is being spoken */
    /* private */
    const float *sin_tab, *gain_tab;
    const uint8_t *volatile pending;
    const uint8_t *p;
    volatile bool cut;
    int frames, frames_left, ticks_left;
    bool voiced, tail;
    float k[JT_LPC_ORDER], dk[JT_LPC_ORDER], g, dg;
    float b[JT_LPC_ORDER + 1], y1;
    float phase, inc, amp, carry;
    float cur, prev, acc, step;
} jt_lpc_t;

/* sin_tab and gain_tab are LPC_SIN and LPC_GAIN from the firmware's words.h */
void jt_lpc_init(jt_lpc_t *l, const float *sin_tab, const float *gain_tab);
/* Start a word: &WORD_DATA[WORD_AT[n]]. Cuts off the word being spoken, without a click. */
static inline void jt_lpc_say(jt_lpc_t *l, const uint8_t *word) { l->pending = word; }
static inline void jt_lpc_stop(jt_lpc_t *l) { l->cut = true; }
static inline bool jt_lpc_busy(const jt_lpc_t *l) { return l->active || l->pending; }
/* How far through the word, 0..1. */
float jt_lpc_progress(const jt_lpc_t *l);
/* In the audio interrupt: jt_lpc_block once per block, then jt_lpc_next for every sample. */
void jt_lpc_block(jt_lpc_t *l);
float jt_lpc_next(jt_lpc_t *l);            /* about -0.5..0.5 */

/* ---- the pieces, for firmware that does something else with the frames (examples/choir) ---- */
int jt_lpc_frames(const uint8_t *word);    /* how many 20 ms frames a word has */
/* Frame `index` of a word: its ten reflection coefficients, excitation level and whether it
 * is voiced. A silent frame gives a level of 0 and leaves k alone. */
void jt_lpc_frame(const uint8_t *word, int index, const float *sin_tab, const float *gain_tab,
                  float k[JT_LPC_ORDER], float *level, bool *voiced);
/* The vocal tract itself: feed it pulses or noise at 8 kHz, one sample at a time. */
typedef struct { float b[JT_LPC_ORDER + 1], y1; } jt_lattice_t;
float jt_lattice_next(jt_lattice_t *f, const float k[JT_LPC_ORDER], float x);

#endif
