//
// Created by Alberto Furlan on 01/04/26.
//

#include <cstdint>
#include <cstdio>

#include "bmp.h"
#include "bmp180_ioctls.h"
#include "bmp_regs.h"

// Internals. Declared here rather than in bmp.h: `static` gives them internal
// linkage, so a declaration in a shared header hands every *other* includer a
// symbol it can never link against — which is exactly what -Wunused-function
// was reporting, once per includer.
namespace bmp
{
    /**
     * @brief True if @p oss names one of the four oversampling modes.
     *
     * @details Single definition of the range check, used by both entry points
     *          that accept an oversampling setting from a caller
     *          (@see bmp180_register and the SET_OSS ioctl).
     */
    constexpr bool bmp180_oss_is_valid(const bmp180_oss_t oss)
    {
        return static_cast<unsigned>(oss) <=
               static_cast<unsigned>(BMP180_OSS_ULTRA_HIGH_RES);
    }

    /** @brief Write one register. */
    static int bmp180_write_reg(const i2c_dev* dev, uint8_t reg, uint8_t value);

    /** @brief Read @p len registers starting at @p reg. */
    static int bmp180_read_regs(const i2c_dev* dev, uint8_t reg,
                                uint8_t* dst, uint16_t len);

    /** @brief Read the uncompensated temperature (trigger, wait, read 0xF6-0xF7). */
    static int bmp180_read_ut(const bmp180_dev_t* self, int32_t* ut_out);

    /** @brief Read the uncompensated pressure (trigger, wait, read 0xF6-0xF8). */
    static int bmp180_read_up(const bmp180_dev_t* self, int32_t* up_out);

    /**
     * @brief Bosch compensation algorithm, BST-BMP180-DS000-09 section 3.5.
     *
     * @return 0 on success, EIO if the calibration and raw temperature combine
     *         to a zero divisor (see the guard in the body).
     */
    static int bmp180_compensate(const bmp180_calib_t* cal,
                                 int32_t ut, int32_t up, uint8_t oss,
                                 int32_t* temp_cdeg_out, int32_t* pressure_pa_out);

    /** @brief ioctl handler installed on the device node. */
    static int bmp180_ioctl(i2c_dev* base, ioctl_command_t cmd, void* arg);

    /** @brief destroy handler installed on the device node. */
    static void bmp180_destroy(i2c_dev* base);
}

static int bmp::bmp180_write_reg(const i2c_dev* dev, const uint8_t reg, const uint8_t value)
{
    uint8_t buf[2] = {reg, value};

    i2c_msg msg = {
        .addr = dev->address,
        .flags = 0,
        .len = static_cast<uint16_t>(sizeof(buf)),
        .buf = buf
    };

    return i2c_bus_transfer(dev->bus, &msg, 1);
}


static int bmp::bmp180_read_regs(const i2c_dev* dev, uint8_t reg,
                                 uint8_t* dst, const uint16_t len)
{
    i2c_msg msgs[2] = {
        {
            .addr = dev->address,
            .flags = 0,
            .len = 1,
            .buf = &reg
        },
        {
            .addr = dev->address,
            .flags = I2C_M_RD,
            .len = len,
            .buf = dst
        }
    };

    return i2c_bus_transfer(dev->bus, msgs, 2);
}


