/* Simon: the robot plays a melody, you play it back. It is not a good loser.
 *
 * Every round the melody grows by a note. The notes come from one scale and wander by small
 * steps, so what you are memorising is a tune, and a band plays under it that gains a part
 * every two rounds and speeds up. Miss a note and the robot sings its taunt on the notes
 * you should have played. Three lives.
 *
 *   SELECT turn    (before a game) which game: CLASSIC, BY EAR, BACKWARDS
 *   SELECT press   start; during your turn, hear the melody again (once a round, and it will comment)
 *   keys           play
 *   LFO FILTER AMP ARP   the instrument: pluck, bell, chip, organ
 *   CUTOFF, RESONANCE    the analog filter, once you move them
 *   OSC            give up, back to the start
 *
 * The best score of each game is kept through power-off (jt_store_save).
 *
 * BY EAR lights only the first note. BACKWARDS wants the melody played last note first.
 */
#include "jt4000.h"
#include "jt_dsp.h"
#include "jt_lpc.h"
#include "words.h"

#define SR ((float)JT_SAMPLE_RATE)
#define MAXLEN 39                           /* the numbers the robot can say stop here */
#define TAPE 8192

enum { KICK = 1, HAT = 2, BASS = 4, CLAP = 8, HAT16 = 16, PAD = 32, SONG = 64 };
enum { CLASSIC, EAR, REVERSE, MODES };
static const char *const MODE_NAME[MODES] = {"CLASSIC", "BY EAR", "BACKWARDS"};
enum { SPLASH, MENU, TALK, SHOW, TURN, WON, OVER };
#define SPLASH_MS 2700
enum { NEUTRAL, GLEE, SULK };
static const char *const TIMBRE_NAME[4] = {"PLUCK", "BELL", "CHIP", "ORGAN"};

/* The keys in play, low to high (key 0 is the low B). The set grows with the melody:
 * five notes of a pentatonic scale, then seven, then every white key. */
static const uint8_t POOL[3][10] = {{1, 3, 5, 8, 10}, {1, 3, 5, 8, 10, 13, 15}, {0, 1, 3, 5, 6, 8, 10, 12, 13, 15}};
static const uint8_t POOL_SIZE[3] = {5, 7, 10};
static int pool_of(int length) { return length < 6 ? 0 : length < 12 ? 1 : 2; }

/* the band: A minor, F, C, G, a bar each */
static const int8_t ROOT[4] = {0, -4, 3, -2};
static const int8_t THIRD[4] = {3, 4, 4, 4};
static const int8_t BASSLINE[16] = {0, -1, -1, 0, -1, -1, 12, -1, 0, -1, 12, 0, -1, 0, -1, 12};

/* ---- shared between the sound, the screen and the game ---- */
static volatile uint32_t samples;
static volatile int32_t song_step = -1;
static volatile int step_len = 4800;        /* samples per 16th */
static volatile int layers;                 /* which parts of the band play */
static volatile int show;                   /* 0 no, 1 waiting for the beat, 2 playing the melody */
static volatile int show_i;
static volatile int lit_key = -1;           /* the key the robot is playing */
static volatile uint32_t lit_at, lit_for;
static volatile bool lit_hidden;            /* ... but not shown (BY EAR) */
static volatile int flourish;
static volatile float kick_level;
static uint8_t seq[MAXLEN + 1], dur[MAXLEN + 1];    /* the melody: keys, and 16ths each lasts */
static volatile int len;
static int mode, timbre;

/* ---- voices ---- */
static struct { float ph, bend, amp; } kick;
static struct { float amp, last; int hits, wait; } clap;
static struct { float amp, last, last2, decay; } hat;
static struct { jt_osc_t osc; jt_svf_t f; float env, amp; int gate; } bass = {.osc = {.wave = JT_WAVE_SAW}};
static struct { jt_osc_t osc[6]; float level, lp; } pad;
static struct { jt_osc_t a, b; float env, lp, decay; int kind; } tone[4];
static struct { jt_osc_t a, b; float amp; } buzz = {.a = {.wave = JT_WAVE_SQUARE}, .b = {.wave = JT_WAVE_SAW}};
static jt_lpc_t vox;
static int16_t tape_buf[TAPE];
static jt_tape_t tape;

