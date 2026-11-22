/* A trance machine: a seven-saw "supersaw" voice, a trance gate, tempo-synced delay,
 * reverb, and built-in riffs that follow the key you hold.
 *
 *   keyboard / USB MIDI   RIFF mode: hold one key, the riff plays in that key
 *                         ARP modes: hold a chord
 *                         mode off:  play it yourself (four voices)
 *   cutoff, resonance     the analog filter over everything: the build-up sweep
 *   panel buttons         menu pages: SOUND, GATE, FILTER, AMP, RIFF (ARP button), FX (SETTINGS)
 *   SELECT                turn to choose, press to edit
 *   OSC + SETTINGS        back to update mode
 *
 * Notes the riff plays also go out over USB MIDI. Riffs are in riffs.h. */
#include "jt4000.h"
#include "jt_dsp.h"
#include "jt_menu.h"
#include "riffs.h"

#define SR 32000.0f
#define CONTROL_SR (SR / JT_AUDIO_BLOCK)
#define VOICES 4
#define SAWS 7
#define TAPE 8192                                   /* 512 ms at the effects' 16 kHz */

/* ---- everything the menu edits, in one array so a preset is one copy ---- */
enum {
    MODE, DETUNE, SPREAD, SUB, OCTAVE, GLIDE,
    GATE, GATE_DEPTH, LFO_RATE, LFO_CUT,
    CUT, RES, F_ENV, F_A, F_D, F_S,
    A_A, A_D, A_S, A_R, VOLUME,
    PLAY, RIFF_N, BPM, LATCH, ARP_OCT,
    D_TIME, D_FB, D_MIX, R_SIZE, R_MIX,
    NV
};
static int16_t V[NV];
static int16_t preset;

enum { OSC_SUPER, OSC_SAW, OSC_SQUARE };
enum { PLAY_OFF, PLAY_RIFF, PLAY_UP, PLAY_DOWN, PLAY_UPDOWN };
enum { R_UPLIFT, R_OFFBT, R_ROLL, R_ACID, R_PLUCK, R_ANTHEM, R_STABS, R_PAD };

typedef struct { const char *name; int16_t v[NV]; } preset_t;
static const preset_t PRESETS[] = {
    /*            mode det spr sub oct gld | gate dep lfo >cut | cut res env  a  d  s |  a  d   s  r vol | play riff bpm lat ao | dt fb mix rs rm */
    {"SUPER",  {OSC_SUPER, 45, 85,  0, 0, 30,   0, 100, 30,  0,  78, 10, 15,  0, 60, 60,  5, 60, 100, 45, 80, PLAY_RIFF, R_UPLIFT, 138, 0, 2,  2, 45, 35, 60, 30}},
    {"PLUCK",  {OSC_SUPER, 30, 80,  0, 0, 30,   0, 100, 30,  0,  38, 15, 62,  0, 46,  0,  0, 52,   0, 45, 85, PLAY_RIFF, R_PLUCK,  138, 0, 2,  2, 50, 45, 65, 35}},
    {"ANTHEM", {OSC_SUPER, 55, 90, 20, 0, 45,   0, 100, 25, 10,  70, 10, 20, 30, 65, 70, 30, 60, 100, 55, 80, PLAY_RIFF, R_ANTHEM, 138, 0, 2,  3, 50, 40, 75, 45}},
    {"ACID",   {OSC_SAW,    0,  0,  0, -1, 40,  0, 100, 30,  0,  34, 82, 55,  0, 42,  0,  0, 50,  60, 30, 80, PLAY_RIFF, R_ACID,   138, 0, 1,  2, 40, 25, 40, 15}},
    {"GATED",  {OSC_SUPER, 60, 95, 15, 0, 30,   3, 100, 20, 15,  66, 12,  0, 40, 60, 100, 45, 60, 100, 60, 80, PLAY_RIFF, R_PAD,    138, 0, 2,  2, 45, 30, 80, 45}},
    {"STABS",  {OSC_SUPER, 40, 85,  0, 0, 30,   0, 100, 30,  0,  50, 10, 45,  0, 50,  0,  0, 55,   0, 48, 80, PLAY_RIFF, R_STABS,  138, 0, 2,  2, 50, 40, 70, 40}},
    {"BASS",   {OSC_SAW,    0,  0, 70, -1, 30,  0, 100, 30,  0,  40, 20, 40,  0, 45, 20,  0, 50,  70, 30, 85, PLAY_RIFF, R_OFFBT,  138, 0, 1,  1, 20,  0, 30,  8}},
    {"ROLLER", {OSC_SQUARE, 0,  0, 40, -1, 30,  0, 100, 30,  0,  42, 30, 45,  0, 40,  0,  0, 45,  40, 28, 85, PLAY_RIFF, R_ROLL,   138, 0, 1,  1, 25, 10, 30, 10}},
    {"ARP",    {OSC_SUPER, 35, 80,  0, 0, 30,   0, 100, 30,  0,  55, 15, 45,  0, 48,  0,  0, 50,  20, 45, 80, PLAY_UP,   R_UPLIFT, 138, 0, 2,  2, 50, 45, 65, 35}},
};
#define PRESET_COUNT ((int)(sizeof PRESETS / sizeof PRESETS[0]))

