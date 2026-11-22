/* USB device: a class-compliant USB-MIDI interface (one IN, one OUT endpoint).
 *
 * The interface layout is the one the stock firmware presents (the standard USB-MIDI 1.0
 * example: 101-byte configuration, bulk endpoints 0x01 and 0x81, 64-byte packets) with
 * the synth's own USB IDs, 1397:127D. The strings say "SDK" so the two can be told apart.
 *
 * Clock: the 48 MHz internal oscillator, trimmed against the host's start-of-frame.
 * Everything runs from the USB interrupt; the application only reads and writes queues. */
#include "jt4000.h"
#include "regs.h"

#define RCC_APB1RSTR1 REG32(0x40021038)
#define RCC_CCIPR   REG32(0x40021088)
#define RCC_CRRCR   REG32(0x40021098)
#define CRS_CR      REG32(0x40002000)

#define USB_BASE    0x40005C00
#define EPR(n)      (*(volatile uint16_t *)(USB_BASE + 4 * (n)))
#define USB_CNTR    (*(volatile uint16_t *)(USB_BASE + 0x40))
#define USB_ISTR    (*(volatile uint16_t *)(USB_BASE + 0x44))
#define USB_DADDR   (*(volatile uint16_t *)(USB_BASE + 0x4C))
#define USB_BTABLE  (*(volatile uint16_t *)(USB_BASE + 0x50))
#define USB_BCDR    (*(volatile uint16_t *)(USB_BASE + 0x58))
#define PMA(off)    (*(volatile uint16_t *)(0x40006000 + (off)))

/* endpoint register: bits that toggle when written as 1, and bits that are plain */
#define EP_CTR_RX  0x8000
#define EP_SETUP   0x0800
#define EP_CTR_TX  0x0080
#define EP_PLAIN   0x8F8F            /* CTR_RX, SETUP, TYPE, KIND, CTR_TX, address */
#define ST_NAK     2
#define ST_VALID   3

/* packet memory layout (byte offsets): table, then four 64-byte buffers */
#define EP0_TX 0x40
#define EP0_RX 0x80
#define EP1_TX 0xC0
#define EP1_RX 0x100

static const uint8_t device_desc[18] = {
    18, 1, 0x00, 0x02, 0, 0, 0, 64, 0x97, 0x13, 0x7D, 0x12, 0x00, 0x02, 1, 2, 3, 1,
};
static const uint8_t config_desc[101] = {
    9, 2, 101, 0, 2, 1, 0, 0x80, 0xFA,              /* configuration: 2 interfaces, bus powered */
    9, 4, 0, 0, 0, 1, 1, 0, 0,                      /* audio control interface */
    9, 0x24, 1, 0x00, 0x01, 9, 0, 1, 1,
    9, 4, 1, 0, 2, 1, 3, 0, 0,                      /* MIDI streaming interface, 2 endpoints */
    7, 0x24, 1, 0x00, 0x01, 0x41, 0,
    6, 0x24, 2, 1, 1, 0,                            /* MIDI IN jack, embedded */
    6, 0x24, 2, 2, 2, 0,                            /* MIDI IN jack, external */
    9, 0x24, 3, 1, 3, 1, 2, 1, 0,                   /* MIDI OUT jack, embedded */
    9, 0x24, 3, 2, 4, 1, 1, 1, 0,                   /* MIDI OUT jack, external */
    9, 5, 0x01, 2, 64, 0, 0, 0, 0,                  /* bulk OUT */
    5, 0x25, 1, 1, 1,
    9, 5, 0x81, 2, 64, 0, 0, 0, 0,                  /* bulk IN */
    5, 0x25, 1, 1, 3,
};
static const char *const strings[] = {"JT-4000 SDK (unofficial)", "JT-4000 SDK"};

static const uint8_t *tx_ptr;       /* control IN data still to send */
static uint16_t tx_left;
static uint8_t pending_addr, configured;
static uint8_t scratch[64];

#define QN 64                        /* MIDI event queues, 4 bytes per event */
static volatile uint32_t rxq[QN], txq[QN];
static volatile uint8_t rx_head, rx_tail, tx_head, tx_tail, tx_busy;

