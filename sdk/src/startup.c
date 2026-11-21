/* Vector table and reset handler. The image is linked at 0x08008000, after the
 * factory bootloader, which jumps here with its own clocks and interrupts still set up. */
#include <stdint.h>
#include "regs.h"

extern uint32_t _estack, _sidata, _sdata, _edata, _sbss, _ebss;
int main(void);

void Default_Handler(void) { for (;;) { } }
static void Null_Handler(void) { }
void DMA1_CH1_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void USB_LP_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));

#ifdef JT_PROBE
/* Bring-up probe: run the first JT_PROBE start-up steps, then reset. With OSC + SETTINGS
 * held, the bootloader stops in update mode after the reset, so "the serial port came
 * back" means every step up to JT_PROBE executed. */
__attribute__((naked)) void Reset_Handler(void) {
    __asm__(
#if JT_PROBE >= 1
        "push {r3, lr}\n"                                   /* first stack write */
#endif
#if JT_PROBE >= 2
        "cpsid i\n"
#endif
#if JT_PROBE >= 3
        "ldr r0, =0xE000ED08\n ldr r1, =0x08008000\n str r1, [r0]\n"   /* VTOR */
#endif
#if JT_PROBE >= 4
        "ldr r0, =0xE000E010\n movs r1, #0\n str r1, [r0]\n"           /* SysTick off */
#endif
#if JT_PROBE >= 5
        "ldr r0, =0xE000E180\n mvn r1, #0\n str r1, [r0]\n str r1, [r0, #4]\n"
        "str r1, [r0, #8]\n str r1, [r0, #12]\n"                        /* NVIC disable */
#endif
        "ldr r0, =0xE000ED0C\n ldr r1, =0x05FA0004\n str r1, [r0]\n b .\n .ltorg\n");
}
#else
__attribute__((used)) static void jt_start(void) {
    SCB_VTOR = 0x08008000;
    SCB_CPACR |= 0xFu << 20;                        /* FPU on: the SDK is built hard-float */
    SYST_CSR = 0;
    SCB_ICSR = (1u << 25) | (1u << 27);             /* drop a pending SysTick / PendSV */
    for (int i = 0; i < 4; i++) { NVIC_ICER(i) = 0xFFFFFFFF; NVIC_ICPR(i) = 0xFFFFFFFF; }
    for (uint32_t *s = &_sidata, *d = &_sdata; d < &_edata;) *d++ = *s++;
    for (uint32_t *d = &_sbss; d < &_ebss;) *d++ = 0;
    main();
    for (;;) { }
}

/* The bootloader arrives here with interrupts enabled, its 1 ms SysTick still running
 * and SP = vector[0] + 8. Mask interrupts and set the stack before anything else. */
__attribute__((naked)) void Reset_Handler(void) {
    __asm__("cpsid i\n ldr r0, =_estack\n mov sp, r0\n b jt_start\n .ltorg\n");
}
#endif

#define IRQS 102
__attribute__((section(".vectors"), used))
void (*const vectors[16 + IRQS])(void) = {
    (void (*)(void))&_estack, Reset_Handler,
    [2 ... 16 + IRQS - 1] = Default_Handler,
    [11] = Null_Handler, [14] = Null_Handler, [15] = Null_Handler,   /* SVC, PendSV, SysTick */
    [16 + 11] = DMA1_CH1_IRQHandler,
    [16 + 20] = USB_LP_IRQHandler,
};