/* 16 steps each, first step in the top bit */
static const uint16_t GATES[] = {0xFFFF, 0xAAAA, 0xBBBB, 0x7777, 0x9292, 0xDB6D, 0xEEEE, 0x8888};
static const char *const GATE_NAMES[] = {"off", "8ths", "gallop", "pump", "3-3-2", "synco", "chop", "4ths"};
static const char *const MODES[] = {"super", "saw", "square"};
static const char *const PLAYS[] = {"off", "riff", "up", "down", "updown"};
static const char *const TIMES[] = {"1/16", "1/8", "3/16", "1/4"};
static const float TIME_STEPS[] = {1.0f, 2.0f, 3.0f, 4.0f};
static const char *const ONOFF[] = {"off", "on"};
static const char *preset_names[PRESET_COUNT];
static const char *riff_names[32];

static const jt_menu_item_t SOUND_ITEMS[] = {
    {"preset", &preset, 0, PRESET_COUNT - 1, preset_names}, {"osc", &V[MODE], 0, 2, MODES},
    {"detune", &V[DETUNE], 0, 100, 0}, {"spread", &V[SPREAD], 0, 100, 0}, {"sub", &V[SUB], 0, 100, 0},
    {"octave", &V[OCTAVE], -2, 2, 0}, {"glide", &V[GLIDE], 0, 100, 0},
};
static const jt_menu_item_t GATE_ITEMS[] = {
    {"pattern", &V[GATE], 0, 7, GATE_NAMES}, {"depth", &V[GATE_DEPTH], 0, 100, 0},
    {"lfo rate", &V[LFO_RATE], 0, 100, 0}, {"lfo>cut", &V[LFO_CUT], 0, 100, 0},
};
static const jt_menu_item_t FILTER_ITEMS[] = {
    {"cutoff", &V[CUT], 0, 100, 0}, {"resonance", &V[RES], 0, 100, 0}, {"env amt", &V[F_ENV], 0, 100, 0},
    {"attack", &V[F_A], 0, 100, 0}, {"decay", &V[F_D], 0, 100, 0}, {"sustain", &V[F_S], 0, 100, 0},
};
static const jt_menu_item_t AMP_ITEMS[] = {
    {"attack", &V[A_A], 0, 100, 0}, {"decay", &V[A_D], 0, 100, 0}, {"sustain", &V[A_S], 0, 100, 0},
    {"release", &V[A_R], 0, 100, 0}, {"volume", &V[VOLUME], 0, 100, 0},
};
static jt_menu_item_t RIFF_ITEMS[] = {              /* not const: the riff count is filled in at start-up */
    {"play", &V[PLAY], 0, 4, PLAYS}, {"riff", &V[RIFF_N], 0, 0, riff_names}, {"bpm", &V[BPM], 100, 180, 0},
    {"latch", &V[LATCH], 0, 1, ONOFF}, {"arp oct", &V[ARP_OCT], 1, 3, 0},
};
static const jt_menu_item_t FX_ITEMS[] = {
    {"dly time", &V[D_TIME], 0, 3, TIMES}, {"dly fback", &V[D_FB], 0, 100, 0}, {"dly mix", &V[D_MIX], 0, 100, 0},
    {"rev size", &V[R_SIZE], 0, 100, 0}, {"rev mix", &V[R_MIX], 0, 100, 0},
};
#define PAGE(title, items) {title, items, (int)(sizeof items / sizeof items[0])}
static const jt_menu_page_t PAGES[] = {
    PAGE("SOUND", SOUND_ITEMS), PAGE("GATE", GATE_ITEMS), PAGE("FILTER", FILTER_ITEMS),
    PAGE("AMP", AMP_ITEMS), PAGE("RIFF", RIFF_ITEMS), PAGE("FX", FX_ITEMS),
};

