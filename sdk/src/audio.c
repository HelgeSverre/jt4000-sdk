/* Audio out and the analog filter's control voltages.
 *   DAC1 channel 1 on PA4, 12-bit. TIM6 ticks at 32 kHz; each tick the DAC takes the
 *   next sample from a 128-sample ring by DMA. Half-way and at the end of the ring an
 *   interrupt asks the application for the 64 samples that were just played out.
 *   Cutoff:    TIM8 CH3N on PB5, PWM period 700.
 *   Resonance: TIM4 CH1  on PB6, PWM period 700, value inverted.
 * This is the stock firmware's arrangement (0x080165FC, 0x0800AE54, 0x08010DD0). */
#include "jt4000.h"
#include "regs.h"

#define GPIOB_AFRL   REG32(0x48000420)
#define DAC_CR       REG32(0x50000800)
#define DAC_DHR12L1  REG32(0x5000080C)
#define DAC_MCR      REG32(0x5000083C)
#define DMA_ISR      REG32(0x40020000)
#define DMA_IFCR     REG32(0x40020004)
#define DMA_CCR1     REG32(0x40020008)
#define DMA_CNDTR1   REG32(0x4002000C)
#define DMA_CPAR1    REG32(0x40020010)
#define DMA_CMAR1    REG32(0x40020014)
#define DMAMUX_C0CR  REG32(0x40020800)

typedef struct {
    volatile uint32_t CR1, CR2, SMCR, DIER, SR, EGR, CCMR1, CCMR2, CCER, CNT, PSC, ARR, RCR,
                      CCR1, CCR2, CCR3, CCR4, BDTR;
} tim_t;
#define TIM4 ((tim_t *)0x40000800)
#define TIM6 ((tim_t *)0x40001000)
#define TIM8 ((tim_t *)0x40013400)

#define PWM_TOP 699
/* Timer clock for the two filter PWMs: 16 MHz gives a 22 kHz PWM, verified to control the
 * filter. Stock uses 40 MHz (61 kHz), not yet verified with this code. */
#ifndef JT_PWM_TIMER_HZ
#define JT_PWM_TIMER_HZ 16000000
#endif

static uint16_t ring[2 * JT_AUDIO_BLOCK];
static jt_audio_fn render;

static void fill(uint16_t *dst) {
    int16_t block[JT_AUDIO_BLOCK];
    render(block, JT_AUDIO_BLOCK);
    for (int i = 0; i < JT_AUDIO_BLOCK; i++) dst[i] = (uint16_t)(block[i] + 0x8000);
}

void DMA1_CH1_IRQHandler(void) {
    uint32_t flags = DMA_ISR;
    DMA_IFCR = flags & 0xFu;
    if (flags & (1u << 2)) fill(&ring[0]);                   /* first half just played */
    if (flags & (1u << 1)) fill(&ring[JT_AUDIO_BLOCK]);      /* second half just played */
}

void jt_audio_init(jt_audio_fn fn) {
    render = fn;
    for (unsigned i = 0; i < 2 * JT_AUDIO_BLOCK; i++) ring[i] = 0x8000;

    RCC_AHB1ENR |= 1u | (1u << 2);                  /* DMA1, DMAMUX */
    RCC_AHB2ENR |= (1u << 16) | 3u;                 /* DAC1, GPIOA, GPIOB */
    RCC_APB1ENR1 |= (1u << 2) | (1u << 4);          /* TIM4, TIM6 */
    RCC_APB2ENR |= 1u << 13;                        /* TIM8 */
    (void)RCC_APB2ENR;

    /* filter control: PB5 = TIM8 CH3N (AF3), PB6 = TIM4 CH1 (AF2) */
    GPIOB_AFRL = (GPIOB_AFRL & ~((0xFu << 20) | (0xFu << 24))) | (3u << 20) | (2u << 24);
    GPIOB->MODER = (GPIOB->MODER & ~((3u << 10) | (3u << 12))) | (2u << 10) | (2u << 12);
    uint32_t psc = jt_clock_hz / JT_PWM_TIMER_HZ;
    psc = psc ? psc - 1 : 0;
    TIM8->PSC = psc; TIM8->ARR = PWM_TOP;
    TIM8->CCMR2 = (6u << 4) | (1u << 3);            /* channel 3: PWM mode 1, preload */
    TIM8->CCER = 1u << 10;                          /* complementary output only */
    TIM8->BDTR = 1u << 15;                          /* main output enable */
    TIM8->EGR = 1; TIM8->CR1 = 1;
    TIM4->PSC = psc; TIM4->ARR = PWM_TOP;
    TIM4->CCMR1 = (6u << 4) | (1u << 3);            /* channel 1: PWM mode 1, preload */
    TIM4->CCER = 1u;
    TIM4->EGR = 1; TIM4->CR1 = 1;
    jt_filter(1000, 0);

    /* DMA1 channel 1: ring -> DAC, circular, interrupts at half and full */
    DMA_CCR1 = 0;
    DMAMUX_C0CR = 6;                                /* request line: DAC1 channel 1 */
    DMA_CPAR1 = 0x5000080C;
    DMA_CMAR1 = (uint32_t)ring;
    DMA_CNDTR1 = 2 * JT_AUDIO_BLOCK;
    DMA_IFCR = 0xFu;
    DMA_CCR1 = (2u << 12) | (1u << 10) | (2u << 8) | (1u << 7) | (1u << 5) | (1u << 4)
             | (1u << 2) | (1u << 1) | 1u;          /* high prio, 16-bit mem, 32-bit periph, inc, circ, to periph */

    /* DAC: triggered by TIM6, DMA requests on, output buffered to PA4 */
    GPIOA->MODER |= 3u << 8;
    DAC_MCR = jt_clock_hz > 160000000 ? (2u << 14) : jt_clock_hz > 80000000 ? (1u << 14) : 0;
    DAC_DHR12L1 = 0x8000;
    DAC_CR = (1u << 12) | (7u << 2) | (1u << 1) | 1u;   /* DMA, trigger 7 = TIM6, trigger on, enable */

    TIM6->PSC = 0; TIM6->ARR = jt_clock_hz / JT_SAMPLE_RATE - 1;
    TIM6->CR2 = 2u << 4;                            /* update event -> trigger output */
    TIM6->EGR = 1; TIM6->SR = 0;

    NVIC_ISER(0) = 1u << 11;                        /* DMA1 channel 1 */
    __asm__ volatile("cpsie i");
    TIM6->CR1 = 1;
}

void jt_filter(uint16_t cutoff, uint16_t resonance) {
    if (cutoff > 1000) cutoff = 1000;
    if (resonance > 1000) resonance = 1000;
    TIM8->CCR3 = (uint32_t)cutoff * PWM_TOP / 1000;
    TIM4->CCR1 = (uint32_t)(1000 - resonance) * PWM_TOP / 1000;
}
