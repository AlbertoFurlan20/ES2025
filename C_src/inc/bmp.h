//
// Created by Alberto Furlan on 01/04/26.
//

#ifndef ES2025_BMP_INC_H
#define ES2025_BMP_INC_H

#include <cstring>
#include <bits/stl_pair.h>

#include <rtems.h>

#include "bmp_types.h"

namespace bmp
{
    /**
     * @brief Loads the calibration values in the device node
     *
     * @param self device node
     *
     * @return 0 if no errors, error code elsewhere
     */
    int bmp180_load_calibration(bmp180_dev_t* self);

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
    int bmp180_do_measurement(bmp180_dev_t* self,
                              bmp180_measurement_t* result);

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

    /**
     * @brief Validates bmp180_compensate against the datasheet worked example
     *        (BST-BMP180-DS000-09 section 3.5). Hardware-independent.
     *
     * @details Feeds the datasheet calibration constants + raw UT=27898,
     *          UP=23843 (oss=0); expects T=150 (15.0 degC) and P=69964 Pa.
     *
     * @return 0 on PASS, -1 on FAIL.
     */
    int bmp180_selftest();
};

#endif //ES2025_BMP_INC_H