/* A note. From the audio interrupt, or with it held off. */
static void strike(int k) {
    static int next;
    static const uint8_t WA[4] = {JT_WAVE_SAW, JT_WAVE_SINE, JT_WAVE_SQUARE, JT_WAVE_TRIANGLE};
    static const uint8_t WB[4] = {JT_WAVE_SAW, JT_WAVE_SINE, JT_WAVE_SQUARE, JT_WAVE_SINE};
    static const float RATIO[4] = {1.006f, 2.76f, 2.0f, 2.0f};
    static const float DECAY[4] = {0.99990f, 0.99993f, 0.99988f, 0.99995f};
    int v = next = (next + 1) & 3;
    float hz = jt_note_hz((float)(59 + k));
    tone[v].a.wave = WA[timbre];
    tone[v].b.wave = WB[timbre];
    jt_osc_hz(&tone[v].a, hz, SR);
    jt_osc_hz(&tone[v].b, hz * RATIO[timbre], SR);
    tone[v].kind = timbre;
    tone[v].decay = DECAY[timbre];
    tone[v].env = 1.0f;
}

static void step(void) {
    static int show_wait, loop_i;
    int n = ++song_step, s = n & 15, chord = (n >> 4) & 3, p = layers;
    float root = 45.0f + (float)ROOT[chord];

    if ((p & KICK) && !(s & 3)) { kick.amp = 1.0f; kick.bend = 1.0f; kick.ph = 0.0f; }
    if ((p & CLAP) && (s == 4 || s == 12)) { clap.hits = 3; clap.wait = 0; }
    if ((p & HAT) && (s & 3) == 2) { hat.amp = 0.8f; hat.decay = 0.9992f; }
    else if (p & HAT16) { hat.amp = s & 1 ? 0.3f : 0.45f; hat.decay = 0.996f; }
    if ((p & BASS) && BASSLINE[s] >= 0) {
        jt_osc_hz(&bass.osc, jt_note_hz(root - 12.0f + (float)BASSLINE[s]), SR);
        bass.env = 1.0f;
        bass.gate = step_len * 6 / 10;
    }
    if (s == 0)
        for (int i = 0; i < 3; i++) {
            float note = root + 12.0f + (i == 0 ? 0.0f : i == 1 ? (float)THIRD[chord] : 7.0f);
            jt_osc_hz(&pad.osc[2 * i], jt_note_hz(note), SR);
            jt_osc_hz(&pad.osc[2 * i + 1], jt_note_hz(note + 0.12f), SR);
        }

    if (flourish > 0) {                     /* got it right: a quick run up the chord of C */
        static const uint8_t UP[4] = {13, 8, 5, 1};
        strike(UP[--flourish]);
    }
    if (show == 1 && !(s & 3)) { show = 2; show_i = 0; show_wait = 0; }     /* the melody starts on a beat */
    if (show == 2 && --show_wait <= 0) {
        if (show_i >= len) {
            show = 0;                       /* the last note has had its time: your turn */
        } else {
            strike(seq[show_i]);
            lit_key = seq[show_i];
            lit_hidden = mode == EAR && show_i > 0;
            lit_at = samples;
            lit_for = (uint32_t)(dur[show_i] * step_len * 7 / 10);
            show_wait = dur[show_i];
            show_i++;
        }
    }
    if ((p & SONG) && len > 0 && !(s & 1)) {    /* afterwards: what you built, round and round */
        int k = seq[loop_i++ % len];
        strike(k);
        lit_key = k;
        lit_hidden = false;
        lit_at = samples;
        lit_for = (uint32_t)step_len;
    }
}

