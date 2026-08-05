//
// Minimal hardware I2C bus driver for the STM32F4 BSP.
//
// The stm32f4 BSP ships no driver for the modern <dev/i2c/i2c.h> framework, so
// this provides a polled master bus that plugs into it, making
// i2c_dev_alloc_and_init()/ioctl() work.
//
// The transfer engine is instance-agnostic: everything that differs between
// I2C1, I2C2 and I2C3 is in stm32f4_i2c_hw below.
//

#ifndef ES2025_I2C_H
#define ES2025_I2C_H

#include <cstdint>

#include <bsp/rcc.h> // stm32f4_rcc_index

/**
 * @brief Everything that differs between the STM32F4's three I2C peripherals.
 *
 * @details The rest of the driver — start/address/data/stop sequencing, status
 *          polling, clock math — is identical for all three and reads the
 *          register block through a pointer.
 */
struct stm32f4_i2c_hw
{
    uintptr_t base;             ///< Peripheral register block, APB1.
    stm32f4_rcc_index rcc;      ///< To gate the peripheral clock.
    int af;                     ///< GPIO alternate-function for this peripheral.
    unsigned scl;               ///< SCL pin, as STM32F4_GPIO_PIN(bank, pin).
    unsigned sda;               ///< SDA pin, same encoding.
};

/** @brief I2C1 on PB6/PB7 — the BMP180 bus on this board. */
extern const stm32f4_i2c_hw STM32F4_I2C1_HW;

/**
 * @brief Bring up an I2C peripheral (clock + pin mux + reset) and register it
 *        as a modern i2c_bus at @p bus_path (e.g. "/dev/i2c-1").
 *
 * @param bus_path node to publish, e.g. "/dev/i2c-1"
 * @param hw which peripheral, e.g. @see STM32F4_I2C_HW
 *
 * @return 0 on success, -1 on error (errno set).
 */
int stm32f4_register_i2c(const char* bus_path, const stm32f4_i2c_hw& hw);

#endif //ES2025_I2C_H
