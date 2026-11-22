/* SSD1306 128x32 over bit-banged SPI. The initialisation commands are the ones the
 * stock firmware sends (its routine at 0x080172C8). */
#include "jt4000.h"
#include "../third_party/font8x8_basic.h"

#ifdef JT_HOST
uint8_t jt_oled_fb[JT_OLED_W * JT_OLED_H / 8];      /* the host build shows this in a window (host/host.c) */
#define fb jt_oled_fb
#else
#include "regs.h"

#define PIN(n) (1u << (n))
#define RST PIN(5)
#define CS  PIN(6)
#define DC  PIN(15)
#define MOSI PIN(7)
#define SCK PIN(3)      /* on GPIOB */

static uint8_t fb[JT_OLED_W * JT_OLED_H / 8];

uint32_t jt_spi_spin = 6;        /* half-bit delay loops; set with the clock */

static void pace(void) {
    for (volatile uint32_t i = jt_spi_spin; i; i--) { }
}
static void spi_byte(uint8_t b) {
    for (int i = 0; i < 8; i++, b <<= 1) {
        GPIOA->BSRR = (b & 0x80) ? MOSI : (MOSI << 16);
        pace();
        GPIOB->BSRR = SCK;
        pace();
        GPIOB->BSRR = SCK << 16;
    }
}
static void cmd(uint8_t c) {
    GPIOA->BSRR = (CS | DC) << 16;
    spi_byte(c);
    GPIOA->BSRR = CS;
}

void jt_oled_init(void) {
    static const uint8_t seq[] = {
        0xAE, 0x20, 0x00, 0xB0, 0xC8, 0x00, 0x10, 0x40, 0x81, 0xFF, 0xA1, 0xA6, 0xA8, 0x1F,
        0xA4, 0xD3, 0x00, 0xD5, 0xF0, 0xD9, 0x22, 0xDA, 0x02, 0xDB, 0x20, 0x8D, 0x14, 0xAF,
    };
    GPIOB->BSRR = SCK << 16;
    GPIOA->BSRR = CS;
    GPIOA->BSRR = RST << 16;
    jt_delay_ms(10);
    GPIOA->BSRR = RST;
    jt_delay_ms(100);
    for (unsigned i = 0; i < sizeof seq; i++) cmd(seq[i]);
    jt_oled_clear();
    jt_oled_flush();
}

#endif /* !JT_HOST */

void jt_oled_clear(void) {
    for (unsigned i = 0; i < sizeof fb; i++) fb[i] = 0;
}

void jt_oled_pixel(int x, int y, bool on) {
    if (x < 0 || x >= JT_OLED_W || y < 0 || y >= JT_OLED_H) return;
    uint8_t *p = &fb[x + (y >> 3) * JT_OLED_W], m = (uint8_t)(1u << (y & 7));
    if (on) *p |= m; else *p &= (uint8_t)~m;
}

void jt_oled_text(int x, int y, const char *s) {
    for (; *s; s++, x += 8) {
        const char *g = font8x8_basic[(unsigned char)*s & 0x7F];
        for (int r = 0; r < 8; r++)
            for (int c = 0; c < 8; c++)
                jt_oled_pixel(x + c, y + r, (g[r] >> c) & 1);
    }
}

bool jt_oled_get(int x, int y) {
    if (x < 0 || x >= JT_OLED_W || y < 0 || y >= JT_OLED_H) return false;
    return fb[x + (y >> 3) * JT_OLED_W] >> (y & 7) & 1;
}

void jt_oled_line(int x0, int y0, int x1, int y1, bool on) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (int guard = 0; guard < 400; guard++) {
        jt_oled_pixel(x0, y0, on);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void jt_oled_text_big(int x, int y, const char *s, int scale) {
    for (; *s; s++, x += 8 * scale) {
        const char *g = font8x8_basic[(unsigned char)*s & 0x7F];
        for (int r = 0; r < 8 * scale; r++)
            for (int c = 0; c < 8 * scale; c++)
                if ((g[r / scale] >> (c / scale)) & 1) jt_oled_pixel(x + c, y + r, true);
    }
}

#ifndef JT_HOST
void jt_oled_invert(bool on) { cmd(on ? 0xA7 : 0xA6); }

void jt_oled_flush(void) {
    for (int page = 0; page < JT_OLED_H / 8; page++) {
        cmd((uint8_t)(0xB0 + page));
        cmd(0x00);
        cmd(0x10);
        GPIOA->BSRR = CS << 16;
        GPIOA->BSRR = DC;
        for (int i = 0; i < JT_OLED_W; i++) spi_byte(fb[page * JT_OLED_W + i]);
        GPIOA->BSRR = CS;
    }
}
#endif