static void render(int16_t *out, int n) {
    static float wet_now, wet_prev, half_send, duck;
    static int alt, left;
    int p = layers;

    jt_svf_set(&bass.f, 110.0f * jt_exp2(bass.env * 4.0f), 0.7f, SR);
    float pad_goal = (p & PAD) ? 1.0f : 0.0f;
    jt_lpc_block(&vox);
    duck += ((jt_lpc_busy(&vox) ? 1.0f : 0.0f) - duck) * 0.12f;
    float band = 0.75f - 0.45f * duck;      /* the band steps back while the robot talks */

    for (int i = 0; i < n; i++) {
        if (--left <= 0) { left = step_len; step(); }
        samples++;
        float white = jt_rand();

        kick.bend *= 0.9985f;
        kick.ph += 48.0f * (1.0f + 4.0f * kick.bend) / SR;
        if (kick.ph >= 1.0f) kick.ph -= 1.0f;
        float k = jt_softclip(jt_sin(kick.ph) * kick.amp * 1.8f);
        kick.amp *= 0.9996f;

        if (clap.hits && --clap.wait <= 0) { clap.hits--; clap.wait = 320; clap.amp = 0.9f; }
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

        float pd = 0.0f;
        pad.level += (pad_goal - pad.level) * 0.00006f;
        if (pad.level > 0.001f) {
            for (int o = 0; o < 6; o++) pd += jt_osc_next(&pad.osc[o]);
            pad.lp += (pd * 0.16f - pad.lp) * 0.05f;
            pd = pad.lp * pad.level;
        }

        float notes = 0.0f;
        for (int v = 0; v < 4; v++) {
            if (tone[v].env < 0.001f) continue;
            float a = jt_osc_next(&tone[v].a), o = jt_osc_next(&tone[v].b), y;
            switch (tone[v].kind) {
            case 0:                         /* pluck: bright at the start, closing as it fades */
                tone[v].lp += (0.5f * (a + o) - tone[v].lp) * (0.04f + 0.5f * tone[v].env);
                y = tone[v].lp * 1.5f;
                break;
            case 1: y = a + 0.4f * o * tone[v].env; break;      /* bell: the odd overtone dies first */
            case 2: y = 0.4f * a + 0.2f * o; break;
            default: y = 0.6f * a + 0.3f * o; break;
            }
            notes += y * tone[v].env;
            tone[v].env *= tone[v].decay;
        }

        float z = 0.0f;                     /* the wrong-note buzzer */
        if (buzz.amp > 0.001f) {
            z = (jt_osc_next(&buzz.a) + jt_osc_next(&buzz.b)) * buzz.amp;
            buzz.amp *= 0.99985f;
        }

        float v = jt_lpc_next(&vox) * 2.4f;
        float music = 0.7f * k + 0.4f * c + 0.15f * h + 0.45f * b + 0.4f * pd;
        float dry = music * band + 0.45f * notes + 0.25f * z + v;
        float send = 0.3f * notes + 0.2f * v + 0.08f * c * band, y;
        if (alt ^= 1) {
            half_send = send;
            y = dry + 0.5f * (wet_prev + wet_now);
        } else {
            wet_prev = wet_now;
            float d = jt_tape_read(&tape, (float)(step_len * 3 / 2 > TAPE - 2 ? TAPE - 2 : step_len * 3 / 2));
            jt_tape_write(&tape, 0.5f * (half_send + send) + d * 0.4f);
            wet_now = d * 0.7f;
            y = dry + wet_prev;
        }
        y = jt_softclip(y * 0.58f);
        out[i] = (int16_t)(y * 30000.0f);
    }
    kick_level = kick.amp;
}

/* ---- the robot's mouth: a queue of words, each at its own pitch ---- */
static struct { int8_t word; uint8_t note; } queue[20];
static int q_len, q_gap, spoken = -1;

static int find(const char *name, int n) {
    for (int w = 0; w < WORD_COUNT; w++) {
        int i = 0;
        while (i < n && WORD_NAMES[w][i] == name[i]) i++;
        if (i == n && !WORD_NAMES[w][i]) return w;
    }
    return -1;
}

/* Say a phrase. A first character sets the tune: '\' falls word by word, '?' lifts the
 * last word, '~' sings the words on the notes of the melody. */
static void say(const char *text) {
    char tune = *text == '\\' || *text == '?' || *text == '~' ? *text++ : 0;
    int first = q_len, i = 0;
    while (*text && q_len < 20) {
        int n = 0;
        while (text[n] && text[n] != ' ') n++;
        int w = find(text, n);
        if (w >= 0) {
            queue[q_len].word = (int8_t)w;
            queue[q_len].note = (uint8_t)(tune == '\\' ? 52 - 3 * i : tune == '~' && len ? 47 + seq[i % len] : 45);
            q_len++;
            i++;
        }
        text += n;
        while (*text == ' ') text++;
    }
    if (tune == '?' && q_len > first) queue[q_len - 1].note = 52;
}