int bmp::bmp180_load_calibration(bmp180_dev_t* self)
{
    uint8_t raw[BMP180_CALIB_DATA_LEN];

    const int rc = bmp180_read_regs(&self->base, BMP180_REG_CALIB_START,
                                    raw, BMP180_CALIB_DATA_LEN);
    if (rc != 0)
    {
        return rc;
    }

#define PARSE_S16(idx) \
    ((int16_t)(((uint16_t)raw[(idx)] << 8) | (uint16_t)raw[(idx) + 1]))
#define PARSE_U16(idx) \
    (((uint16_t)raw[(idx)] << 8) | (uint16_t)raw[(idx) + 1])

    self->calib.AC1 = PARSE_S16(0);
    self->calib.AC2 = PARSE_S16(2);
    self->calib.AC3 = PARSE_S16(4);
    self->calib.AC4 = PARSE_U16(6);
    self->calib.AC5 = PARSE_U16(8);
    self->calib.AC6 = PARSE_U16(10);
    self->calib.B1 = PARSE_S16(12);
    self->calib.B2 = PARSE_S16(14);
    self->calib.MB = PARSE_S16(16);
    self->calib.MC = PARSE_S16(18);
    self->calib.MD = PARSE_S16(20);

#undef PARSE_S16
#undef PARSE_U16

    const auto* words = reinterpret_cast<const uint16_t*>(&self->calib);
    for (size_t i = 0; i < sizeof(self->calib) / sizeof(uint16_t); ++i)
    {
        if (words[i] == 0x0000u || words[i] == 0xFFFFu)
        {
            return EIO;
        }
    }

    self->calib_loaded = true;
    return 0;
}


static int bmp::bmp180_read_ut(const bmp180_dev_t* self, int32_t* ut_out)
{
    if (const int rc = bmp180_write_reg(&self->base,
                                        BMP180_REG_CTRL_MEAS,
                                        BMP180_MEAS_CTRL_TEMP); rc != 0)
    {
        return rc;
    }

    rtems_task_wake_after(
        RTEMS_MILLISECONDS_TO_TICKS(BMP180_CONV_TIME_TEMP_MS)
    );

    uint8_t buf[2];

    if (const int rc = bmp180_read_regs(&self->base, BMP180_REG_OUT_MSB, buf, 2); rc != 0)
    {
        return rc;
    }

    *ut_out = (static_cast<int32_t>(buf[0]) << 8) | static_cast<int32_t>(buf[1]);
    return 0;
}


static int bmp::bmp180_read_up(const bmp180_dev_t* self, int32_t* up_out)
{
    static constexpr uint8_t ctrl_vals[4] = {
        BMP180_MEAS_CTRL_PRESS_OSS0,
        BMP180_MEAS_CTRL_PRESS_OSS1,
        BMP180_MEAS_CTRL_PRESS_OSS2,
        BMP180_MEAS_CTRL_PRESS_OSS3
    };

    static constexpr uint32_t wait_ms[4] = {
        BMP180_CONV_TIME_PRESS_OSS0_MS,
        BMP180_CONV_TIME_PRESS_OSS1_MS,
        BMP180_CONV_TIME_PRESS_OSS2_MS,
        BMP180_CONV_TIME_PRESS_OSS3_MS
    };

    const auto oss_idx = static_cast<uint8_t>(self->oss);

    if (const int rc = bmp180_write_reg(&self->base,
                                        BMP180_REG_CTRL_MEAS,
                                        ctrl_vals[oss_idx]); rc != 0)
    {
        return rc;
    }

    rtems_task_wake_after(
        RTEMS_MILLISECONDS_TO_TICKS(wait_ms[oss_idx])
    );

    uint8_t buf[3];

    if (const int rc = bmp180_read_regs(&self->base, BMP180_REG_OUT_MSB, buf, 3); rc != 0)
    {
        return rc;
    }

    *up_out = ((static_cast<int32_t>(buf[0]) << 16) |
        (static_cast<int32_t>(buf[1]) << 8) |
        static_cast<int32_t>(buf[2])) >> (8 - oss_idx);

    return 0;
}


