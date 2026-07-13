//
// Minimal hardware I2C1 bus driver for the STM32F4 BSP.
//
// The stm32f4 BSP ships no driver for the modern <dev/i2c/i2c.h> framework
// (only the legacy stm32f4_i2c API, which is disabled and whose F4XXXX GPIO mux
// is "#error Not implemented"). This registers a polled master bus that plugs
// into the generic framework so i2c_dev_alloc_and_init()/ioctl() work.
//
// Pins: PB6 = SCL, PB7 = SDA (AF4, open-drain). Standard mode 100 kHz.
//

#ifndef ES2025_I2C1_H
#define ES2025_I2C1_H

/**
 * @brief Bring up I2C1 (clocks + PB6/PB7 AF mux + peripheral) and register it
 *        as a modern i2c_bus at @p bus_path (e.g. "/dev/i2c-1").
 *
 * @return 0 on success, -1 on error (errno set).
 */
int stm32f4_register_i2c1(const char* bus_path);

#endif //ES2025_I2C1_H