static void say_number(int n) {
    static const char *const NAME[20] = {"ZERO", "ONE", "TWO", "THREE", "FOUR", "FIVE", "SIX", "SEVEN", "EIGHT", "NINE", "TEN",
                                         "ELEVEN", "TWELVE", "THIRTEEN", "FOURTEEN", "FIFTEEN", "SIXTEEN", "SEVENTEEN", "EIGHTEEN", "NINETEEN"};
    if (n >= 20) {
        say(n >= 30 ? "THIRTY" : "TWENTY");
        n %= 10;
        if (!n) return;
    }
    say(NAME[n]);
}

static void mouth(void) {                   /* about every 8 ms: the next word, after a short breath */
    if (!q_len || jt_lpc_busy(&vox)) { q_gap = 0; return; }
    if (++q_gap < 4) return;
    spoken = queue[0].word;
    vox.hz = jt_note_hz((float)queue[0].note);
    jt_lpc_say(&vox, &WORD_DATA[WORD_AT[spoken]]);
    q_len--;
    for (int i = 0; i < q_len; i++) queue[i] = queue[i + 1];
}
static bool talking(void) { return q_len || jt_lpc_busy(&vox); }
static void hush(void) { q_len = 0; jt_lpc_stop(&vox); }

/* ---- the game ---- */
static int state = MENU, after, lives, pos, score, mood, teach_key = -1;
static uint8_t best[MODES];                 /* kept in the EEPROM, so the robot remembers */
#define BEST_TAG JT_TAG('S', 'I', 'M', 'N')
static bool warned, replayed, record;
static uint32_t hold_until, deadline, turn_began, flash_until, name_until, over_at;
static uint32_t seed = 1;

static uint32_t rnd(void) {
    seed = seed * 1664525u + 1013904223u;
    return seed >> 12;
}
static const char *pick(const char *const *list, int n) { return list[rnd() % (uint32_t)n]; }

static void band_for(int length) {          /* a part more every two rounds, and a little faster */
    int bpm = 96 + 3 * length;
    if (bpm > 144) bpm = 144;
    step_len = JT_SAMPLE_RATE * 15 / bpm;
    layers = KICK | (length >= 3 ? HAT : 0) | (length >= 5 ? BASS : 0) | (length >= 7 ? CLAP : 0) | (length >= 9 ? HAT16 : 0) |
             (length >= 11 ? PAD : 0);
}

/* The next note: a step or two along the scale from the last one, now and then a leap. */
static void add_note(void) {
    static const int8_t MOVE[8] = {-1, 1, -1, 1, -2, 2, 3, -3};
    const uint8_t *p = POOL[pool_of(len + 1)];
    int n = POOL_SIZE[pool_of(len + 1)], at = (int)(rnd() % (uint32_t)n);
    if (len) {
        for (int i = 0; i < n; i++)
            if (p[i] == seq[len - 1]) at = i;
        at += MOVE[rnd() & 7];
        if (at < 0) at = -at;
        if (at >= n) at = 2 * (n - 1) - at;
        if (len >= 2 && p[at] == seq[len - 1] && p[at] == seq[len - 2]) at = (at + 1) % n;
    }
    seq[len] = p[at];
    dur[len] = rnd() % 4 == 0 ? 4 : 2;
    len++;
    band_for(len);
}

static void talk_then(int next, uint32_t ms, uint32_t pause) {
    state = TALK;
    after = next;
    hold_until = ms + pause;
}

static void to_menu(void) {
    hush();
    state = MENU;
    show = 0;
    teach_key = -1;
    mood = NEUTRAL;
    step_len = 4800;
    layers = PAD | HAT;
}

static void start(uint32_t ms) {
    static const char *const HELLO[MODES] = {"REPEAT AFTER ME", "LISTEN HUMAN", "REPEAT AFTER ME BACKWARDS"};
    seed ^= samples * 2654435761u + jt_pot(JT_ADC_CUTOFF) + ((uint32_t)jt_pot(JT_ADC_RESONANCE) << 12);
    len = 0;
    lives = 3;
    score = 0;
    record = replayed = false;
    add_note();
    hush();
    say(HELLO[mode]);
    mood = NEUTRAL;
    talk_then(SHOW, ms, 300);
}