static void ep_stat(int ep, int tx, int stat) {
    uint16_t mask = tx ? 0x0030 : 0x3000, v = EPR(ep);
    v = (uint16_t)((v & (EP_PLAIN | mask)) ^ (stat << (tx ? 4 : 12)));
    EPR(ep) = v | EP_CTR_RX | EP_CTR_TX;
}
static void ep_clear(int ep, uint16_t ctr) {
    EPR(ep) = (uint16_t)((EPR(ep) & EP_PLAIN & ~ctr) | ((EP_CTR_RX | EP_CTR_TX) & ~ctr));
}
static void pma_write(int off, const uint8_t *src, int n) {
    for (int i = 0; i < n; i += 2) PMA(off + i) = (uint16_t)(src[i] | (i + 1 < n ? src[i + 1] << 8 : 0));
}
static void pma_read(int off, uint8_t *dst, int n) {
    for (int i = 0; i < n; i += 2) {
        uint16_t w = PMA(off + i);
        dst[i] = (uint8_t)w;
        if (i + 1 < n) dst[i + 1] = (uint8_t)(w >> 8);
    }
}

static void ep0_send_chunk(void) {
    int n = tx_left > 64 ? 64 : tx_left;
    pma_write(EP0_TX, tx_ptr, n);
    PMA(2) = (uint16_t)n;                           /* COUNT0_TX */
    tx_ptr += n;
    tx_left = (uint16_t)(tx_left - n);
    ep_stat(0, 1, ST_VALID);
}
static void ep0_reply(const uint8_t *data, int len, int wanted) {
    tx_ptr = data;
    tx_left = (uint16_t)(len < wanted ? len : wanted);
    ep0_send_chunk();
}
static void ep0_stall(void) { ep_stat(0, 1, 1); ep_stat(0, 0, 1); }

static int string_desc(int index) {
    int n = 2;
    if (index == 0) {
        scratch[2] = 0x09; scratch[3] = 0x04;       /* English (US) */
        n = 4;
    } else if (index == 3) {                        /* serial number: the chip's unique ID */
        const uint8_t *uid = (const uint8_t *)0x1FFF7590;
        for (int i = 0; i < 6; i++) {
            scratch[n++] = "0123456789ABCDEF"[uid[i] >> 4]; scratch[n++] = 0;
            scratch[n++] = "0123456789ABCDEF"[uid[i] & 15]; scratch[n++] = 0;
        }
    } else if (index <= 2) {
        for (const char *s = strings[index - 1]; *s; s++) { scratch[n++] = (uint8_t)*s; scratch[n++] = 0; }
    } else {
        return 0;
    }
    scratch[0] = (uint8_t)n;
    scratch[1] = 3;
    return n;
}

static void setup(void) {
    uint8_t s[8];
    pma_read(EP0_RX, s, 8);
    int type = s[0], req = s[1], value = s[2] | s[3] << 8, len = s[6] | s[7] << 8, n;
    tx_left = 0;
    if ((type & 0x60) != 0) { ep0_stall(); return; }             /* no class or vendor requests */
    switch (req) {
    case 6:                                                      /* GET_DESCRIPTOR */
        if ((value >> 8) == 1) ep0_reply(device_desc, sizeof device_desc, len);
        else if ((value >> 8) == 2) ep0_reply(config_desc, sizeof config_desc, len);
        else if ((value >> 8) == 3 && (n = string_desc(value & 0xFF)) != 0) ep0_reply(scratch, n, len);
        else ep0_stall();
        break;
    case 5:                                                      /* SET_ADDRESS: applies after the status stage */
        pending_addr = (uint8_t)(value | 0x80);
        ep0_reply(scratch, 0, 0);
        break;
    case 9:                                                      /* SET_CONFIGURATION */
        configured = (uint8_t)value;
        PMA(8) = EP1_TX; PMA(10) = 0;
        PMA(12) = EP1_RX; PMA(14) = 0x8400;                      /* 64-byte receive buffer */
        EPR(1) = 0x0001;                                         /* bulk, endpoint number 1 */
        ep_stat(1, 0, ST_VALID);
        ep_stat(1, 1, ST_NAK);
        tx_busy = 0;
        ep0_reply(scratch, 0, 0);
        break;
    case 8:                                                      /* GET_CONFIGURATION */
        scratch[0] = configured;
        ep0_reply(scratch, 1, len);
        break;
    case 0:                                                      /* GET_STATUS */
        scratch[0] = scratch[1] = 0;
        ep0_reply(scratch, 2, len);
        break;
    case 1: case 3: case 11:                                     /* CLEAR/SET_FEATURE, SET_INTERFACE */
        ep0_reply(scratch, 0, 0);
        break;
    default:
        ep0_stall();
    }
}

