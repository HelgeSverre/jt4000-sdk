#include "jt4000.h"
#include "regs.h"

/* Pins (docs/HARDWARE.md): OLED reset PA5, chip select PA6, data/command PA15,
 * data PA7, clock PB3. Buttons: OSC PB4, SETTINGS PA10, both low when pressed. */
#define PIN(n) (1u << (n))

static void mode(gpio_t *g, int pin, uint32_t m) {
    g->MODER = (g->MODER & ~(3u << (pin * 2))) | (m << (pin * 2));
}
static void pullup(gpio_t *g, int pin) {
    g->PUPDR = (g->PUPDR & ~(3u << (pin * 2))) | (1u << (pin * 2));
}

uint32_t jt_clock_hz = 170000000;
extern uint32_t jt_i2c_spin, jt_spi_spin;

static void set_clock(uint32_t hz) {
    jt_clock_hz = hz;
    jt_i2c_spin = hz > 20000000 ? 300 : 30;         /* about 30 kHz bit rate either way */
    jt_spi_spin = hz > 20000000 ? 6 : 0;            /* keep the OLED clock under 4 MHz */
}

void jt_clock_hsi16(void) {
    RCC_CR |= 1u << 8;                              /* HSION */
    while (!(RCC_CR & (1u << 10))) { }              /* HSIRDY */
    RCC_CFGR = (RCC_CFGR & ~3u) | 1u;               /* SW = HSI16 */
    while (((RCC_CFGR >> 2) & 3u) != 1u) { }
    RCC_CFGR = 1u;                                  /* no prescalers */
    RCC_CR &= ~((1u << 24) | (1u << 16));           /* PLL and HSE off */
    set_clock(16000000);
}

/* 8 MHz crystal / 2 x 85 / 2 = 170 MHz, the bootloader's and stock firmware's setting.
 * The bootloader always hands over with this already running, so normally this only
 * records the fact. The full sequence is for code that switched to HSI first. */
void jt_clock_170mhz(void) {
    if (((RCC_CFGR >> 2) & 3u) != 3u) {
        RCC_APB1ENR1 |= 1u << 28;                   /* PWR */
        (void)RCC_APB1ENR1;
        REG32(0x40007080) &= ~(1u << 8);            /* PWR_CR5: range 1 boost */
        REG32(0x40022000) = (REG32(0x40022000) & ~0xFu) | 4u;   /* flash: 4 wait states */
        while ((REG32(0x40022000) & 0xFu) != 4u) { }
        RCC_CR |= 1u << 16;                         /* HSE */
        while (!(RCC_CR & (1u << 17))) { }
        RCC_CR &= ~(1u << 24);
        while (RCC_CR & (1u << 25)) { }
        RCC_PLLCFGR = 3u | (1u << 4) | (85u << 8) | (1u << 24);
        RCC_CR |= 1u << 24;
        while (!(RCC_CR & (1u << 25))) { }
        RCC_CFGR = (RCC_CFGR & ~0xF0u) | 0x80u;     /* AHB / 2 while crossing 80 MHz */
        RCC_CFGR = (RCC_CFGR & ~3u) | 3u;
        while (((RCC_CFGR >> 2) & 3u) != 3u) { }
        for (volatile int i = 0; i < 200; i++) { }
        RCC_CFGR &= ~0xF0u;
    }
    set_clock(170000000);
}

void jt_delay_ms(uint32_t ms) {
    SYST_RVR = jt_clock_hz / 1000 - 1;
    SYST_CVR = 0;
    SYST_CSR = 5;                                   /* core clock, no interrupt */
    while (ms) {
        if (SYST_CSR & (1u << 16)) ms--;
    }
    SYST_CSR = 0;
}

void jt_reset(void) {
    SCB_AIRCR = 0x05FA0004;
    for (;;) { }
}

bool jt_button_osc(void)      { return !(GPIOB->IDR & PIN(4)); }
bool jt_button_settings(void) { return !(GPIOA->IDR & PIN(10)); }

void jt_poll_escape(void) {
    if (jt_usb_update_request) jt_enter_update_mode();   /* asked for over USB (make flash) */
    if (jt_button_osc() && jt_button_settings()) {
        jt_delay_ms(30);
        if (jt_button_osc() && jt_button_settings()) jt_reset();
    }
}

void jt_oled_init(void);

void jt_gpio_init(void) {
    RCC_AHB2ENR |= 3u;                              /* GPIOA, GPIOB clocks */
    (void)RCC_AHB2ENR;
    GPIOA->BSRR = PIN(5) | PIN(6);                  /* reset and chip select idle high */
    mode(GPIOA, 5, 1); mode(GPIOA, 6, 1); mode(GPIOA, 15, 1); mode(GPIOA, 7, 1);
    mode(GPIOB, 3, 1);
    GPIOA->OSPEEDR |= (1u << 10) | (1u << 12) | (1u << 14) | (1u << 30);   /* medium speed edges */
    GPIOB->OSPEEDR |= 1u << 6;
    mode(GPIOA, 10, 0); pullup(GPIOA, 10);
    mode(GPIOB, 4, 0);  pullup(GPIOB, 4);
}

void jt_init(void) {
    jt_clock_170mhz();
    jt_gpio_init();
    jt_oled_init();
    jt_panel_init();
    jt_adc_init();
    jt_encoder_init();
    jt_keyboard_init();
}