static int bmp::bmp180_compensate(
    const bmp180_calib_t* cal,
    const int32_t ut,
    const int32_t up,
    const uint8_t oss,
    int32_t* temp_cdeg_out,
    int32_t* pressure_pa_out)
{
    int32_t X1 = ((ut - static_cast<int32_t>(cal->AC6)) * static_cast<int32_t>(cal->AC5)) >> 15;

    // The datasheet formula divides by (X1 + MD) with no guard. The calibration
    // sanity check rejects all-zero and all-ones coefficients but cannot rule
    // out a runtime X1 that happens to cancel MD - reachable with corrupt
    // calibration or a wild UT. On Cortex-M4 the outcome depends on DIV_0_TRP in
    // SCB->CCR: either a silent zero or a UsageFault. Neither is acceptable in
    // the measurement path, so refuse the sample instead and let the caller
    // report it.
    const int32_t divisor = X1 + static_cast<int32_t>(cal->MD);
    if (divisor == 0)
    {
        return EIO;
    }

    int32_t X2 = (static_cast<int32_t>(cal->MC) << 11) / divisor;

    const int32_t B5 = X1 + X2;
    const int32_t T = (B5 + 8) >> 4;

    *temp_cdeg_out = T;

    const int32_t B6 = B5 - 4000;

    X1 = (static_cast<int32_t>(cal->B2) * ((B6 * B6) >> 12)) >> 11;
    X2 = (static_cast<int32_t>(cal->AC2) * B6) >> 11;

    int32_t X3 = X1 + X2;
    const int32_t B3 = (((static_cast<int32_t>(cal->AC1) * 4 + X3) << oss) + 2) / 4;

    X1 = (static_cast<int32_t>(cal->AC3) * B6) >> 13;
    X2 = (static_cast<int32_t>(cal->B1) * ((B6 * B6) >> 12)) >> 16;
    X3 = ((X1 + X2) + 2) >> 2;

    const uint32_t B4 = (static_cast<uint32_t>(cal->AC4) * static_cast<uint32_t>(X3 + 32768)) >> 15;
    const uint32_t B7 = (static_cast<uint32_t>(up) - static_cast<uint32_t>(B3)) * (50000u >> oss);

    int32_t p;

    if (B7 < 0x80000000u)
    {
        p = static_cast<int32_t>((B7 * 2u) / B4);
    }
    else
    {
        p = static_cast<int32_t>((B7 / B4) * 2u);
    }

    X1 = (p >> 8) * (p >> 8);
    X1 = (X1 * 3038) >> 16;
    X2 = (-7357 * p) >> 16;

    p = p + ((X1 + X2 + 3791) >> 4);

    *pressure_pa_out = p;
    return 0;
}


int bmp::bmp180_do_measurement(bmp180_dev_t* self,
                                      bmp180_measurement_t* result)
{
    if (!self->calib_loaded)
    {
        if (const int rc = bmp180_load_calibration(self); rc != 0)
        {
            return rc;
        }
    }

    int32_t ut;

    if (const int rc = bmp180_read_ut(self, &ut); rc != 0)
    {
        return rc;
    }

    int32_t up;
    if (const int rc = bmp180_read_up(self, &up); rc != 0)
    {
        return rc;
    }

    return bmp180_compensate(&self->calib, ut, up,
                             static_cast<uint8_t>(self->oss),
                             &result->temperature_cdeg,
                             &result->pressure_pa);
}



static int bmp::bmp180_ioctl(i2c_dev* base, const ioctl_command_t cmd, void* arg)
{
    const auto self = reinterpret_cast<bmp180_dev_t*>(base);

    switch (cmd)
    {
    case BMP180_IOCTL_READ_MEASUREMENT:
        {
            if (arg == nullptr)
            {
                return -EINVAL;
            }

            const auto result = static_cast<bmp180_measurement_t*>(arg);

            return bmp180_do_measurement(self, result) != 0 ? -EIO : 0;
        }

    case BMP180_IOCTL_SET_OSS:
        {
            if (arg == nullptr)
            {
                return -EINVAL;
            }

            const bmp180_oss_t new_oss = *static_cast<const bmp180_oss_t*>(arg);

            if (!bmp180_oss_is_valid(new_oss))
            {
                return -EINVAL;
            }

            self->oss = new_oss;

            return 0;
        }

    case BMP180_IOCTL_GET_OSS:
        {
            if (arg == nullptr)
            {
                return -EINVAL;
            }

            *static_cast<bmp180_oss_t*>(arg) = self->oss;

            return 0;
        }

    case BMP180_IOCTL_SOFT_RESET:
        {
            const int rc = bmp180_write_reg(base, BMP180_REG_SOFT_RESET,
                                            BMP180_SOFT_RESET_VALUE);
            if (rc != 0)
            {
                return -EIO;
            }

            self->calib_loaded = false;

            rtems_task_wake_after(RTEMS_MILLISECONDS_TO_TICKS(12));
            return 0;
        }

    default:
        return -ENOTTY;
    }
}