static void game_over(uint32_t ms, bool won) {
    static const char *const VERDICT[5] = {"\\PATHETIC", "~MY TOASTER PLAYS BETTER", "NOT BAD FOR A HUMAN", "IMPRESSIVE", "I HATE YOU"};
    state = OVER;
    over_at = ms;
    say(won ? "YOU WIN" : "\\GAME OVER");
    say("YOU SCORED");
    say_number(score);
    if (score > best[mode]) {
        if (best[mode]) say("?NEW RECORD");
        best[mode] = (uint8_t)score;
        record = true;
        jt_store_save(0, BEST_TAG, best, sizeof best);
    }
    say(VERDICT[score < 3 ? 0 : score < 6 ? 1 : score < 10 ? 2 : score < 15 ? 3 : 4]);
    step_len = JT_SAMPLE_RATE * 15 / 132;
    layers = KICK | HAT | BASS | CLAP | PAD;
}

static void round_won(uint32_t ms) {
    static const char *const GRUDGE[7] = {"LUCKY", "NOT BAD", "OK", "I AM NOT WORRIED", "?IS THAT ALL", "\\BORING", "NOT BAD FOR A HUMAN"};
    score = len;
    mood = SULK;
    replayed = false;
    flourish = 4;
    if (len >= MAXLEN) { game_over(ms, true); return; }
    if (len == 10) say("IMPRESSIVE");
    else if (len == 15) say("I HATE YOU");
    else if (len == 1 || rnd() % 3 == 0) say(pick(GRUDGE, 7));
    add_note();
    talk_then(SHOW, ms, 1100);
}

static void fail(uint32_t ms, bool slow) {
    static const char *const WRONG[8] = {"~WRONG", "~NO NO NO", "\\HA HA HA", "~MY TOASTER PLAYS BETTER", "?ARE YOU EVEN TRYING",
                                         "\\THAT WAS BAD", "~TRY AGAIN HUMAN", "\\PATHETIC"};
    jt_irq_off();
    buzz.amp = 1.0f;
    for (int v = 0; v < 4; v++) tone[v].env = 0.0f;
    jt_irq_on();
    mood = GLEE;
    teach_key = seq[mode == REVERSE ? len - 1 - pos : pos];
    flash_until = ms + 350;
    replayed = false;
    if (--lives <= 0) { game_over(ms, false); return; }
    say(slow ? "\\TOO SLOW" : pick(WRONG, 8));
    if (lives == 1) say("LAST CHANCE");
    talk_then(SHOW, ms, 700);
}

static void press(int k, uint32_t ms) {     /* a key, during your turn */
    jt_irq_off();
    strike(k);
    jt_irq_on();
    if (k != seq[mode == REVERSE ? len - 1 - pos : pos]) { fail(ms, false); return; }
    pos++;
    deadline = ms + 3500;
    warned = false;
    turn_began = ms;
    if (pos == len) { state = WON; hold_until = ms + 500; }      /* let the last note ring before anything answers */
}

/* ---- the screen ---- */
static void block(int x, int y, int w, int h) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) jt_oled_pixel(i, j, true);
}
static void frame(int x, int y, int w, int h) {
    for (int i = x; i < x + w; i++) { jt_oled_pixel(i, y, true); jt_oled_pixel(i, y + h - 1, true); }
    for (int j = y; j < y + h; j++) { jt_oled_pixel(x, j, true); jt_oled_pixel(x + w - 1, j, true); }
}

/* The robot. It is pleased when you fail and sulks when you do not. */
static void face(uint32_t ms) {
    block(7, 0, 2, 2);                      /* antenna */
    frame(1, 2, 14, 14);
    if (mood == GLEE) {
        for (int e = 0; e < 2; e++) {
            int x = 4 + 6 * e;
            jt_oled_pixel(x - 1, 7, true); jt_oled_pixel(x, 6, true); jt_oled_pixel(x + 1, 6, true); jt_oled_pixel(x + 2, 7, true);
        }
    } else if (mood == SULK) {
        block(3, 7, 4, 1); block(9, 7, 4, 1);
        jt_oled_pixel(3, 5, true); jt_oled_pixel(4, 6, true); jt_oled_pixel(12, 5, true); jt_oled_pixel(11, 6, true);
    } else {
        bool blink = ms % 3100 < 110;
        block(4, blink ? 7 : 5, 2, blink ? 1 : 3);
        block(10, blink ? 7 : 5, 2, blink ? 1 : 3);
    }
    if (jt_lpc_busy(&vox)) {
        int open = 1 + (int)(ms / 70 % 3);
        frame(5, 10, 6, open + 2);
    } else {
        block(5, 12, 6, 1);
        if (mood == GLEE) { jt_oled_pixel(4, 11, true); jt_oled_pixel(11, 11, true); }
        if (mood == SULK) { jt_oled_pixel(4, 13, true); jt_oled_pixel(11, 13, true); }
    }
}

