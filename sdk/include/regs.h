/* Minimal STM32G431 register definitions (only what the SDK uses). */
#ifndef JT_REGS_H
#define JT_REGS_H
#include <stdint.h>

#define REG32(a) (*(volatile uint32_t *)(a))

#define RCC_CR      REG32(0x40021000)
#define RCC_CFGR    REG32(0x40021008)
#define RCC_AHB2ENR REG32(0x4002104C)

typedef struct {
    volatile uint32_t MODER, OTYPER, OSPEEDR, PUPDR, IDR, ODR, BSRR;
} gpio_t;
#define GPIOA ((gpio_t *)0x48000000)
#define GPIOB ((gpio_t *)0x48000400)

#define SCB_ICSR  REG32(0xE000ED04)
#define SCB_VTOR  REG32(0xE000ED08)
#define SCB_CPACR REG32(0xE000ED88)
#define NVIC_ISER(n) REG32(0xE000E100 + 4 * (n))
#define RCC_PLLCFGR REG32(0x4002100C)
#define RCC_AHB1ENR REG32(0x40021048)
#define RCC_APB1ENR1 REG32(0x40021058)
#define RCC_APB2ENR REG32(0x40021060)
#define SCB_AIRCR REG32(0xE000ED0C)
#define SYST_CSR  REG32(0xE000E010)
#define SYST_RVR  REG32(0xE000E014)
#define SYST_CVR  REG32(0xE000E018)
#define NVIC_ICER(n) REG32(0xE000E180 + 4 * (n))
#define NVIC_ICPR(n) REG32(0xE000E280 + 4 * (n))

#endif
