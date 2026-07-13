//
// Created by Alberto Furlan on 01/04/26.
//

#ifndef ES2025_BMP180_H
#define ES2025_BMP180_H

#include <dev/i2c/i2c.h>
#include <sys/_stdint.h>

#define BMP180_I2C_ADDR         0x77u
#define BMP180_CHIP_ID_VALUE    0x55u


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

#endif //ES2025_BMP180_H