/* ---- the engine: read by the audio interrupt ---- */
typedef struct {
    jt_osc_t saw[SAWS], sub;
    jt_env_t amp, fenv;
    jt_svf_t filter;
    float note, target, level;
    uint32_t age;
    int16_t key;                                    /* MIDI note sounding, -1 once released */
} voice_t;

static voice_t voice[VOICES];
static jt_lfo_t lfo;
static jt_tape_t tape;
static jt_reverb_t reverb;
static int16_t tape_buf[TAPE];
static uint32_t voice_clock;

static struct {
    float detune, centre, side, sub, octave, glide;
    float cut_note, resonance, env_amount, lfo_cut;
    float gate_depth, volume, tick_len, delay, feedback, delay_mix, reverb_mix;
    uint16_t gate;
    uint8_t mode;
} P;

/* The seven saws' tuning offsets, as a fraction of the pitch at full detune. */
static const float SPREADS[SAWS] = {0.0f, -0.0195f, 0.0199f, -0.0628f, 0.0622f, -0.1100f, 0.1075f};

static float ms_from(int v) { return jt_exp2((float)v * 0.13f); }      /* 0..100 -> 1 ms .. 8 s */

static void apply(void) {
    float d = (float)V[DETUNE] * 0.01f, s = (float)V[SPREAD] * 0.01f;
    P.mode = (uint8_t)V[MODE];
    P.detune = d * d * 0.35f;                       /* gentle at first, wide at the top */
    P.centre = P.mode == OSC_SUPER ? 1.0f - 0.55f * s : 1.0f;
    P.side = P.mode == OSC_SUPER ? 0.42f * s : 0.0f;
    P.sub = (float)V[SUB] * 0.01f;
    P.octave = 12.0f * (float)V[OCTAVE];
    P.glide = 1.0f / (1.0f + ms_from(V[GLIDE]) * CONTROL_SR * (1.0f / 3000.0f));
    P.cut_note = 24.0f + (float)V[CUT];
    P.resonance = (float)V[RES] * 0.0095f;
    P.env_amount = (float)V[F_ENV] * 0.72f;
    P.lfo_cut = (float)V[LFO_CUT] * 0.36f;
    P.gate = GATES[V[GATE]];
    P.gate_depth = V[GATE] ? (float)V[GATE_DEPTH] * 0.01f : 0.0f;
    P.volume = (float)V[VOLUME] * 0.01f;
    float step = SR * 15.0f / (float)V[BPM];        /* samples in a 16th note */
    P.tick_len = step * 0.5f;                       /* the clock ticks in 32nd notes */
    P.delay = step * 0.5f * TIME_STEPS[V[D_TIME]];  /* in the effects' 16 kHz samples; a 1/4 below 118 bpm
                                                     * is longer than the tape and gets shortened */
    P.feedback = (float)V[D_FB] * 0.0095f;
    P.delay_mix = (float)V[D_MIX] * 0.01f;
    P.reverb_mix = (float)V[R_MIX] * 0.012f;
    reverb.decay = (float)V[R_SIZE] * 0.01f;
    reverb.damp = 0.35f;
    jt_lfo_hz(&lfo, 0.05f * jt_exp2((float)V[LFO_RATE] * 0.0865f), CONTROL_SR);
    for (int i = 0; i < VOICES; i++) {
        voice_t *x = &voice[i];
        for (int k = 0; k < SAWS; k++) x->saw[k].wave = P.mode == OSC_SQUARE ? JT_WAVE_SQUARE : JT_WAVE_SAW;
        x->sub.wave = JT_WAVE_SQUARE;
        jt_env_set(&x->amp, SR, ms_from(V[A_A]), ms_from(V[A_D]), (float)V[A_S] * 0.01f, ms_from(V[A_R]));
        jt_env_set(&x->fenv, CONTROL_SR, ms_from(V[F_A]), ms_from(V[F_D]), (float)V[F_S] * 0.01f, ms_from(V[A_R]));
    }
}

