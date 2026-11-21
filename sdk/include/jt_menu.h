/* JT-4000 SDK: a small menu for editing numbers with the panel.
 *
 *   - Up to six pages, one per panel button (OSC, LFO, FILTER, AMP, ARP, SETTINGS); the
 *     button's light shows the open page.
 *   - Turn SELECT to move through the page's items, press it to edit the chosen item
 *     (shown with *), turn to change the value, press again to go back.
 *   - The display shows the page title and three items at a time.
 *
 * You describe the pages as tables of items pointing at your own int16_t variables;
 * jt_menu_poll() does the rest and tells you when a value changed. */
#ifndef JT_MENU_H
#define JT_MENU_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *name;                 /* up to 9 characters */
    int16_t *value;
    int16_t min, max;
    const char *const *labels;        /* optional: text for each value, labels[value - min] */
} jt_menu_item_t;

typedef struct {
    const char *title;                /* up to 11 characters */
    const jt_menu_item_t *items;
    int count;
} jt_menu_page_t;

void jt_menu_init(const jt_menu_page_t *pages, int count);
/* Read the panel and redraw when needed. Call every 10 ms or so, after jt_init().
 * Returns true when the user changed a value. */
bool jt_menu_poll(void);
void jt_menu_redraw(void);            /* force a redraw, e.g. after a value was changed over MIDI */
int jt_menu_page(void);               /* index of the open page */

#endif
