//
// Created by Alberto Furlan on 01/04/26.
//

#ifndef ES2025_BMP_INC_H
#define ES2025_BMP_INC_H

#include <cstring>
#include <bits/stl_pair.h>

#include <rtems.h>

#include "bmp180_ioctls.h"
#include "bmp_types.h"

namespace bmp
{
    /**
     * @brief Util to write onto registers
     */
    static int bmp180_write_reg(const i2c_dev* dev,
                                uint8_t reg,
                                uint8_t value);

    /**
     * @brief Util to read from registers
     */
    static int bmp180_read_regs(const i2c_dev* dev,
                                uint8_t reg,
                                uint8_t* dst,
                                uint16_t len);
    /**
     * @brief Loads the calibration values in the device node
     *
     * @param self device node
     *
     * @return 0 if no errors, error code elsewhere
     */
    static int bmp180_load_calibration(bmp180_dev_t* self);

    /**
 * @brief Reads the uncompensated temperature value from the sensor
 *
 * @param self the device node you're reading
 * @param ut_out ptr to save the uncompensated pressure value to
 *
 * @details Steps:
 *          1. Trigger pressure measurement
 *          2. Wait for conversion (max 4.5 ms, we wait 5 ms)
 *          3. Read 16-bit result from 0xF6 (MSB) and 0xF7 (LSB)
 *
 * @return 0 if the read is successful, error code otherwise
 */
    static int bmp180_read_ut(const bmp180_dev_t* self, int32_t* ut_out);

    /**
 * @brief Reads the uncompensated pressure value from the sensor
 *
 * @param self the device node you're reading
 * @param up_out ptr to save the uncompensated pressure value to
 *
 * @details Steps:
 *          1. Trigger pressure measurement
 *          2. Wait for conversion
 *          3. Read 19-bit raw value: MSB (0xF6), LSB (0xF7), XLSB (0xF8)
 *
 * @return 0 if the read is successful, error code otherwise
 */
    static int bmp180_read_up(const bmp180_dev_t* self, int32_t* up_out);

    /**
 * @name Bosch compensation algorithm [PP 15 docs]
 *
 * @brief Perform compensation on raw temperature and pressure readings, computing "true" values in physical units.
 *
 * @details Steps:
 *          1. Temperature compensation
 *          2. Pressure compensation
 *
 * @param cal the calibration parameters
 * @param ut the raw temperature
 * @param up the raw pressure
 * @param oss the oversampling setting
 * @param temp_cdeg_out ptr to save the temperature to
 * @param pressure_pa_out ptr to save the pression to
 */
    static void bmp180_compensate(
        const bmp180_calib_t* cal,
        int32_t ut,
        int32_t up,
        uint8_t oss,
        int32_t* temp_cdeg_out,
        int32_t* pressure_pa_out);

    /**
 * @brief Perform a full measurement sequence: trigger temperature and pressure measurements, read raw values, compensate and compute "true" values.
 *
 * @details This function also performs compasation on readings
 *
 * @param self device you're reading
 * @param result ptr to the measurement obj
 *
 * @todo Check documentation to see if it's possible that the sensor de-calibrates on run-time.
 *       - If not, remove first if branch to ease compiler life
 *
 * @return 0 if the both @see bmp180_read_ut and @see bmp180_read_up succeed and the compensation is done, error code otherwise.
 */
    static int bmp180_do_measurement(bmp180_dev_t* self,
                                     bmp180_measurement_t* result);

    /**
 * @brief Wrapper for ioctl calls to device
 *
 * @param base target device
 * @param cmd command code
 * @param arg eventual args
 *
 * @note Cheatsheet [PP 5.1] tells to wait at least 10 ms after reset before communicating
 *
 * @return 0 or the error code
 */
    static int bmp180_ioctl(i2c_dev* base, ioctl_command_t cmd, void* arg);

    /**
     * @brief Wrapper for @see i2c_dev_destroy_and_free method.
     *
     * @param base address of the device to destroy
     */
    static void bmp180_destroy(i2c_dev* base);

    /**
     * @brief Registers a new sensor device node
     *
     * @param bus_path   – path to the I2C bus (e.g. "/dev/i2c-0")
     * @param dev_path   – desired device node path (e.g. "/dev/bmp180-0")
     * @param oss        – default oversampling setting for pressure measurements
     *
     * @return (1) [RTEMS_SUCCESSFUL, node_ptr] on success
     *         (2) [Error code, nullptr] otherwise
     */
    std::pair<rtems_status_code, bmp180_dev_t*> bmp180_register(
        const char* bus_path,
        const char* dev_path,
        bmp180_oss_t oss
    );
};

#endif //ES2025_BMP_INC_H
