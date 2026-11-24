/* JT-4000 Micro SDK: board support for custom firmware.
 * Hardware facts come from the reverse-engineered stock firmware 1.1.5 (JT-4000 Micro) and
 * 1.1.6 (JT-4000M Micro); see ../docs. One image runs on both synths. */
#ifndef JT4000_H
#define JT4000_H

#include <stdbool.h>
#include <stdint.h>

/* ---- board ---- */
void jt_init(void);               /* 170 MHz clock, GPIO, display, panel, pots, keyboard. Call first. */
extern uint32_t jt_clock_hz;      /* current core clock */
void jt_delay_ms(uint32_t ms);
void jt_reset(void);              /* system reset */

/* Which synth this is. The JT-4000M has a MIDI socket; to make room for it, its display's
 * data/command line is on another pin and the keyboard's key-down line is not connected.
 * jt_gpio_init (part of jt_init) finds out by reading two pins as inputs, before it drives
 * either; jt_board() returns JT_BOARD_MICRO until then. Build with
 * DEFS=-DJT_BOARD_FORCE=JT_BOARD_M (or JT_BOARD_MICRO) to skip the test. */
#define JT_BOARD_MICRO 0          /* JT-4000 Micro */
#define JT_BOARD_M     1          /* JT-4000M Micro */
int jt_board(void);

/* The steps of jt_init, exposed for bring-up tests. */
void jt_clock_170mhz(void);       /* HSE crystal + PLL; a no-op when the bootloader left it running */
void jt_clock_hsi16(void);        /* 16 MHz internal oscillator */
void jt_gpio_init(void);
void jt_oled_init(void);

/* Set the bootloader's update flag in the EEPROM and reset. The synth comes back as
 * the bootloader's USB serial port, ready for tools/flash.py. Needs nothing else to be
 * initialised: it bit-bangs the I2C bus with its own delays. */
void jt_enter_update_mode(void);

/* ---- buttons wired straight to the MCU (the other four go through the I2C key chip) ---- */
bool jt_button_osc(void);         /* PB4, true while pressed */
bool jt_button_settings(void);    /* PA10 */

/* Optional. Call regularly to make OSC + SETTINGS, held together while the firmware runs,
 * restart into update mode. Nothing depends on it: `make flash` reaches update mode over
 * USB by itself (see jt_usb_init), and holding the two buttons while switching on always
 * works, because the bootloader reads them. Leave it out to use the combination yourself. */
void jt_poll_escape(void);

/* ---- display: SSD1306 OLED, 128 x 32, 1 bit per pixel ---- */
#define JT_OLED_W 128
#define JT_OLED_H 32
void jt_oled_clear(void);
void jt_oled_pixel(int x, int y, bool on);
void jt_oled_text(int x, int y, const char *s);   /* 8x8 font: 16 columns x 4 rows */
void jt_oled_flush(void);                         /* send the frame buffer to the panel */
bool jt_oled_get(int x, int y);                   /* read a pixel back from the frame buffer */
void jt_oled_line(int x0, int y0, int x1, int y1, bool on);
void jt_oled_text_big(int x, int y, const char *s, int scale);   /* scale 2 = 16-pixel letters; only sets pixels */
void jt_oled_invert(bool on);                     /* whole panel, at once (no flush needed) */

/* ---- I2C bus shared by the EEPROM (0x50) and the LED/key chip (0x70) ---- */
void jt_i2c_init(void);
bool jt_i2c_write(uint8_t addr7, const uint8_t *data, int n);   /* false if not acknowledged */
bool jt_i2c_read(uint8_t addr7, uint8_t *data, int n);

/* ---- storage that survives power-off: 24 KiB of the EEPROM ----
 * The part of the chip the stock firmware never uses, so its presets are left alone and
 * what you save survives a return to stock. Call from the main loop only (it shares the
 * bus with the buttons and lights); sound keeps playing meanwhile. Measured: reading takes
 * 0.4 ms a byte, writing about 50 ms for every 64 bytes that changed. A byte lasts about a
 * million writes: save when something changes, not on a timer. All four return false when
 * the chip did not answer or the range does not fit. */
#define JT_STORE_SIZE 0x6000
bool jt_store_read(uint16_t at, void *data, int n);
bool jt_store_write(uint16_t at, const void *data, int n);
/* A record: n bytes with a name (tag) and a checksum in front, n + 8 bytes at `at`.
 * jt_store_load leaves `data` untouched and returns false unless a record with this tag
 * and size was saved there and is intact, so defaults set beforehand stay. */
#define JT_TAG(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
bool jt_store_save(uint16_t at, uint32_t tag, const void *data, int n);
bool jt_store_load(uint16_t at, uint32_t tag, void *data, int n);

