/* Storage that survives power-off: 24 KiB of the synth's EEPROM.
 *
 * The EEPROM holds 32 KiB. The stock firmware keeps its presets in the first 2 KiB
 * (0x0000..0x07FF) and its settings and the bootloader's update flag in the last 192 bytes
 * (0x7F40..0x7FFF). The store is 0x1000..0x6FFF, which the stock firmware never reads or
 * writes, so presets survive custom firmware and saved data survives a return to stock.
 * Nothing here can address a byte outside that range. */
#include "jt4000.h"

#define EEPROM 0x50
#define BASE 0x1000
#define PAGE 64                             /* the chip writes at most one 64-byte page at a time */

/* The chip does not answer while a page is being written (up to 5 ms): ask until it does. */
static bool seek(uint16_t at) {
    uint8_t a[2] = {(uint8_t)((BASE + at) >> 8), (uint8_t)(BASE + at)};
    for (int i = 0; i < 100; i++)
        if (jt_i2c_write(EEPROM, a, 2)) return true;
    return false;
}

static bool inside(uint16_t at, int n) { return n >= 0 && at <= JT_STORE_SIZE && n <= JT_STORE_SIZE - at; }

bool jt_store_read(uint16_t at, void *data, int n) {
    if (!inside(at, n)) return false;
    return n == 0 || (seek(at) && jt_i2c_read(EEPROM, data, n));
}

bool jt_store_write(uint16_t at, const void *data, int n) {
    const uint8_t *p = data;
    if (!inside(at, n)) return false;
    while (n > 0) {
        int m = PAGE - at % PAGE;
        uint8_t msg[2 + PAGE], old[PAGE];
        bool same = true;
        if (m > n) m = n;
        if (!jt_store_read(at, old, m)) return false;
        for (int i = 0; i < m; i++) same &= old[i] == p[i];
        if (!same) {                        /* a page that already holds this is left alone: less wear */
            msg[0] = (uint8_t)((BASE + at) >> 8);
            msg[1] = (uint8_t)(BASE + at);
            for (int i = 0; i < m; i++) msg[2 + i] = p[i];
            if (!seek(at) || !jt_i2c_write(EEPROM, msg, 2 + m) || !seek(at)) return false;
        }
        at = (uint16_t)(at + m);
        p += m;
        n -= m;
    }
    return true;
}

/* ---- records: data with a name and a checksum in front ---- */
typedef struct { uint8_t tag[4], len[2], sum[2]; } head_t;

static uint16_t sum_more(uint16_t s, const uint8_t *p, int n) {     /* Fletcher's checksum */
    uint32_t a = s & 0xFF, b = s >> 8;
    for (int i = 0; i < n; i++) { a = (a + p[i]) % 255; b = (b + a) % 255; }
    return (uint16_t)(b << 8 | a);
}

bool jt_store_save(uint16_t at, uint32_t tag, const void *data, int n) {
    if (n < 0 || n > 0xFFFF || !inside(at, n + (int)sizeof(head_t))) return false;
    uint16_t sum = sum_more(0x0101, data, n);
    head_t h = {{(uint8_t)(tag >> 24), (uint8_t)(tag >> 16), (uint8_t)(tag >> 8), (uint8_t)tag},
                {(uint8_t)(n >> 8), (uint8_t)n}, {(uint8_t)(sum >> 8), (uint8_t)sum}};
    /* the data first: a save cut short by power-off leaves a record that fails its checksum */
    return jt_store_write((uint16_t)(at + sizeof h), data, n) && jt_store_write(at, &h, sizeof h);
}

bool jt_store_load(uint16_t at, uint32_t tag, void *data, int n) {
    head_t h;
    uint8_t part[32];
    uint16_t sum = 0x0101;
    if (n < 0 || !inside(at, n + (int)sizeof h) || !jt_store_read(at, &h, sizeof h)) return false;
    if (((uint32_t)h.tag[0] << 24 | (uint32_t)h.tag[1] << 16 | (uint32_t)h.tag[2] << 8 | h.tag[3]) != tag) return false;
    if ((h.len[0] << 8 | h.len[1]) != n) return false;
    for (int done = 0; done < n; done += (int)sizeof part) {        /* check it before touching `data` */
        int m = n - done < (int)sizeof part ? n - done : (int)sizeof part;
        if (!jt_store_read((uint16_t)(at + sizeof h + done), part, m)) return false;
        sum = sum_more(sum, part, m);
    }
    if (sum != (h.sum[0] << 8 | h.sum[1])) return false;
    return jt_store_read((uint16_t)(at + sizeof h), data, n);
}
