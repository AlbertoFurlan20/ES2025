//
// Created by Alberto Furlan on 01/04/26.
//

#ifndef ES2025_BMP180_IOCTLS_H
#define ES2025_BMP180_IOCTLS_H

#include <dev/i2c/i2c.h>
#include <sys/_stdint.h>

#define BMP180_I2C_ADDR         0x77u
/* The expected chip-id value lives in bmp180/regs.h as BMP180_CHIP_ID_EXPECTED,
 * with the register address it is read from. This header defines the public
 * ioctl ABI and has no business carrying a second name for it. */


/**
 * @brief IOCTL commands for BMP180 device driver
 */

/**
 * @brief Triggers a measurement cycle and retrieves the results.
 *
 * @details Steps:
 *          1. Triggers a temperature + pressure measurement cycle
 *          2. Stores the results.
 *
 * @param 'B' is the group command of the BMP180 driver.
 * @param 0x01 is the unique command number.
 * @param @see bmp180_measurement_t is the type of the argument.
 */
#define BMP180_IOCTL_READ_MEASUREMENT   _IOW('B', 0x01, bmp180_measurement_t)

/**
 * @brief Changes the oversampling setting for subsequent measurements.
 *
 * @param 'B' is the group command of the BMP180 driver.
 * @param 0x02 is the unique command number.
 * @param @see bmp180_oss_t is the type of the argument.
 */
#define BMP180_IOCTL_SET_OSS            _IOW('B', 0x02, bmp180_oss_t)

/**
 * @brief Retrieves the currently configured oversampling setting.
 *
 * @param 'B' is the group command of the BMP180 driver.
 * @param 0x03 is the unique command number.
 * @param @see bmp180_oss_t is the type of the argument.
 */
#define BMP180_IOCTL_GET_OSS            _IOR('B', 0x03, bmp180_oss_t)

/**
 * @brief Writes 0xB6 to the soft-reset register
 *        (equivalent to power-on reset)
 *
 * @param 'B' is the group command of the BMP180 driver.
 * @param 0x04 is the unique command number.
 *
 * @note _IO commands just send a command that requires no extra data payload
 */
#define BMP180_IOCTL_SOFT_RESET         _IO('B', 0x04)

/**
 * @brief Sets how often the temperature is re-measured, in milliseconds.
 *
 * @details Pressure compensation needs a temperature, not a *fresh* temperature.
 *          Between re-conversions the cached value is reused and a measurement
 *          costs one pressure conversion instead of two conversions. 0 restores
 *          a temperature conversion per measurement.
 *
 * @param 'B' is the group command of the BMP180 driver.
 * @param 0x05 is the unique command number.
 * @param uint32_t interval in ms.
 */
#define BMP180_IOCTL_SET_TEMP_INTERVAL  _IOW('B', 0x05, uint32_t)

/**
 * @brief Retrieves the configured temperature re-conversion interval.
 *
 * @param 'B' is the group command of the BMP180 driver.
 * @param 0x06 is the unique command number.
 * @param uint32_t interval in ms.
 */
#define BMP180_IOCTL_GET_TEMP_INTERVAL  _IOR('B', 0x06, uint32_t)

#endif //ES2025_BMP180_IOCTLS_H