/* ---- button lights and scanned buttons ---- */
#define JT_LED_OSC      0x01
#define JT_LED_LFO      0x02
#define JT_LED_FILTER   0x04
#define JT_LED_AMP      0x08
#define JT_LED_ARP      0x10
#define JT_LED_SETTINGS 0x20
#define JT_KEY_LFO      0x01
#define JT_KEY_FILTER   0x02
#define JT_KEY_AMP      0x04
#define JT_KEY_ARP      0x08
#define JT_KEY_SELECT   0x20              /* pressing the SELECT knob (confirmed on hardware) */
void jt_panel_init(void);
void jt_leds(uint8_t mask);                       /* JT_LED_* bits */
void jt_leds_raw(uint8_t row4, uint8_t row6);     /* both LED rows the stock firmware uses */
uint8_t jt_keys_raw(uint8_t out[6]);              /* key-scan bytes; returns byte 0 (JT_KEY_* bits) */

/* ---- analog inputs (ADC1), 0..4095 ---- */
#define JT_POTS 3
#define JT_ADC_CUTOFF    0                 /* PA2, ADC channel 3: cutoff knob */
#define JT_ADC_RESONANCE 1                 /* PA3, ADC channel 4: resonance knob */
#define JT_ADC_KEYBOARD  2                 /* PB0, ADC channel 15: 4095 idle, about 52 while any key is held.
                                            * JT-4000M: not connected, always reads 4095; use jt_keyboard() */
void jt_adc_init(void);
uint16_t jt_pot(int n);

/* ---- touch keyboard: 16 keys, one bit each (0 if the chip does not answer) ---- */
void jt_keyboard_init(void);
uint16_t jt_keyboard(void);
#define JT_KEYBOARD_KEYS 16
#define JT_KEYBOARD_BIT(k) (0x8000u >> (k))   /* k = 0 for the lowest key (verified) */
/* Keys low to high: B C C# D D# E F F# G G# A A# B C C# D. Poll about every 10 ms and
 * debounce; reading the chip flat out makes it return gaps. */
int jt_keyboard_lowest(uint16_t bits);        /* index of the lowest held key, or -1 */

/* ---- audio: 12-bit DAC at 32 kHz, then the analog filter ----
 * Audio runs from an interrupt. Your render function fills blocks of JT_AUDIO_BLOCK
 * signed samples (2 ms each); keep it short and don't call display or I2C code from it. */
#define JT_SAMPLE_RATE 32000
#define JT_AUDIO_BLOCK 64
typedef void (*jt_audio_fn)(int16_t *out, int n);
void jt_audio_init(jt_audio_fn render);           /* not part of jt_init: call when you want sound */
/* Around a change the audio interrupt must see whole (starting a note: several fields). */
#ifdef JT_HOST
void jt_irq_off(void);
void jt_irq_on(void);
#else
static inline void jt_irq_off(void) { __asm__ volatile("cpsid i"); }
static inline void jt_irq_on(void) { __asm__ volatile("cpsie i"); }
#endif
void jt_filter(uint16_t cutoff, uint16_t resonance);   /* each 0..1000 */

/* ---- SELECT knob (rotary encoder) ---- */
void jt_encoder_init(void);
int32_t jt_encoder(void);                         /* running count, about 200 per turn */

/* ---- USB MIDI: class-compliant device, shows up as "JT-4000 SDK" ---- */
#define JT_SDK_VERSION_MAJOR 0
#define JT_SDK_VERSION_MINOR 1
#define JT_SDK_VERSION_PATCH 0
/* Call after jt_init; everything then runs from the USB interrupt. That includes the
 * stock "reboot to update mode" SysEx (F0 00 20 32 00 01 38 02 F7), which `make flash`
 * sends: the synth restarts into update mode whatever the main loop is doing. */
void jt_usb_init(void);
/* One received MIDI event, from USB or from the JT-4000M's MIDI socket: ev[0] & 0x0F is
 * the kind (8 note off, 9 note on, 0xB controller, 0xE pitch bend, 4..7 SysEx pieces,
 * 0xF a single byte such as clock), ev[1..3] the MIDI bytes. ev[0] & 0xF0 says where it
 * came from: 0 for USB, JT_MIDI_SOCKET for the socket. */
#define JT_MIDI_SOCKET 0x10
bool jt_midi_get(uint8_t ev[4]);
/* The JT-4000M's MIDI socket: input only, 31250 baud. jt_init starts it on a JT-4000M and
 * does nothing on a JT-4000 Micro; it needs no jt_usb_init. Received bytes wait in a
 * 256-byte ring (82 ms of a full stream) until jt_midi_get is called. */
void jt_midi_socket_init(void);
void jt_midi_send(uint8_t status, uint8_t d1, uint8_t d2);     /* channel messages, e.g. 0x90, note, velocity */
void jt_midi_send_sysex(const uint8_t *msg, int n);            /* complete message, F0 ... F7 */
#endif
