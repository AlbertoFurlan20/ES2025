//
// Created by Alberto Furlan on 01/04/26.
//

#ifndef ES2025_BMP_REGS_H
#define ES2025_BMP_REGS_H

// region register addresses                                                  */

#define BMP180_REG_CHIP_ID          0xD0u
#define BMP180_REG_SOFT_RESET       0xE0u
#define BMP180_REG_CTRL_MEAS        0xF4u
#define BMP180_REG_OUT_MSB          0xF6u
#define BMP180_REG_OUT_LSB          0xF7u
#define BMP180_REG_OUT_XLSB         0xF8u

/* First calibration coefficient address */
#define BMP180_REG_CALIB_START      0xAAu

/* Number of calibration bytes: 11 coefficients × 2 bytes */
#define BMP180_CALIB_DATA_LEN       22u

/* ctrl_meas register bit fields */
#define BMP180_MEAS_CTRL_TEMP       0x2Eu  /* start temperature measurement */

/* Pressure ctrl values per OSS */
#define BMP180_MEAS_CTRL_PRESS_OSS0 0x34u
#define BMP180_MEAS_CTRL_PRESS_OSS1 0x74u
#define BMP180_MEAS_CTRL_PRESS_OSS2 0xB4u
#define BMP180_MEAS_CTRL_PRESS_OSS3 0xF4u

/* Soft-reset magic byte */
#define BMP180_SOFT_RESET_VALUE     0xB6u

/* Chip-ID expected value */
#define BMP180_CHIP_ID_EXPECTED     0x55u
// endregion

// Conversion times (ms)
#define BMP180_CONV_TIME_TEMP_MS        5u
#define BMP180_CONV_TIME_PRESS_OSS0_MS  5u
#define BMP180_CONV_TIME_PRESS_OSS1_MS  8u
#define BMP180_CONV_TIME_PRESS_OSS2_MS  14u
#define BMP180_CONV_TIME_PRESS_OSS3_MS  26u

#endif //ES2025_BMP_REGS_H
