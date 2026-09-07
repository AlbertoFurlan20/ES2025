//
// Created by Alberto Furlan on 01/04/26.
//

#ifndef ES2025_BMP180_REGS_H
#define ES2025_BMP180_REGS_H

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

/* ctrl_meas bit 5 (SCO): set by a conversion trigger, cleared by the sensor when
 * the result registers hold the new value. Polling it is what makes the waits
 * below timeouts rather than the measurement's timing contract. */
#define BMP180_CTRL_MEAS_SCO        0x20u

/* Soft-reset magic byte */
#define BMP180_SOFT_RESET_VALUE     0xB6u

/* Chip-ID expected value */
#define BMP180_CHIP_ID_EXPECTED     0x55u
// endregion

// Conversion times (ms).
//
// Two sets, both from BST-BMP180-DS000-09 Rev 2.5: typical and maximum.
//
// The driver sleeps the typical time, then polls the SCO bit until the sensor
// reports the conversion finished, giving up at the maximum. So the typicals set
// the common-case cycle time and the maxima are only a bound on a sensor that
// stopped answering — reaching one is an error (ETIMEDOUT), not a slow sample.
//
// The maxima carry one tick of padding over the datasheet figures (4.5 / 4.5 /
// 7.5 / 13.5 / 25.5 ms, Table 3). rtems_task_wake_after(n) blocks for between
// n-1 and n ticks, since the call lands somewhere inside the current tick, so an
// unpadded timeout could expire while the sensor was still within specification.
// Do not "correct" them back.
#define BMP180_CONV_TYP_TEMP_MS         3u
#define BMP180_CONV_TYP_PRESS_OSS0_MS   3u
#define BMP180_CONV_TYP_PRESS_OSS1_MS   5u
#define BMP180_CONV_TYP_PRESS_OSS2_MS   9u
#define BMP180_CONV_TYP_PRESS_OSS3_MS   17u

#define BMP180_CONV_TIME_TEMP_MS        6u
#define BMP180_CONV_TIME_PRESS_OSS0_MS  6u
#define BMP180_CONV_TIME_PRESS_OSS1_MS  9u
#define BMP180_CONV_TIME_PRESS_OSS2_MS  15u
#define BMP180_CONV_TIME_PRESS_OSS3_MS  27u

#endif //ES2025_BMP180_REGS_H