static void load_preset(int n) {
    for (int i = 0; i < NV; i++) V[i] = PRESETS[n].v[i];
    apply();
}

/* ---- voices: called from the audio interrupt, or with interrupts off ---- */
static void voice_on(int key, int velocity, bool slide) {
    int best = 0;
    float score, best_score = 1e9f;
    if (slide)                                      /* carry on the newest sounding voice, no new attack */
        for (int i = 0; i < VOICES; i++)
            if (voice[i].key >= 0 && voice[i].age == voice_clock) {
                voice[i].target = (float)key;
                voice[i].key = (int16_t)key;
                return;
            }
    for (int i = 0; i < VOICES; i++) {              /* a silent voice, else the quietest released, else the oldest */
        voice_t *x = &voice[i];
        if (x->amp.stage == 0) score = -1.0f;
        else if (x->key < 0) score = x->amp.level;
        else score = 2.0f + (float)x->age * 1e-6f;
        if (score < best_score) { best_score = score; best = i; }
    }
    voice_t *x = &voice[best];
    x->note = x->target = (float)key;
    x->key = (int16_t)key;
    x->level = 0.35f + 0.65f * (float)velocity * (1.0f / 127.0f);
    x->age = ++voice_clock;
    jt_env_gate(&x->amp, true);
    jt_env_gate(&x->fenv, true);
}

static void voice_off(int key) {
    for (int i = 0; i < VOICES; i++)
        if (voice[i].key == key) {
            voice[i].key = -1;
            jt_env_gate(&voice[i].amp, false);
            jt_env_gate(&voice[i].fenv, false);
        }
}

/* ---- the sequencer: riffs and arpeggios, clocked from the audio interrupt ---- */
#define HELD_MAX 8
static struct {
    uint8_t held[HELD_MAX];                         /* keys down, in the order they were pressed */
    uint8_t count, fingers, root, running;
    uint32_t tick;                                  /* 32nd notes since the riff started */
    struct { uint8_t key, left; } on[8];            /* notes sounding, and 32nds until each ends */
    int8_t arp_pos, arp_dir;
    uint8_t last_key;
} seq;

static void seq_note(int key, int velocity, int len, bool slide) {
    if (key < 0 || key > 127) return;
    int slot = -1;
    if (slide)                                      /* the note being slid from is replaced, not stopped */
        for (int i = 0; i < 8; i++)
            if (seq.on[i].left && seq.on[i].key == seq.last_key) { slot = i; jt_midi_send(0x80, seq.last_key, 0); }
    if (slot < 0) {
        slide = false;
        for (int i = 0; i < 8 && slot < 0; i++) if (!seq.on[i].left) slot = i;
        if (slot < 0) return;
    }
    seq.on[slot].key = (uint8_t)key;
    seq.on[slot].left = (uint8_t)len;
    seq.last_key = (uint8_t)key;
    voice_on(key, velocity, slide);
    jt_midi_send(0x90, (uint8_t)key, (uint8_t)velocity);
}

