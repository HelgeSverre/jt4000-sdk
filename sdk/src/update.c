/* Enter the bootloader's update mode from running firmware.
 *
 * The bootloader reads EEPROM address 0x7FC0 at start-up and stays in update mode when it
 * finds 0xAA there (it clears the byte itself). Uses the bit-banged I2C at its slowest,
 * clock-independent setting, so it works in any state, including from an interrupt: the
 * USB driver calls it when the flasher asks, whatever the main loop is doing. */
#include "jt4000.h"

extern uint32_t jt_i2c_spin;
void jt_i2c_stop_delay(uint32_t loops);
void jt_i2c_recover(void);

void jt_enter_update_mode(void) {
    static const uint8_t set_flag[] = {0x7F, 0xC0, 0xAA};
    jt_i2c_spin = 300;
    jt_i2c_init();
    /* This can run from the USB interrupt, in the middle of the main loop's own transfer or
     * while the EEPROM is busy writing a page: free the bus, then ask until it answers. */
    jt_i2c_recover();
    for (int i = 0; i < 200 && !jt_i2c_write(0x50, set_flag, sizeof set_flag); i++) { }
    jt_i2c_stop_delay(1000000);                     /* EEPROM write cycle: >= 10 ms at 170 MHz */
    jt_reset();
}