static void bmp::bmp180_destroy(i2c_dev* base)
{
    i2c_dev_destroy_and_free(base);
}


std::pair<rtems_status_code, bmp180_dev_t*> bmp::bmp180_register(
    const char* bus_path,
    const char* dev_path,
    const bmp180_oss_t oss)
{
    // bmp180_oss_t is a plain unscoped enum, so an out-of-range value is trivial
    // to pass by accident. bmp180_read_up indexes two 4-element tables with it
    // and would otherwise read past both and write a garbage control byte to the
    // sensor. Checked before allocating, so a bad argument costs nothing.
    if (!bmp180_oss_is_valid(oss))
    {
        return std::make_pair(RTEMS_INVALID_NUMBER, nullptr);
    }

    const auto dev = reinterpret_cast<bmp180_dev_t*>(i2c_dev_alloc_and_init(
        sizeof(bmp180_dev_t),
        bus_path,
        BMP180_I2C_ADDR
    ));

    if (dev == nullptr)
    {
        return std::make_pair(RTEMS_NO_MEMORY, nullptr);
    }

    dev->base.ioctl = bmp180_ioctl;
    dev->base.destroy = bmp180_destroy;

    dev->oss = oss;
    dev->calib_loaded = false;

    memset(&dev->calib, 0, sizeof(dev->calib));

    uint8_t chip_id = 0;

    if (const int rc = bmp180_read_regs(&dev->base, BMP180_REG_CHIP_ID, &chip_id, 1); rc != 0 || chip_id !=
        BMP180_CHIP_ID_EXPECTED)
    {
        i2c_dev_destroy_and_free(&dev->base);
        return std::make_pair(RTEMS_IO_ERROR, nullptr);
    }

    // i2c_dev_register returns 0 on success; on failure it already calls
    // dev->destroy() internally, so on error we must NOT touch dev again.
    if (i2c_dev_register(&dev->base, dev_path) != 0)
    {
        return std::make_pair(RTEMS_INTERNAL_ERROR, nullptr);
    }

    return std::make_pair(RTEMS_SUCCESSFUL, dev);
}


int bmp::bmp180_selftest()
{
    // Datasheet BST-BMP180-DS000-09, section 3.5 worked example.
    constexpr bmp180_calib_t cal = {
        408,    // AC1
        -72,    // AC2
        -14383, // AC3
        32741,  // AC4
        32757,  // AC5
        23153,  // AC6
        6190,   // B1
        4,      // B2
        -32768, // MB
        -8711,  // MC
        2868    // MD
    };

    int32_t temp_cdeg = 0;
    int32_t pressure_pa = 0;
    const int rc = bmp180_compensate(&cal, 27898, 23843, 0,
                                     &temp_cdeg, &pressure_pa);

    const bool ok = (rc == 0) && (temp_cdeg == 150) && (pressure_pa == 69964);
    printf("[SELFTEST] compensate: T=%ld (exp 150)  P=%ld (exp 69964)  -> %s\n",
           static_cast<long>(temp_cdeg), static_cast<long>(pressure_pa),
           ok ? "PASS" : "FAIL");

    return ok ? 0 : -1;
}