static void seq_silence(void) {
    for (int i = 0; i < 8; i++)
        if (seq.on[i].left) { seq.on[i].left = 0; voice_off(seq.on[i].key); jt_midi_send(0x80, seq.on[i].key, 0); }
}

static void seq_tick(void) {
    for (int i = 0; i < 8; i++)
        if (seq.on[i].left && --seq.on[i].left == 0) { voice_off(seq.on[i].key); jt_midi_send(0x80, seq.on[i].key, 0); }
    if (!seq.running || (seq.tick & 1)) return;     /* notes start on 16ths */

    if (V[PLAY] == PLAY_RIFF) {
        const riff_t *r = &RIFFS[V[RIFF_N]];
        unsigned step = (seq.tick / 2) % r->steps;
        for (unsigned i = 0; i < r->count; i++) {
            const riff_ev_t *e = &r->ev[i];
            if (e->step != step) continue;
            seq_note(seq.root + e->note, (e->flags & RIFF_ACCENT) ? 127 : 92, e->len, e->flags & RIFF_SLIDE);
        }
    } else if (seq.count) {                         /* arpeggio over the held keys, lowest first */
        uint8_t sorted[HELD_MAX];
        int n = seq.count, span = n * V[ARP_OCT];
        for (int i = 0; i < n; i++) {
            int j = i;
            for (; j > 0 && sorted[j - 1] > seq.held[i]; j--) sorted[j] = sorted[j - 1];
            sorted[j] = seq.held[i];
        }
        if (V[PLAY] == PLAY_UP) seq.arp_pos = (int8_t)((seq.arp_pos + 1) % span);
        else if (V[PLAY] == PLAY_DOWN) seq.arp_pos = (int8_t)((seq.arp_pos + span - 1) % span);
        else if (span > 1) {
            if (seq.arp_pos + seq.arp_dir >= span || seq.arp_pos + seq.arp_dir < 0) seq.arp_dir = (int8_t)-seq.arp_dir;
            seq.arp_pos = (int8_t)(seq.arp_pos + seq.arp_dir);
        } else seq.arp_pos = 0;
        if (seq.arp_pos >= span) seq.arp_pos = 0;
        seq_note(sorted[seq.arp_pos % n] + 12 * (seq.arp_pos / n), (seq.tick & 7) ? 92 : 127, 1, false);
    }
}

/* ---- audio ---- */
static float tick_acc;                              /* samples since the last clock tick */
static float fx(float in) {
    static float delay, tone;
    float want = P.delay > TAPE - 4 ? TAPE - 4 : P.delay;
    delay += (want - delay) * 0.002f;
    float d = jt_tape_read(&tape, delay);
    tone += (d - tone) * 0.6f;
    jt_tape_write(&tape, in + tone * P.feedback);
    float wet = d * P.delay_mix;
    return wet + jt_reverb_next(&reverb, in + wet) * P.reverb_mix;
}

