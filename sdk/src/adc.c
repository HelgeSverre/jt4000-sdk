/* The three analog inputs on ADC1: PA2 (channel 3, cutoff knob), PA3 (channel 4,
 * resonance knob), PB0 (channel 15, keyboard sense line).
 *
 * The converter free-runs through the three channels in a fixed order and DMA1 channel 2
 * stores each result in its own slot, so jt_pot() only reads memory. An earlier version
 * selected a channel and started single conversions by hand; each call returned the
 * channel requested by the call before, which hid a wrong channel-to-knob table for a
 * while. Waiting for the converter to go idle froze the firmware. */
#include "jt4000.h"
#include "regs.h"

#define ADC1_ISR   REG32(0x50000000)
#define ADC1_CR    REG32(0x50000008)
#define ADC1_CFGR  REG32(0x5000000C)
#define ADC1_SMPR1 REG32(0x50000014)
#define ADC1_SMPR2 REG32(0x50000018)
#define ADC1_SQR1  REG32(0x50000030)
#define ADC12_CCR  REG32(0x50000308)
#define DMA_CCR2    REG32(0x4002001C)
#define DMA_CNDTR2  REG32(0x40020020)
#define DMA_CPAR2   REG32(0x40020024)
#define DMA_CMAR2   REG32(0x40020028)
#define DMAMUX_C1CR REG32(0x40020804)

static volatile uint16_t value[JT_POTS];

/* Bounded wait: a converter that never answers must not freeze the firmware. */
static void wait_for(volatile uint32_t *reg, uint32_t mask, uint32_t want) {
    for (int i = 0; i < 200000 && (*reg & mask) != want; i++) { }
}

void jt_adc_init(void) {
    RCC_AHB1ENR |= 1u | (1u << 2);                  /* DMA1, DMAMUX */
    RCC_AHB2ENR |= (1u << 13) | 3u;                 /* ADC12, GPIOA, GPIOB */
    (void)RCC_AHB2ENR;
    GPIOA->MODER |= (3u << 4) | (3u << 6);          /* PA2, PA3 analog */
    GPIOB->MODER |= 3u;                             /* PB0 analog */

    ADC12_CCR = (ADC12_CCR & ~(3u << 16)) | (3u << 16);   /* clock = HCLK / 4 */
    ADC1_CR = 0;                                    /* leave deep power-down */
    ADC1_CR = 1u << 28;                             /* voltage regulator on */
    jt_delay_ms(1);
    ADC1_CR = (1u << 28) | (1u << 31);              /* calibrate */
    wait_for(&ADC1_CR, 1u << 31, 0);
    jt_delay_ms(1);

    /* DMA1 channel 2: ADC data register -> value[], 16-bit, circular */
    DMA_CCR2 = 0;
    DMAMUX_C1CR = 5;                                /* request line: ADC1 */
    DMA_CPAR2 = 0x50000040;
    DMA_CMAR2 = (uint32_t)value;
    DMA_CNDTR2 = JT_POTS;
    DMA_CCR2 = (1u << 10) | (1u << 8) | (1u << 7) | (1u << 5) | 1u;

    ADC1_ISR = 0x7FF;                               /* clear all flags */
    ADC1_CR = (1u << 28) | 1u;                      /* enable */
    wait_for(&ADC1_ISR, 1u, 1u);
    ADC1_CFGR = (1u << 13) | (1u << 1) | 1u;        /* continuous, DMA circular, DMA on */
    ADC1_SMPR1 = (7u << 9) | (7u << 12);            /* 640.5 cycles: the pots are high-impedance */
    ADC1_SMPR2 = 7u << 15;
    ADC1_SQR1 = 2u | (3u << 6) | (4u << 12) | (15u << 18);   /* 3 conversions: channels 3, 4, 15 */
    ADC1_CR = (1u << 28) | 1u | (1u << 2);          /* start, and keep going */
}

uint16_t jt_pot(int n) {
    return n >= 0 && n < JT_POTS ? value[n] : 0;
}