static void midi_tx_kick(void) {
    int n = 0;
    if (tx_busy || !configured) return;
    while (tx_tail != tx_head && n < 64) {
        uint32_t e = txq[tx_tail];
        tx_tail = (uint8_t)((tx_tail + 1) % QN);
        PMA(EP1_TX + n) = (uint16_t)e;
        PMA(EP1_TX + n + 2) = (uint16_t)(e >> 16);
        n += 4;
    }
    if (!n) return;
    PMA(10) = (uint16_t)n;                          /* COUNT1_TX */
    tx_busy = 1;
    ep_stat(1, 1, ST_VALID);
}

/* The few system messages the SDK answers itself, in the stock firmware's format, so the
 * project's tools keep working: 02 = go to update mode, 08 = version. */
static void system_sysex(const uint8_t *m, int n) {
    static const uint8_t head[7] = {0xF0, 0x00, 0x20, 0x32, 0x00, 0x01, 0x38};
    if (n < 9) return;
    for (int i = 0; i < 7; i++) if (m[i] != head[i]) return;
    if (m[7] == 0x02) jt_enter_update_mode();       /* the flasher asks: does not return */
    if (m[7] == 0x08) {
        static const uint8_t v[13] = {0xF0, 0x00, 0x20, 0x32, 0x00, 0x01, 0x38, 0x09, 0x00,
                                      JT_SDK_VERSION_MAJOR, JT_SDK_VERSION_MINOR, JT_SDK_VERSION_PATCH, 0xF7};
        jt_midi_send_sysex(v, sizeof v);
    }
}

static void midi_rx(void) {
    static uint8_t sx[16];
    static int sxn;
    int n = PMA(14) & 0x3FF;
    for (int i = 0; i + 4 <= n; i += 4) {
        uint32_t e = PMA(EP1_RX + i) | (uint32_t)PMA(EP1_RX + i + 2) << 16;
        uint8_t next = (uint8_t)((rx_head + 1) % QN);
        if (next != rx_tail) { rxq[rx_head] = e; rx_head = next; }
        int cin = e & 0x0F, bytes = cin == 4 || cin == 7 ? 3 : cin == 6 ? 2 : cin == 5 ? 1 : 0;
        for (int b = 0; b < bytes; b++)
            if (sxn < (int)sizeof sx) sx[sxn++] = (uint8_t)(e >> (8 * (b + 1)));
        if (cin >= 5 && cin <= 7) { system_sysex(sx, sxn); sxn = 0; }
        if (cin > 7) sxn = 0;
    }
    ep_stat(1, 0, ST_VALID);
}

void USB_LP_IRQHandler(void) {
    uint16_t istr;
    while ((istr = USB_ISTR) & 0x8000) {            /* a transfer completed */
        int ep = istr & 0x0F;
        uint16_t r = EPR(ep);
        if (ep == 0) {
            if (r & EP_CTR_RX) {
                ep_clear(0, EP_CTR_RX);
                if (r & EP_SETUP) setup();
                ep_stat(0, 0, ST_VALID);
            }
            if (r & EP_CTR_TX) {
                ep_clear(0, EP_CTR_TX);
                if (pending_addr) { USB_DADDR = pending_addr; pending_addr = 0; }
                if (tx_left) ep0_send_chunk();
            }
        } else {
            if (r & EP_CTR_RX) { ep_clear(ep, EP_CTR_RX); midi_rx(); }
            if (r & EP_CTR_TX) { ep_clear(ep, EP_CTR_TX); tx_busy = 0; midi_tx_kick(); }
        }
    }
    if (istr & 0x0400) {                            /* bus reset */
        USB_ISTR = (uint16_t)~0x0400;
        USB_BTABLE = 0;
        PMA(0) = EP0_TX; PMA(2) = 0;
        PMA(4) = EP0_RX; PMA(6) = 0x8400;
        EPR(0) = 0x0200;                            /* control endpoint */
        ep_stat(0, 0, ST_VALID);
        ep_stat(0, 1, ST_NAK);
        USB_DADDR = 0x80;                           /* enabled, address 0 */
        configured = 0; tx_busy = 0; pending_addr = 0;
    }
    USB_ISTR = (uint16_t)(istr & 0x8400);           /* drop the other event flags */
}