static void render(int16_t *out, int n) {
    static float wet_now, wet_prev, half, gate = 1.0f;
    static int phase;

    /* once per block: pitch, filter, modulation */
    float mod = jt_lfo_next(&lfo);
    for (int v = 0; v < VOICES; v++) {
        voice_t *x = &voice[v];
        x->note += (x->target - x->note) * P.glide;
        float hz = jt_note_hz(x->note + P.octave);
        for (int k = 0; k < SAWS; k++) jt_osc_hz(&x->saw[k], hz * (1.0f + SPREADS[k] * P.detune), SR);
        jt_osc_hz(&x->sub, hz * 0.5f, SR);
        float e = jt_env_next(&x->fenv);
        float cut = P.cut_note + e * P.env_amount + mod * P.lfo_cut + 0.4f * (x->note - 60.0f);
        jt_svf_set(&x->filter, jt_note_hz(cut), P.resonance, SR);
    }

    for (int i = 0; i < n; i++) {
        if (++tick_acc >= P.tick_len) {             /* the 32nd-note clock */
            tick_acc -= P.tick_len;
            seq_tick();
            seq.tick++;
        }
        float s = 0.0f;
        for (int v = 0; v < VOICES; v++) {
            voice_t *x = &voice[v];
            float a = jt_env_next(&x->amp);
            if (x->amp.stage == 0) continue;
            float w = jt_osc_next(&x->saw[0]) * P.centre;
            if (P.side > 0.0f) {
                float sides = 0.0f;
                for (int k = 1; k < SAWS; k++) sides += jt_osc_next(&x->saw[k]);
                w += sides * P.side;
            }
            if (P.sub > 0.0f) w += jt_osc_next(&x->sub) * P.sub;
            s += jt_svf_next(&x->filter, w) * a * x->level;
        }

        /* trance gate: each 16th is open for its first half if its pattern bit is set */
        unsigned step = (seq.tick / 2) & 15;
        float open = ((P.gate << step) & 0x8000) && !(seq.tick & 1) ? 1.0f : 1.0f - P.gate_depth;
        gate += (open - gate) * 0.01f;              /* 3 ms edges: no clicks */
        s *= 0.3f * gate * P.volume;

        if (phase ^= 1) {                           /* effects on every second sample */
            half = s;
            out[i] = (int16_t)(jt_softclip(s + 0.5f * (wet_prev + wet_now)) * 30000.0f);
        } else {
            wet_prev = wet_now;
            wet_now = fx(0.5f * (half + s));
            out[i] = (int16_t)(jt_softclip(s + wet_prev) * 30000.0f);
        }
    }
}

/* ---- keys, from the touch keyboard and from MIDI ---- */
static void irq_off(void) { jt_irq_off(); }
static void irq_on(void) { jt_irq_on(); }

/* Without latch the riff or arpeggio runs while keys are down. With latch it keeps going
 * after you let go: a riff follows the last key, an arpeggio keeps its chord until you
 * start a new one. */
static void key_down(int key, int velocity) {
    irq_off();
    if (V[PLAY] == PLAY_OFF) {
        voice_on(key, velocity, false);
    } else {
        if (V[LATCH] && !seq.fingers) seq.count = 0;                 /* a new chord replaces the latched one */
        if (seq.count < HELD_MAX) seq.held[seq.count++] = (uint8_t)key;
        seq.fingers++;
        seq.root = (uint8_t)key;
        if (!seq.running) {                         /* start now, from the top */
            seq.running = 1;
            seq.tick = 0;
            seq.arp_pos = -1;
            seq.arp_dir = 1;
            tick_acc = P.tick_len;
        }
    }
    irq_on();
}

static void key_up(int key) {
    irq_off();
    if (V[PLAY] == PLAY_OFF) {
        voice_off(key);
    } else {
        if (seq.fingers) seq.fingers--;
        if (!V[LATCH]) {
            int n = 0;
            for (int i = 0; i < seq.count; i++) if (seq.held[i] != key) seq.held[n++] = seq.held[i];
            seq.count = (uint8_t)n;
            if (!n) { seq.running = 0; seq_silence(); }
            else seq.root = seq.held[n - 1];
        }
    }
    irq_on();
}

static void stop_all(void) {
    irq_off();
    seq.running = 0;
    seq.count = seq.fingers = 0;
    seq_silence();
    for (int i = 0; i < VOICES; i++) if (voice[i].key >= 0) voice_off(voice[i].key);
    irq_on();
}

static void splash(void) {
    for (int f = 0; f <= 16; f++) {                 /* the name slides in between two closing bars */
        jt_oled_clear();
        jt_oled_text_big(16, 8, "TRANCE", 2);
        for (int y = 0; y < 32; y++)
            for (int x = 0; x < 128; x++)
                if (((x + y * 3) & 15) >= f) jt_oled_pixel(x, y, false);
        jt_oled_line(0, f < 8 ? f : 7, 127, f < 8 ? f : 7, true);
        jt_oled_line(0, 31 - (f < 8 ? f : 7), 127, 31 - (f < 8 ? f : 7), true);
        jt_oled_flush();
        jt_leds((uint8_t)(0x3F >> (f < 6 ? 6 - f : 0)));
        jt_delay_ms(25);
    }
    jt_delay_ms(500);
}

