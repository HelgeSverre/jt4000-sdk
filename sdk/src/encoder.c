/* The SELECT knob: a quadrature encoder on PA0 / PA1, counted by TIM2 in encoder mode
 * (the stock firmware routes those pins to TIM2 CH1/CH2 as well). */
#include "jt4000.h"
#include "regs.h"

#define RCC_APB1ENR1 REG32(0x40021058)
#define TIM2_CR1   REG32(0x40000000)
#define TIM2_SMCR  REG32(0x40000008)
#define TIM2_CCMR1 REG32(0x40000018)
#define TIM2_CCER  REG32(0x40000020)
#define TIM2_CNT   REG32(0x40000024)
#define TIM2_ARR   REG32(0x4000002C)
#define GPIOA_AFRL REG32(0x48000020)

void jt_encoder_init(void) {
    RCC_AHB2ENR |= 1u;
    RCC_APB1ENR1 |= 1u;                             /* TIM2 */
    (void)RCC_APB1ENR1;
    GPIOA->MODER = (GPIOA->MODER & ~0xFu) | 0xAu;   /* PA0, PA1 alternate function */
    GPIOA->PUPDR = (GPIOA->PUPDR & ~0xFu) | 0x5u;   /* pull-ups */
    GPIOA_AFRL = (GPIOA_AFRL & ~0xFFu) | 0x11u;     /* AF1 = TIM2 CH1, CH2 */
    TIM2_CR1 = 0;
    TIM2_ARR = 0xFFFFFFFF;
    TIM2_CCMR1 = 0x0101 | (0xF << 4) | (0xF << 12); /* both inputs, maximum filter */
    TIM2_CCER = 0;
    TIM2_SMCR = 3;                                  /* count on both edges of both inputs */
    TIM2_CNT = 0;
    TIM2_CR1 = 1;
}

int32_t jt_encoder(void) { return (int32_t)TIM2_CNT; }