void jt_usb_init(void) {
    RCC_CRRCR |= 1u;                                /* 48 MHz oscillator */
    for (int i = 0; i < 200000 && !(RCC_CRRCR & 2u); i++) { }
    RCC_CCIPR &= ~(3u << 26);                       /* USB clock = HSI48 */
    RCC_APB1ENR1 |= (1u << 23) | (1u << 8);         /* USB (bit 23, as stock does), clock recovery */
    (void)RCC_APB1ENR1;
    RCC_APB1RSTR1 |= 1u << 23;                      /* drop whatever the bootloader set up */
    RCC_APB1RSTR1 &= ~(1u << 23);
    CRS_CR |= (1u << 6) | (1u << 5);                /* trim HSI48 from the host's frames */

    USB_BCDR = 0;                                   /* detach long enough for the host to notice */
    USB_CNTR = 1;                                   /* powered, held in reset */
    jt_delay_ms(100);
    USB_CNTR = 0;
    USB_ISTR = 0;
    USB_BTABLE = 0;
    USB_CNTR = 0x8400;                              /* interrupts: transfer complete, bus reset */
    NVIC_ISER(0) = 1u << 20;                        /* USB low-priority interrupt */
    __asm__ volatile("cpsie i");
    USB_BCDR = 0x8000;                              /* pull-up on: attach */
}

bool jt_midi_get(uint8_t ev[4]) {
    if (rx_tail == rx_head) return false;
    uint32_t e = rxq[rx_tail];
    rx_tail = (uint8_t)((rx_tail + 1) % QN);
    ev[0] = (uint8_t)e; ev[1] = (uint8_t)(e >> 8); ev[2] = (uint8_t)(e >> 16); ev[3] = (uint8_t)(e >> 24);
    return true;
}

/* Queue access from the application is guarded by masking interrupts; the previous mask
 * is restored, so these are also safe to call from the USB interrupt itself. */
static uint32_t lock(void) {
    uint32_t m;
    __asm__ volatile("mrs %0, primask\n cpsid i" : "=r"(m));
    return m;
}
static void unlock(uint32_t m) { __asm__ volatile("msr primask, %0" : : "r"(m)); }

static void put(uint32_t e) {
    uint32_t m = lock();
    uint8_t next = (uint8_t)((tx_head + 1) % QN);
    if (next != tx_tail) { txq[tx_head] = e; tx_head = next; }
    midi_tx_kick();
    unlock(m);
}

void jt_midi_send(uint8_t status, uint8_t d1, uint8_t d2) {
    put((uint32_t)(status >> 4) | (uint32_t)status << 8 | (uint32_t)d1 << 16 | (uint32_t)d2 << 24);
}

void jt_midi_send_sysex(const uint8_t *m, int n) {
    uint32_t mask = lock();
    for (int i = 0; i < n; i += 3) {
        int left = n - i, cin = left > 3 ? 4 : 4 + left;        /* 5, 6, 7 = ends with 1, 2, 3 bytes */
        uint32_t e = (uint32_t)cin | (uint32_t)m[i] << 8;
        if (left > 1) e |= (uint32_t)m[i + 1] << 16;
        if (left > 2) e |= (uint32_t)m[i + 2] << 24;
        uint8_t next = (uint8_t)((tx_head + 1) % QN);
        if (next != tx_tail) { txq[tx_head] = e; tx_head = next; }
    }
    midi_tx_kick();
    unlock(mask);
}