static void number(char *s, int n) {        /* up to two digits, then the end of the string */
    if (n >= 10) *s++ = (char)('0' + n / 10);
    *s++ = (char)('0' + n % 10);
    *s = 0;
}
static char *copy(char *to, const char *from) {
    while (*from) *to++ = *from++;
    *to = 0;
    return to;
}

static void wipe(int x, int y, int w, int h) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) jt_oled_pixel(i, j, false);
}

/* The keyboard as it is printed on the synth: ten white keys side by side, six black keys
 * on top of the joins. A white key in play carries a mark; a sounding key is filled. */
static void keyboard(uint16_t held, uint32_t ms) {
    static const uint8_t WHITE[10] = {0, 1, 3, 5, 6, 8, 10, 12, 13, 15};
    static const uint8_t BLACK[6][2] = {{2, 2}, {4, 3}, {7, 5}, {9, 6}, {11, 7}, {14, 9}};   /* key, and the white key to its right */
    int which = pool_of(len ? len : 1);
    bool robot = lit_key >= 0 && samples - lit_at < lit_for && !lit_hidden;
#define SOUNDING(k) ((held & JT_KEYBOARD_BIT(k)) || (robot && lit_key == (k)) || (teach_key == (k) && (ms / 120 & 1)))
    for (int i = 0; i < 10; i++) {
        int k = WHITE[i], x = 4 + 12 * i;
        bool in_play = false;
        for (int j = 0; j < POOL_SIZE[which]; j++) in_play |= POOL[which][j] == k;
        frame(x, 18, 13, 14);
        if (SOUNDING(k)) block(x + 1, 19, 11, 12);
        else if (in_play) block(x + 4, 26, 5, 4);
    }
    for (int i = 0; i < 6; i++) {
        int x = 4 + 12 * BLACK[i][1] - 3;
        wipe(x - 1, 18, 9, 8);
        if (SOUNDING(BLACK[i][0])) frame(x, 18, 7, 7);
        else block(x, 18, 7, 7);
    }
#undef SOUNDING
}

static void compose(uint32_t ms, uint16_t held) {
    static const uint8_t HEART[6] = {0x36, 0x7F, 0x7F, 0x3E, 0x1C, 0x08};
    char top[17], *t = top;
    jt_oled_clear();
    face(ms);
    top[0] = 0;
    if (jt_lpc_busy(&vox) && spoken >= 0) copy(top, WORD_NAMES[spoken]);
    else if (ms < name_until) copy(top, TIMBRE_NAME[timbre]);
    else if (state == MENU) copy(top, ms / 2000 & 1 ? "PRESS SELECT" : "SIMON SAYS");
    else if (state == TURN) copy(top, mode == REVERSE ? "BACKWARDS!" : "YOUR TURN");
    else if (state == WON) copy(top, "CORRECT");
    else if (state == OVER) { t = copy(top, "SCORE "); number(t, score); }
    else { t = copy(top, "ROUND "); number(t, len); }
    jt_oled_text(20, 0, top);

    if (state == MENU) {
        t = copy(top, "<");
        t = copy(t, MODE_NAME[mode]);
        copy(t, ">");
        jt_oled_text(20, 9, top);
        if (best[mode]) { number(top, best[mode]); jt_oled_text(112, 9, top); }
    } else if (state == OVER) {
        t = copy(top, record ? "NEW BEST " : "BEST ");
        number(t, best[mode]);
        jt_oled_text(20, 9, top);
    } else {
        for (int i = 0; i < 3; i++)         /* lives */
            for (int r = 0; r < 6; r++)
                for (int c = 0; c < 7; c++)
                    if (i < lives ? HEART[r] >> (6 - c) & 1 : r == 2 && c == 3) jt_oled_pixel(20 + 9 * i + c, 9 + r, true);
        int done = state == TURN || state == WON ? pos : show == 2 ? show_i : state == SHOW ? 0 : 0;
        for (int i = 0; i < len; i++) {     /* the melody as ticks: tall once played */
            bool tall = (state == TURN || state == WON) && mode == REVERSE ? i >= len - done : i < done;
            block(49 + 2 * i, tall ? 9 : 13, 1, tall ? 5 : 1);
        }
        if (lit_hidden && samples - lit_at < lit_for) jt_oled_text(120, 0, "?");
        if (state == TURN && deadline > turn_began) {       /* the time you have left, running out */
            int w = (int)((uint64_t)(deadline > ms ? deadline - ms : 0) * 108 / (deadline - turn_began));
            block(20, 16, w, 1);
        }
    }
    keyboard(state == SHOW ? 0 : held, ms);
}

