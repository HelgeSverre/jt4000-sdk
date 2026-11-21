/* An ambient synth: four voices, two oscillators each, a digital filter with its own
 * envelope, then a delay, grains and a reverb, and finally the analog filter.
 *
 *   keyboard / USB MIDI   notes (four at once)
 *   cutoff, resonance     the analog filter, last in the chain; also CC 74 / 71 both ways
 *   panel buttons         menu pages: OSC, LFO, FILTER, AMP, GRAIN (ARP button), FX (SETTINGS)
 *   SELECT                turn to choose, press to edit
 *   OSC + SETTINGS        back to update mode
 *
 * The effects run at half the sample rate (16 kHz) to fit their memory in 32 KiB of RAM. */
#include "jt4000.h"
#include "jt_dsp.h"
#include "jt_menu.h"

#define SR 32000.0f
#define FX_SR 16000.0f
#define CONTROL_SR (SR / JT_AUDIO_BLOCK)           /* 500 Hz: once per audio block */
#define VOICES 4
#define TAPE 8192                                   /* 512 ms at 16 kHz */

/* ---- settings edited in the menu ---- */
static int16_t wave1 = JT_WAVE_SAW, wave2 = JT_WAVE_SAW, semi2 = 0, detune = 12, mix2 = 80, noise = 0, octave = 0;
static int16_t lfo_rate = 35, lfo_wave = 0, lfo_pitch = 0, lfo_filter = 20;
static int16_t f_cut = 55, f_res = 20, f_env = 40, f_a = 60, f_d = 75, f_s = 40;
static int16_t a_a = 55, a_d = 60, a_s = 80, a_r = 80, volume = 80;
static int16_t g_mix = 0, g_size = 50, g_pitch = 3, g_spread = 50;
static int16_t d_time = 80, d_fb = 55, d_mix = 35, r_size = 85, r_damp = 40, r_mix = 60;

static const char *const WAVES[] = {"saw", "square", "tri", "sine"};
static const char *const LFOS[] = {"sine", "tri", "square", "random"};
static const char *const PITCHES[] = {"-12", "-5", "0", "+12", "+19", "+24"};
static const float PITCH_RATIO[] = {0.5f, 0.7491535f, 1.0f, 2.0f, 2.9966142f, 4.0f};

static const jt_menu_item_t OSC_ITEMS[] = {
    {"wave 1", &wave1, 0, 3, WAVES}, {"wave 2", &wave2, 0, 3, WAVES}, {"semi 2", &semi2, -24, 24, 0},
    {"detune", &detune, 0, 50, 0},   {"level 2", &mix2, 0, 100, 0},   {"noise", &noise, 0, 100, 0},
    {"octave", &octave, -2, 2, 0},
};
static const jt_menu_item_t LFO_ITEMS[] = {
    {"rate", &lfo_rate, 0, 100, 0}, {"wave", &lfo_wave, 0, 3, LFOS},
    {"to pitch", &lfo_pitch, 0, 100, 0}, {"to filt", &lfo_filter, 0, 100, 0},
};
static const jt_menu_item_t FILTER_ITEMS[] = {
    {"cutoff", &f_cut, 0, 100, 0},  {"resonance", &f_res, 0, 100, 0}, {"env amt", &f_env, 0, 100, 0},
    {"attack", &f_a, 0, 100, 0},    {"decay", &f_d, 0, 100, 0},       {"sustain", &f_s, 0, 100, 0},
};
static const jt_menu_item_t AMP_ITEMS[] = {
    {"attack", &a_a, 0, 100, 0}, {"decay", &a_d, 0, 100, 0}, {"sustain", &a_s, 0, 100, 0},
    {"release", &a_r, 0, 100, 0}, {"volume", &volume, 0, 100, 0},
};
static const jt_menu_item_t GRAIN_ITEMS[] = {
    {"mix", &g_mix, 0, 100, 0}, {"size", &g_size, 0, 100, 0},
    {"pitch", &g_pitch, 0, 5, PITCHES}, {"spread", &g_spread, 0, 100, 0},
};
static const jt_menu_item_t FX_ITEMS[] = {
    {"dly time", &d_time, 0, 100, 0}, {"dly fback", &d_fb, 0, 100, 0}, {"dly mix", &d_mix, 0, 100, 0},
    {"rev size", &r_size, 0, 100, 0}, {"rev damp", &r_damp, 0, 100, 0}, {"rev mix", &r_mix, 0, 100, 0},
};
#define PAGE(title, items) {title, items, (int)(sizeof items / sizeof items[0])}
static const jt_menu_page_t PAGES[] = {
    PAGE("OSC", OSC_ITEMS), PAGE("LFO", LFO_ITEMS), PAGE("FILTER", FILTER_ITEMS),
    PAGE("AMP", AMP_ITEMS), PAGE("GRAIN", GRAIN_ITEMS), PAGE("FX", FX_ITEMS),
};

