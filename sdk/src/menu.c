/* The panel menu; see jt_menu.h. */
#include "jt_menu.h"
#include "jt4000.h"

#define NAV_COUNTS  8                 /* encoder counts per item when moving through a page */
#define EDIT_COUNTS 4                 /* encoder counts per value step when editing */
#define FAST_COUNTS 12                /* counts within one poll that mean "spinning": steps x4 */
#define ROWS 3

static const jt_menu_page_t *pages;
static int npages, page, editing, dirty;
static int cursor[6], top[6];
static int32_t enc_last, enc_acc;
static uint8_t held;

/* bit n = the button of page n, bit 6 = SELECT pressed */
static uint8_t buttons(void) {
    uint8_t k = jt_keys_raw(0), b = 0;
    if (jt_button_osc()) b |= 1;
    if (k & JT_KEY_LFO) b |= 2;
    if (k & JT_KEY_FILTER) b |= 4;
    if (k & JT_KEY_AMP) b |= 8;
    if (k & JT_KEY_ARP) b |= 16;
    if (jt_button_settings()) b |= 32;
    if (k & JT_KEY_SELECT) b |= 64;
    return b;
}

static void value_text(const jt_menu_item_t *it, char *out) {    /* out: 6 characters + 0, right-aligned */
    char tmp[7];
    int n = 0;
    int v = *it->value;
    if (it->labels) {
        const char *s = it->labels[v - it->min];
        while (s[n] && n < 6) { tmp[n] = s[n]; n++; }
    } else {
        int neg = v < 0, digits = 0;
        char rev[6];
        if (neg) v = -v;
        do { rev[digits++] = (char)('0' + v % 10); v /= 10; } while (v && digits < 5);
        if (neg) tmp[n++] = '-';
        while (digits) tmp[n++] = rev[--digits];
    }
    int pad = 6 - n;
    for (int i = 0; i < 6; i++) out[i] = i < pad ? ' ' : tmp[i - pad];
    out[6] = 0;
}

static void draw(void) {
    const jt_menu_page_t *p = &pages[page];
    char line[17];
    jt_oled_clear();
    jt_oled_text(0, 0, p->title);
    line[0] = (char)('0' + (cursor[page] + 1) / 10);
    line[1] = (char)('0' + (cursor[page] + 1) % 10);
    line[2] = '/';
    line[3] = (char)('0' + p->count / 10);
    line[4] = (char)('0' + p->count % 10);
    line[5] = 0;
    jt_oled_text(88, 0, line[0] == '0' ? line + 1 : line);
    for (int r = 0; r < ROWS && top[page] + r < p->count; r++) {
        const jt_menu_item_t *it = &p->items[top[page] + r];
        int sel = top[page] + r == cursor[page], n = 1;
        for (int i = 0; i < 16; i++) line[i] = ' ';
        line[0] = sel ? (editing ? '*' : '>') : ' ';
        for (const char *s = it->name; *s && n < 10; s++) line[n++] = *s;
        value_text(it, line + 10);
        jt_oled_text(0, 8 * (r + 1), line);
    }
    jt_oled_flush();
    jt_leds((uint8_t)(1u << page));
}

void jt_menu_init(const jt_menu_page_t *p, int count) {
    pages = p;
    npages = count > 6 ? 6 : count;
    page = editing = 0;
    for (int i = 0; i < 6; i++) cursor[i] = top[i] = 0;
    enc_last = jt_encoder();
    enc_acc = 0;
    held = buttons();
    dirty = 1;
}

void jt_menu_redraw(void) { dirty = 1; }
int jt_menu_page(void) { return page; }

bool jt_menu_poll(void) {
    bool changed = false;
    uint8_t now = buttons(), pressed = (uint8_t)(now & ~held);
    held = now;

    if (!((now & 1) && (now & 32)))                 /* OSC + SETTINGS together is the way out, not a page */
        for (int i = 0; i < npages; i++)
            if ((pressed & (1u << i)) && i != page) { page = i; editing = 0; enc_acc = 0; dirty = 1; }
    if (pressed & 64) { editing = !editing; enc_acc = 0; dirty = 1; }

    int32_t enc = jt_encoder(), moved = enc - enc_last;
    enc_last = enc;
    enc_acc += moved;
    int per = editing ? EDIT_COUNTS : NAV_COUNTS;
    int steps = enc_acc / per;
    enc_acc -= steps * per;
    if (steps) {
        const jt_menu_page_t *p = &pages[page];
        if (editing) {
            const jt_menu_item_t *it = &p->items[cursor[page]];
            if (moved >= FAST_COUNTS || moved <= -FAST_COUNTS) steps *= 4;
            int v = *it->value + steps;
            if (v < it->min) v = it->min;
            if (v > it->max) v = it->max;
            if (v != *it->value) { *it->value = (int16_t)v; changed = true; dirty = 1; }
        } else {
            int c = cursor[page] + steps;
            if (c < 0) c = 0;
            if (c > p->count - 1) c = p->count - 1;
            if (c != cursor[page]) {
                cursor[page] = c;
                if (c < top[page]) top[page] = c;
                if (c >= top[page] + ROWS) top[page] = c - ROWS + 1;
                dirty = 1;
            }
        }
    }
    if (dirty) { dirty = 0; draw(); }
    return changed;
}