/* Switching on: the robot's face, then into its mouth, which dissolves into the game. */
static void splash(uint32_t ms, uint16_t held) {
    static const uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    static uint16_t src[16];
    static uint8_t snap[JT_OLED_H][JT_OLED_W / 8];
    jt_oled_clear();
    face(ms);                               /* drawn small, read back, then drawn large */
    for (int y = 0; y < 16; y++) {
        src[y] = 0;
        for (int x = 0; x < 16; x++) src[y] |= (uint16_t)(jt_oled_get(x, y) << x);
    }
    jt_oled_clear();
    int t = ms < 600 ? 0 : ms > 2100 ? 1500 : (int)ms - 600;
    int zoom = 32 + t * t / 9000;           /* in 16ths: twice the size, speeding up to seventeen times */
    int focus = 8 * 16 + 60 * t / 1500;     /* from the middle of the face down to the mouth */
    for (int y = 0; y < JT_OLED_H; y++)
        for (int x = 0; x < JT_OLED_W; x++) {
            int u = ((x - 64) * 256 + 4096 * zoom) / zoom - 4096 + 8 * 16, v = ((y - 16) * 256 + 4096 * zoom) / zoom - 4096 + focus;
            if (u >= 0 && u < 256 && v >= 0 && v < 256 && (src[v >> 4] >> (u >> 4) & 1)) jt_oled_pixel(x, y, true);
        }
    if (!t) { jt_oled_text(6, 12, "SIMON"); jt_oled_text(90, 12, "SAYS"); }
    if (ms > 2100) {                        /* the game shows through, a few more pixels each frame */
        int level = (int)(ms - 2100) * 17 / (SPLASH_MS - 2100);
        for (int y = 0; y < JT_OLED_H; y++)
            for (int x = 0; x < JT_OLED_W; x++) {
                if (!(x & 7)) snap[y][x >> 3] = 0;
                snap[y][x >> 3] |= (uint8_t)(jt_oled_get(x, y) << (x & 7));
            }
        compose(ms, held);
        for (int y = 0; y < JT_OLED_H; y++)
            for (int x = 0; x < JT_OLED_W; x++)
                if (BAYER[y & 3][x & 3] >= level) jt_oled_pixel(x, y, snap[y][x >> 3] >> (x & 7) & 1);
    }
}

/* bit 0 OSC, 1 LFO, 2 FILTER, 3 AMP, 4 ARP, 6 SELECT; a button counts after two readings */
static uint8_t buttons(void) {
    static uint8_t now, last;
    uint8_t k = jt_keys_raw(0), raw = 0;
    if (jt_button_osc()) raw |= 1;
    if (k & JT_KEY_LFO) raw |= 2;
    if (k & JT_KEY_FILTER) raw |= 4;
    if (k & JT_KEY_AMP) raw |= 8;
    if (k & JT_KEY_ARP) raw |= 16;
    if (k & JT_KEY_SELECT) raw |= 64;
    now = (uint8_t)((now & (raw | last)) | (raw & last));
    last = raw;
    return now;
}

