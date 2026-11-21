/* Bit-banged I2C master for the synth's two buses:
 *   bus 1: PB7 (SDA) / PB8 (SCL): EEPROM (0x50) and LED/key chip (0x70)
 *   bus 2: PA8 (SDA) / PA9 (SCL): touch-keyboard controller (0x65)
 * Open-drain GPIO with busy-wait delays, so it works at any clock. */
#include "jt4000.h"
#include "regs.h"

typedef struct { gpio_t *g; uint32_t sda, scl; int sda_pin, scl_pin; } bus_t;
static const bus_t BUS1 = {GPIOB, 1u << 7, 1u << 8, 7, 8};
static const bus_t BUS2 = {GPIOA, 1u << 8, 1u << 9, 8, 9};
static const bus_t *b = &BUS1;

uint32_t jt_i2c_spin = 300;      /* half-bit delay loops; 300 is safe at 170 MHz */

static void spin(uint32_t n) {
    for (volatile uint32_t i = n; i; i--) { }
}
static void sda(int v) { b->g->BSRR = v ? b->sda : b->sda << 16; spin(jt_i2c_spin); }
static void scl(int v) { b->g->BSRR = v ? b->scl : b->scl << 16; spin(jt_i2c_spin); }

static void bus_init(const bus_t *bus, bool pullups) {
    b = bus;
    RCC_AHB2ENR |= 3u;
    (void)RCC_AHB2ENR;
    b->g->BSRR = b->sda | b->scl;                   /* released = high */
    b->g->OTYPER |= b->sda | b->scl;                /* open drain */
    uint32_t m = (3u << (b->sda_pin * 2)) | (3u << (b->scl_pin * 2));
    if (pullups) b->g->PUPDR = (b->g->PUPDR & ~m) | (0x55555555u & m);
    b->g->MODER = (b->g->MODER & ~m) | (0x55555555u & m);
    spin(10 * jt_i2c_spin);
    for (int i = 0; i < 9; i++) { scl(0); scl(1); } /* free a slave stuck mid-byte */
}

static void start(void) { sda(1); scl(1); sda(0); scl(0); }
static void stop(void)  { sda(0); scl(1); sda(1); }

static bool put(uint8_t v) {
    for (int i = 0; i < 8; i++, v <<= 1) { sda(v & 0x80); scl(1); scl(0); }
    sda(1); scl(1);
    bool ack = !(b->g->IDR & b->sda);
    scl(0);
    return ack;
}
static uint8_t get(bool ack) {
    uint8_t v = 0;
    sda(1);
    for (int i = 0; i < 8; i++) { scl(1); v = (uint8_t)(v << 1 | !!(b->g->IDR & b->sda)); scl(0); }
    sda(!ack); scl(1); scl(0); sda(1);
    return v;
}

static bool wr(uint8_t addr, const uint8_t *data, int n) {
    start();
    bool ok = put((uint8_t)(addr << 1));
    for (int i = 0; i < n; i++) ok &= put(data[i]);
    stop();
    return ok;
}
static bool rd(uint8_t addr, uint8_t *data, int n) {
    start();
    bool ok = put((uint8_t)(addr << 1 | 1));
    for (int i = 0; i < n; i++) data[i] = get(i < n - 1);
    stop();
    return ok;
}

void jt_i2c_init(void) { bus_init(&BUS1, false); }  /* this bus has pull-ups on the board */
bool jt_i2c_write(uint8_t addr, const uint8_t *data, int n) { b = &BUS1; return wr(addr, data, n); }
bool jt_i2c_read(uint8_t addr, uint8_t *data, int n) { b = &BUS1; return rd(addr, data, n); }
void jt_i2c_stop_delay(uint32_t loops) { spin(loops); }

/* Touch keyboard: a plain 2-byte read returns one bit per key, as the stock firmware
 * does at 0x0801084C. Stock enables the MCU's pull-ups on this bus, so we do too. */
void jt_keyboard_init(void) { bus_init(&BUS2, true); }
uint16_t jt_keyboard(void) {
    uint8_t v[2] = {0, 0};
    b = &BUS2;
    if (!rd(0x65, v, 2)) return 0;
    return (uint16_t)(v[0] | v[1] << 8);
}

int jt_keyboard_lowest(uint16_t bits) {
    for (int k = 0; k < JT_KEYBOARD_KEYS; k++)
        if (bits & JT_KEYBOARD_BIT(k)) return k;
    return -1;
}