int main(void) {
    static const uint8_t CC[2] = {74, 71};
    int knob_raw[2] = {0, 0}, knob_sent[2] = {-1, -1}, knob_owns[2] = {1, 1}, filt[2] = {1000, 0};
    uint16_t last_bits = 0, stable = 0, playing = 0;
    int quiet = 0, shown_preset = 0, shown_play;
    uint8_t ev[4];

    jt_init();
    for (int i = 0; i < PRESET_COUNT; i++) preset_names[i] = PRESETS[i].name;
    for (int i = 0; i < RIFF_COUNT && i < 32; i++) riff_names[i] = RIFFS[i].name;
    RIFF_ITEMS[1].max = (int16_t)((RIFF_COUNT < 32 ? RIFF_COUNT : 32) - 1);
    for (int i = 0; i < VOICES; i++) {
        voice[i].key = -1;
        for (int k = 0; k < SAWS; k++) voice[i].saw[k].phase = 0.5f + 0.5f * jt_rand();   /* free-running saws */
    }
    jt_tape_init(&tape, tape_buf, TAPE);
    load_preset(0);
    shown_play = V[PLAY];
    jt_audio_init(render);
    jt_usb_init();
    splash();
    jt_menu_init(PAGES, 6);

    for (;;) {
        jt_delay_ms(10);
        jt_poll_escape();

        uint16_t bits = jt_keyboard();
        if (bits && bits == last_bits) { stable = bits; quiet = 0; }
        else if (!bits && ++quiet >= 4) stable = 0;
        last_bits = bits;
        for (int k = 0; k < JT_KEYBOARD_KEYS; k++) {
            uint16_t b = JT_KEYBOARD_BIT(k);
            int key = 47 + k;                       /* the lowest key is B2 */
            if ((stable & b) && !(playing & b)) { key_down(key, 100); if (V[PLAY] == PLAY_OFF) jt_midi_send(0x90, (uint8_t)key, 100); }
            if (!(stable & b) && (playing & b)) { key_up(key); if (V[PLAY] == PLAY_OFF) jt_midi_send(0x80, (uint8_t)key, 0); }
        }
        playing = stable;

        while (jt_midi_get(ev)) {
            int kind = ev[0] & 0x0F;
            if (kind == 9 && ev[3]) key_down(ev[2], ev[3]);
            else if (kind == 8 || kind == 9) key_up(ev[2]);
            else if (kind == 0xB)
                for (int i = 0; i < 2; i++)
                    if (ev[2] == CC[i]) { filt[i] = ev[3] * 1000 / 127; knob_owns[i] = 0; knob_raw[i] = jt_pot(i); }
        }

        for (int i = 0; i < 2; i++) {               /* the knobs: a received CC holds until the knob moves */
            int raw = jt_pot(i == 0 ? JT_ADC_CUTOFF : JT_ADC_RESONANCE);
            int d = raw - knob_raw[i];
            if (!knob_owns[i] && (d > 80 || d < -80)) knob_owns[i] = 1;
            if (!knob_owns[i]) continue;
            knob_raw[i] = raw;
            filt[i] = raw >= 4000 ? 1000 : raw / 4;
            int ds = raw - knob_sent[i];
            if (knob_sent[i] < 0) knob_sent[i] = raw;
            else if (ds > 40 || ds < -40) {
                knob_sent[i] = raw;
                jt_midi_send(0xB0, CC[i], (uint8_t)(raw >= 4000 ? 127 : raw * 127 / 4000));
            }
        }
        jt_filter((uint16_t)filt[0], (uint16_t)filt[1]);

        if (jt_menu_poll()) {
            if (preset != shown_preset) {           /* a preset replaces every setting, riff included */
                shown_preset = preset;
                stop_all();
                load_preset(preset);
                jt_menu_redraw();
            }
            if (V[PLAY] != shown_play || (!V[LATCH] && !seq.fingers && seq.running)) stop_all();
            shown_play = V[PLAY];
            apply();
        }
    }
}
