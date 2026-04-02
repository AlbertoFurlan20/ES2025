//
// Created by Alberto Furlan on 01/04/26.
//

#ifndef ES2025_BMP_CONFIGS_H
#define ES2025_BMP_CONFIGS_H

#include <dev/i2c/i2c.h>

/**
 * @brief Oversampling settings
 */
typedef enum {
    BMP180_OSS_ULTRA_LOW_POWER  = 0,
    BMP180_OSS_STANDARD         = 1,
    BMP180_OSS_HIGH_RESOLUTION  = 2,
    BMP180_OSS_ULTRA_HIGH_RES   = 3
} bmp180_oss_t;


/**
 * @brief These are the calibration coeffs in the docs
 *
 * @ref Cheatsheet - 11 words of 16 bits
 */
typedef struct {
    int16_t  AC1;
    int16_t  AC2;
    int16_t  AC3;
    uint16_t AC4;
    uint16_t AC5;
    uint16_t AC6;
    int16_t  B1;
    int16_t  B2;
    int16_t  MB;
    int16_t  MC;
    int16_t  MD;
} bmp180_calib_t;

/**
 * @brief This struct represents the measurements of the sensor.
 *
 * @param temperature_cdeg  Temperature [steps of 0.1°C]
 * @param pressure_pa       Pressure    [steps of 1Pa (= 0.01hPa = 0.01mbar)]
 *
 * @note  Docs highlight this sensor registers temperature as well
 */
typedef struct {
    int32_t  temperature_cdeg;
    int32_t  pressure_pa;
} bmp180_measurement_t;


typedef struct {
    i2c_dev       base;
    bmp180_calib_t calib;
    bmp180_oss_t   oss;
    bool           calib_loaded;
} bmp180_dev_t;

#endif //ES2025_BMP_CONFIGS_H