/* ---- the sound engine: these are read by the audio interrupt ---- */
typedef struct {
    jt_osc_t o1, o2;
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
    float ratio2, level2, noise, octave;
    float lfo_pitch, lfo_filter, cutoff_note, resonance, env_amount;
    float volume, delay, feedback, delay_mix, grain_mix, reverb_mix;
} P;
static uint32_t clock;
volatile uint32_t audio_blocks;                     /* a 2 ms clock for the intro */
void intro(void);

static float ms_from(int v) { return jt_exp2((float)v * 0.13f); }      /* 0..100 -> 1 ms .. 8 s */

/* Turn the menu's numbers into what the engine uses. */
static void apply(void) {
    for (int i = 0; i < VOICES; i++) {
        voice[i].o1.wave = (uint8_t)wave1;
        voice[i].o2.wave = (uint8_t)wave2;
        jt_env_set(&voice[i].env, SR, ms_from(a_a), ms_from(a_d), (float)a_s * 0.01f, ms_from(a_r));
    }
    jt_env_set(&filter_env, CONTROL_SR, ms_from(f_a), ms_from(f_d), (float)f_s * 0.01f, ms_from(a_r));
    P.ratio2 = jt_exp2(((float)semi2 + (float)detune * 0.01f) * (1.0f / 12.0f));
    P.level2 = (float)mix2 * 0.01f;
    P.noise = (float)noise * 0.01f;
    P.octave = 12.0f * (float)octave;
    lfo.wave = (uint8_t)lfo_wave;
    jt_lfo_hz(&lfo, 0.05f * jt_exp2((float)lfo_rate * 0.0865f), CONTROL_SR);   /* 0.05 .. 20 Hz */
    P.lfo_pitch = (float)(lfo_pitch * lfo_pitch) * 0.0012f;                   /* up to 12 semitones */
    P.lfo_filter = (float)lfo_filter * 0.36f;                                 /* up to 3 octaves */
    P.cutoff_note = 24.0f + (float)f_cut * 1.0f;                              /* 33 Hz .. 10 kHz */
    P.resonance = (float)f_res * 0.0095f;
    P.env_amount = (float)f_env * 0.72f;                                      /* up to 6 octaves */
    P.volume = (float)volume * 0.01f;
    P.delay = 16.0f * (20.0f + (float)d_time * 4.8f);                         /* 20 .. 500 ms, in 16 kHz samples */
    P.feedback = (float)d_fb * 0.0095f;
    P.delay_mix = (float)d_mix * 0.01f;
    P.grain_mix = (float)g_mix * 0.012f;
    P.reverb_mix = (float)r_mix * 0.012f;
    grains.pitch = PITCH_RATIO[g_pitch];
    float size = 16.0f * (20.0f + (float)g_size * 2.3f);                      /* 20 .. 250 ms */
    float most = (TAPE - 600.0f) / (grains.pitch > 1.0f ? grains.pitch - 1.0f : 1.0f);
    grains.size = size > most ? most : size;                                  /* a fast grain must fit on the tape */
    grains.spread = (float)g_spread * 30.0f;
    reverb.decay = (float)r_size * 0.01f;
    reverb.damp = (float)r_damp * 0.01f;
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
    /* once per block: modulation */
    float mod = jt_lfo_next(&lfo), any = 0;
    for (int v = 0; v < VOICES; v++) {
        voice_t *x = &voice[v];
        if (x->key >= 0) any = 1;
        float hz = jt_note_hz(x->note + P.octave + mod * P.lfo_pitch);
        jt_osc_hz(&x->o1, hz, SR);
        jt_osc_hz(&x->o2, hz * P.ratio2, SR);
    }
    if (any && filter_env.stage != 1 && filter_env.stage != 2) jt_env_gate(&filter_env, true);
    if (!any) jt_env_gate(&filter_env, false);
    float e = jt_env_next(&filter_env);
    jt_svf_set(&filter, jt_note_hz(P.cutoff_note + e * P.env_amount + mod * P.lfo_filter), P.resonance, SR);

    for (int i = 0; i < n; i++) {
        float s = 0.0f;
        for (int v = 0; v < VOICES; v++) {
            voice_t *x = &voice[v];
            float a = jt_env_next(&x->env);
            if (x->env.stage == 0) continue;
            float w = jt_osc_next(&x->o1) + jt_osc_next(&x->o2) * P.level2;
            if (P.noise > 0.0f) w += jt_rand() * P.noise;
            s += w * a * x->level;
        }
        s = jt_svf_next(&filter, s * 0.22f) * P.volume;

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

/* ---- notes ---- */
static void irq_off(void) { __asm__ volatile("cpsid i"); }
static void irq_on(void) { __asm__ volatile("cpsie i"); }

void note_on(int key, int velocity) {
    int best = 0;
    float score, best_score = 1e9f;
    for (int i = 0; i < VOICES; i++) {              /* take a silent voice, else the quietest released, else the oldest */
        voice_t *x = &voice[i];
        if (x->env.stage == 0) score = -1.0f;
        else if (x->key < 0) score = x->env.level;
        else score = 2.0f + (float)x->age * 1e-9f - (float)(clock - x->age) * 1e-6f;
        if (score < best_score) { best_score = score; best = i; }
    }
    irq_off();
    voice[best].note = (float)key;
    voice[best].key = (int8_t)key;
    voice[best].level = 0.3f + 0.7f * (float)velocity * (1.0f / 127.0f);
    voice[best].age = ++clock;
    jt_env_gate(&voice[best].env, true);
    irq_on();
}

void note_off(int key) {
    irq_off();
    for (int i = 0; i < VOICES; i++)
        if (voice[i].key == key) { voice[i].key = -1; jt_env_gate(&voice[i].env, false); }
    irq_on();
}

int main(void) {
    static const uint8_t CC[2] = {74, 71};          /* cutoff, resonance: the stock numbers */
    int knob_raw[2] = {0, 0}, knob_sent[2] = {-1, -1}, knob_owns[2] = {1, 1}, filt[2] = {1000, 0};
    uint16_t last_bits = 0, stable = 0, playing = 0;
    int quiet = 0;
    uint8_t ev[4];

    jt_init();
    for (int i = 0; i < VOICES; i++) voice[i].key = -1;
    jt_tape_init(&tape, tape_buf, TAPE);
    apply();
    jt_audio_init(render);
    jt_usb_init();
    intro();
    jt_menu_init(PAGES, 6);

    for (;;) {
        jt_delay_ms(10);
        jt_poll_escape();

        /* touch keyboard: two agreeing readings to change, 40 ms of nothing to let go */
        uint16_t bits = jt_keyboard();
        if (bits && bits == last_bits) { stable = bits; quiet = 0; }
        else if (!bits && ++quiet >= 4) stable = 0;
        last_bits = bits;
        for (int k = 0; k < JT_KEYBOARD_KEYS; k++) {
            uint16_t b = JT_KEYBOARD_BIT(k);
            int key = 47 + k;                       /* the lowest key is B2 */
            if ((stable & b) && !(playing & b)) { note_on(key, 100); jt_midi_send(0x90, (uint8_t)key, 100); }
            if (!(stable & b) && (playing & b)) { note_off(key); jt_midi_send(0x80, (uint8_t)key, 0); }
        }
        playing = stable;

        while (jt_midi_get(ev)) {
            int kind = ev[0] & 0x0F;
            if (kind == 9 && ev[3]) note_on(ev[2], ev[3]);
            else if (kind == 8 || kind == 9) note_off(ev[2]);
            else if (kind == 0xB)
                for (int i = 0; i < 2; i++)
                    if (ev[2] == CC[i]) { filt[i] = ev[3] * 1000 / 127; knob_owns[i] = 0; knob_raw[i] = jt_pot(i); }
        }

        /* the two knobs: a received CC holds until the knob is moved again */
        for (int i = 0; i < 2; i++) {
            int raw = jt_pot(i == 0 ? JT_ADC_CUTOFF : JT_ADC_RESONANCE);
            int d = raw - knob_raw[i];
            if (!knob_owns[i] && (d > 80 || d < -80)) knob_owns[i] = 1;
            if (!knob_owns[i]) continue;
            knob_raw[i] = raw;
            filt[i] = raw >= 4000 ? 1000 : raw / 4;
            int ds = raw - knob_sent[i];
            if (knob_sent[i] < 0) knob_sent[i] = raw;                   /* nothing sent at start-up */
            else if (ds > 40 || ds < -40) {
                knob_sent[i] = raw;
                jt_midi_send(0xB0, CC[i], (uint8_t)(raw >= 4000 ? 127 : raw * 127 / 4000));
            }
        }
        jt_filter((uint16_t)filt[0], (uint16_t)filt[1]);

        if (jt_menu_poll()) apply();
    }
}