int main(void) {
    static const char *const NAG[4] = {"HURRY UP", "I AM WAITING", "?HELLO", "WAKE UP HUMAN"};
    uint32_t polled = 0;
    uint16_t last_bits = 0, stable = 0, before = 0;
    int quiet = 0, anchor[2], owns[2] = {0, 0};
    uint8_t held = 0xFF, ev[4];
    int32_t enc_last;

    jt_init();
    for (int o = 0; o < 6; o++) pad.osc[o].wave = JT_WAVE_SAW;
    jt_tape_init(&tape, tape_buf, TAPE);
    jt_lpc_init(&vox, LPC_SIN, LPC_GAIN);
    vox.hz = 110.0f;
    vox.ratio = vox.speed = 1.0f;
    jt_filter(1000, 0);
    jt_store_load(0, BEST_TAG, best, sizeof best);
    to_menu();
    state = SPLASH;
    say("HELLO HUMAN");
    jt_audio_init(render);
    jt_usb_init();
    enc_last = jt_encoder();
    anchor[0] = jt_pot(JT_ADC_CUTOFF);
    anchor[1] = jt_pot(JT_ADC_RESONANCE);

    for (;;) {
        jt_poll_escape();
        uint32_t ms = samples / 32;

        if (samples - polled >= 256) {      /* hands: about every 8 ms */
            polled = samples;
            uint8_t now = buttons(), pressed = (uint8_t)(now & ~held);
            held = now;
            uint16_t bits = jt_keyboard();
            if (bits && bits == last_bits) { stable = bits; quiet = 0; }
            else if (!bits && ++quiet >= 3) stable = 0;
            last_bits = bits;
            int fresh = jt_keyboard_lowest((uint16_t)(stable & ~before));
            before = stable;
            int steps = (jt_encoder() - enc_last) / 8;
            enc_last += steps * 8;
            while (jt_midi_get(ev)) { }
            for (int i = 0; i < 2; i++) {
                int d = jt_pot(i ? JT_ADC_RESONANCE : JT_ADC_CUTOFF) - anchor[i];
                if (d > 90 || d < -90) owns[i] = 1;
            }
            jt_filter((uint16_t)(owns[0] ? jt_pot(JT_ADC_CUTOFF) / 4 : 1000), (uint16_t)(owns[1] ? jt_pot(JT_ADC_RESONANCE) / 4 : 0));
            for (int b = 0; b < 4; b++)
                if (pressed & (2 << b)) {
                    timbre = b;
                    name_until = ms + 700;
                    if (state == MENU || state == TURN) { jt_irq_off(); strike(8); jt_irq_on(); }
                }
            mouth();

            if ((pressed & 1) && state > MENU) to_menu();
            else switch (state) {
            case SPLASH:
                if (ms >= SPLASH_MS || (pressed & 64) || fresh >= 0) state = MENU;      /* any touch skips it */
                break;
            case MENU:
                if (steps) mode = ((mode + steps) % MODES + MODES) % MODES;
                if ((pressed & 64) || fresh >= 0) start(ms);
                break;
            case TALK:
                if (!talking() && ms >= hold_until) {
                    state = after;
                    teach_key = -1;
                    mood = NEUTRAL;
                    show = 1;
                }
                break;
            case SHOW:
                if (!show) {
                    state = TURN;
                    pos = 0;
                    turn_began = ms;
                    deadline = ms + 5000 + (mode == REVERSE ? 150u * (uint32_t)len : 0);
                    warned = false;
                }
                break;
            case TURN:
                if (fresh >= 0) press(fresh, ms);
                else if ((pressed & 64) && !replayed) {     /* once a round: hear it again, and hear about it */
                    replayed = true;
                    say("?AGAIN REALLY");
                    talk_then(SHOW, ms, 200);
                } else if (ms > deadline) fail(ms, true);
                else if (!warned && !talking() && ms - turn_began > (deadline - turn_began) * 6 / 10) {
                    warned = true;
                    say(pick(NAG, 4));
                }
                break;
            case WON:
                if (ms >= hold_until) round_won(ms);
                break;
            case OVER:
                if (!talking() && !(layers & SONG) && ms - over_at > 800) layers |= SONG;   /* your melody, with the whole band */
                if (((pressed & 64) || fresh >= 0) && ms - over_at > 1500) to_menu();
                break;
            }
        }

        bool flash = ms < flash_until;
        jt_oled_invert(flash);
        jt_leds((uint8_t)(flash ? 0x3F : state == TURN ? (1u << lives) - 1 : kick_level > 0.5f ? 0x3F : 0));
        if (state == SPLASH) splash(ms, stable);
        else compose(ms, stable);
        jt_oled_flush();
    }
}
