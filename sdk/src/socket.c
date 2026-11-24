/* The JT-4000M's MIDI socket: input only, on USART2.
 *
 * From stock JT-4000M firmware 1.1.6: USART2 at 31250 baud, 8 data bits, no parity
 * (its routine at 0x0800EEE4), receive pin PA15 as alternate function 7 and a circular
 * DMA transfer from the receive register (0x0800DB9C). No transmit pin is set up.
 *
 * This driver does the same with DMA1 channel 3 (channel 1 plays the audio, channel 2
 * reads the knobs). No interrupt is involved, so no byte is lost while the audio
 * interrupt runs; jt_midi_get parses whatever has arrived since it was last called.
 * Register addresses and bits: RM0440 (USART2 0x40004400, DMA request 26 = USART2_RX).
 * Checked in the simulator only: not yet tried on a JT-4000M. */
#include "jt4000.h"
#include "regs.h"

#define USART2_CR1  REG32(0x40004400)
#define USART2_CR3  REG32(0x40004408)
#define USART2_BRR  REG32(0x4000440C)
#define USART2_RDR  0x40004424
#define DMA_CCR3    REG32(0x40020030)
#define DMA_CNDTR3  REG32(0x40020034)
#define DMA_CPAR3   REG32(0x40020038)
#define DMA_CMAR3   REG32(0x4002003C)
#define DMAMUX_C2CR REG32(0x40020808)

#define RING 256
static uint8_t ring[RING];
static uint16_t rd;
static bool running;

static uint8_t status, have, need, data[2];         /* channel or system message being received */
static uint8_t sx[3], sxn;                          /* SysEx bytes not yet handed out */
static bool in_sysex;

void jt_midi_socket_init(void) {
    if (jt_board() != JT_BOARD_M || running) return;
    RCC_AHB1ENR |= 1u | (1u << 2);                  /* DMA1, DMAMUX */
    RCC_AHB2ENR |= 1u;                              /* GPIOA */
    RCC_APB1ENR1 |= 1u << 17;                       /* USART2 */
    (void)RCC_APB1ENR1;
    GPIOA->AFRH = (GPIOA->AFRH & ~(0xFu << 28)) | (7u << 28);       /* PA15 = USART2_RX */
    GPIOA->PUPDR = (GPIOA->PUPDR & ~(3u << 30)) | (1u << 30);       /* pull-up: idle when nothing drives it */
    GPIOA->MODER = (GPIOA->MODER & ~(3u << 30)) | (2u << 30);

    USART2_CR1 = 0;
    USART2_BRR = (jt_clock_hz + 15625) / 31250;     /* clocked from the bus clock = core clock */
    USART2_CR3 = (1u << 6) | (1u << 12);            /* DMA on receive; a missed byte must not stall it */
    DMA_CCR3 = 0;
    DMAMUX_C2CR = 26;                               /* request line: USART2_RX */
    DMA_CPAR3 = USART2_RDR;
    DMA_CMAR3 = (uint32_t)ring;
    DMA_CNDTR3 = RING;
    DMA_CCR3 = (1u << 7) | (1u << 5) | 1u;          /* bytes, memory increment, circular, on */
    USART2_CR1 = (1u << 2) | 1u;                    /* receiver, USART */
    running = true;
}

static bool event(uint8_t ev[4], int kind, uint8_t a, uint8_t b, uint8_t c) {
    ev[0] = (uint8_t)(JT_MIDI_SOCKET | kind);
    ev[1] = a; ev[2] = b; ev[3] = c;
    return true;
}

/* One byte of the MIDI stream in; true when it completed an event. Events have the
 * USB-MIDI shape, so firmware reads both sources the same way. */
static bool parse(uint8_t b, uint8_t ev[4]) {
    if (b >= 0xF8) return event(ev, 0xF, b, 0, 0);  /* clock, start, stop: may sit inside any message */
    if (b == 0xF0) { in_sysex = true; status = 0; sx[0] = b; sxn = 1; return false; }
    if (b == 0xF7) {
        if (!in_sysex) return false;
        in_sysex = false;
        sx[sxn++] = b;                              /* ends with 1, 2 or 3 bytes: kinds 5, 6, 7 */
        return event(ev, 4 + sxn, sx[0], sxn > 1 ? sx[1] : 0, sxn > 2 ? sx[2] : 0);
    }
    if (b & 0x80) {                                 /* a status byte also ends an unfinished SysEx */
        in_sysex = false;
        have = 0;
        status = b;
        if (b == 0xF6) { status = 0; return event(ev, 5, b, 0, 0); }
        if (b == 0xF4 || b == 0xF5) status = 0;     /* undefined */
        need = (b & 0xE0) == 0xC0 || b == 0xF1 || b == 0xF3 ? 1 : 2;
        return false;
    }
    if (in_sysex) {
        sx[sxn++] = b;
        if (sxn < 3) return false;
        sxn = 0;
        return event(ev, 4, sx[0], sx[1], sx[2]);
    }
    if (!status) return false;
    data[have++] = b;
    if (have < need) return false;
    have = 0;                                       /* the status stays: running status */
    uint8_t s = status;
    if (s >= 0xF0) { status = 0; return event(ev, need + 1, s, data[0], need == 2 ? data[1] : 0); }
    return event(ev, s >> 4, s, data[0], need == 2 ? data[1] : 0);
}

bool jt_midi_socket_get(uint8_t ev[4]) {
    if (!running) return false;
    uint16_t wr = (uint16_t)((RING - (DMA_CNDTR3 & 0xFFFF)) % RING);
    while (rd != wr) {
        uint8_t b = ring[rd];
        rd = (uint16_t)((rd + 1) % RING);
        if (parse(b, ev)) return true;
    }
    return false;
}
