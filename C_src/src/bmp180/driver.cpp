//
// Created by Alberto Furlan on 01/04/26.
//

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <unistd.h>

#ifdef BMP180_TEARDOWN_TEST
#include <fcntl.h>
#endif

#include "bmp180/driver.h"
#include "bmp180/ioctls.h"
#include "bmp180/regs.h"

// Internals. Declared here rather than in bmp180/driver.h: `static` gives them internal
// linkage, so a declaration in a shared header hands every *other* includer a
// symbol it can never link against — which is exactly what -Wunused-function
// was reporting, once per includer.
namespace bmp
{
    /**
     * @brief Holds the I2C bus lock for the enclosing scope.
     *
     * @details A BMP180 measurement is four bus transfers with conversion sleeps
     *          between them. `i2c_bus_do_transfer` locks per *transfer*, so
     *          without an outer lock two tasks would trigger conversions into
     *          each other's sleep windows and read each other's results —
     *          silently, since the sensor returns the previous conversion rather
     *          than an error.
     *
     *          The bus mutex is an `rtems_recursive_mutex`, so the per-transfer
     *          locks nest inside this one safely.
     *
     *          Also serialises `self->oss`, which selects both the control byte
     *          and the compensation shift and is otherwise read and written with
     *          no synchronisation.
     *
     *          RAII because every user has several early returns; a manual
     *          release would have to be repeated on each and would eventually be
     *          missed, deadlocking the bus.
     */
    class BusLock
    {
    public:
        // -DBMP180_NO_BUS_LOCK makes this a no-op. Exists so the two-reader
        // concurrency test can be run against both behaviours from one tree;
        // never define it in a real build.
        explicit BusLock(i2c_bus* bus) noexcept : bus_(bus)
        {
#ifndef BMP180_NO_BUS_LOCK
            i2c_bus_obtain(bus_);
#endif
        }

        ~BusLock()
        {
#ifndef BMP180_NO_BUS_LOCK
            i2c_bus_release(bus_);
#endif
        }

        BusLock(const BusLock&) = delete;
        BusLock& operator=(const BusLock&) = delete;

    private:
        i2c_bus* bus_;
    };

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

    /**
     * @brief Wait for a triggered conversion to finish.
     *
     * @details Sleeps the datasheet typical time, then polls the SCO bit in
     *          `ctrl_meas` once per tick until the sensor clears it. Replaces a
     *          fixed sleep of the datasheet *maximum*: that made every sample pay
     *          the worst case, and it also meant a sensor that never finished was
     *          read anyway, returning the previous conversion with no error.
     *
     * @param typical_ms  datasheet typical conversion time for this mode.
     * @param timeout_ms  datasheet maximum plus a tick; giving up here means the
     *                    sensor is not converting, not that it is slow.
     *
     * @return 0 once SCO is clear, ETIMEDOUT if it never cleared, or the bus
     *         error from the poll read.
     */
    static int bmp180_wait_conversion(const bmp180_dev_t* self,
                                      uint32_t typical_ms, uint32_t timeout_ms);

    /** @brief Read the uncompensated temperature (trigger, wait, read 0xF6-0xF7). */
    static int bmp180_read_ut(const bmp180_dev_t* self, int32_t* ut_out);

    /** @brief Read the uncompensated pressure (trigger, wait, read 0xF6-0xF8). */
    static int bmp180_read_up(const bmp180_dev_t* self, int32_t* up_out);

    /**
     * @brief Uncompensated temperature for this measurement, from the cache when
     *        it is still inside the configured re-conversion interval.
     *
     * @details Compensation needs a temperature, not a fresh one: the datasheet
     *          itself notes temperature may be sampled far more slowly than
     *          pressure. Reading it every cycle spent a second conversion on a
     *          quantity that moves in minutes.
     *
     * @see BMP180_DEFAULT_TEMP_INTERVAL_MS, BMP180_IOCTL_SET_TEMP_INTERVAL
     */
    static int bmp180_acquire_ut(bmp180_dev_t* self, int32_t* ut_out);

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

    // Sanity check the words as they came off the wire, before any of them is
    // stored. The datasheet guarantees no coefficient is 0x0000 or 0xFFFF, so
    // either value means the sensor answered with a floating or stuck bus
    // rather than with calibration data.
    //
    // Checked on `raw` rather than by walking the parsed struct through a
    // uint16_t*: that was an aliasing violation, and it silently assumed
    // bmp180_calib_t has no padding. Nothing enforced either property.
    for (uint16_t i = 0; i < BMP180_CALIB_DATA_LEN; i += 2)
    {
        const uint16_t word =
            static_cast<uint16_t>(static_cast<uint16_t>(raw[i]) << 8 | raw[i + 1]);
        if (word == 0x0000u || word == 0xFFFFu)
        {
            return EIO;
        }
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

    self->calib_loaded = true;
    return 0;
}


static int bmp::bmp180_wait_conversion(const bmp180_dev_t* self,
                                       const uint32_t typical_ms,
                                       const uint32_t timeout_ms)
{
    // Taken before the first sleep so the budget covers the whole wait, not just
    // the polling that follows it.
    const uint64_t deadline_ns = rtems_clock_get_uptime_nanoseconds() +
        static_cast<uint64_t>(timeout_ms) * 1000000u;

    rtems_task_wake_after(RTEMS_MILLISECONDS_TO_TICKS(typical_ms));

    for (;;)
    {
        uint8_t ctrl = 0;

        if (const int rc = bmp180_read_regs(&self->base, BMP180_REG_CTRL_MEAS,
                                            &ctrl, 1); rc != 0)
        {
            return rc;
        }

        if ((ctrl & BMP180_CTRL_MEAS_SCO) == 0)
        {
            return 0;
        }

        if (rtems_clock_get_uptime_nanoseconds() >= deadline_ns)
        {
            return ETIMEDOUT;
        }

        rtems_task_wake_after(1);
    }
}


static int bmp::bmp180_read_ut(const bmp180_dev_t* self, int32_t* ut_out)
{
    if (const int rc = bmp180_write_reg(&self->base,
                                        BMP180_REG_CTRL_MEAS,
                                        BMP180_MEAS_CTRL_TEMP); rc != 0)
    {
        return rc;
    }

    if (const int rc = bmp180_wait_conversion(self, BMP180_CONV_TYP_TEMP_MS,
                                              BMP180_CONV_TIME_TEMP_MS); rc != 0)
    {
        return rc;
    }

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

    static constexpr uint32_t typical_ms[4] = {
        BMP180_CONV_TYP_PRESS_OSS0_MS,
        BMP180_CONV_TYP_PRESS_OSS1_MS,
        BMP180_CONV_TYP_PRESS_OSS2_MS,
        BMP180_CONV_TYP_PRESS_OSS3_MS
    };

    static constexpr uint32_t timeout_ms[4] = {
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

    if (const int rc = bmp180_wait_conversion(self, typical_ms[oss_idx],
                                              timeout_ms[oss_idx]); rc != 0)
    {
        return rc;
    }

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


static int bmp::bmp180_acquire_ut(bmp180_dev_t* self, int32_t* ut_out)
{
    if (self->ut_valid && self->temp_interval_ms != 0u)
    {
        const uint64_t age_ns =
            rtems_clock_get_uptime_nanoseconds() - self->cached_ut_ns;

        if (age_ns < static_cast<uint64_t>(self->temp_interval_ms) * 1000000u)
        {
            *ut_out = self->cached_ut;
            return 0;
        }
    }

    if (const int rc = bmp180_read_ut(self, ut_out); rc != 0)
    {
        return rc;
    }

    self->cached_ut = *ut_out;
    self->cached_ut_ns = rtems_clock_get_uptime_nanoseconds();
    self->ut_valid = true;

    return 0;
}


int bmp::bmp180_do_measurement(bmp180_dev_t* self,
                                      bmp180_measurement_t* result)
{
    // Held across all four transfers and both conversion sleeps, so a
    // concurrent reader cannot interleave its own conversions with ours.
    const BusLock lock(self->base.bus);

    if (!self->calib_loaded)
    {
        if (const int rc = bmp180_load_calibration(self); rc != 0)
        {
            return rc;
        }
    }

    int32_t ut;

    if (const int rc = bmp180_acquire_ut(self, &ut); rc != 0)
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

            // Reported as-is rather than flattened to -EIO: a conversion that
            // never finished (ETIMEDOUT) and a bus that NAKed are different
            // faults, and the telemetry stream records whichever errno arrives.
            const int rc = bmp180_do_measurement(self, result);

            return rc <= 0 ? rc : -rc;
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

            // Same lock as the measurement path, so a mode change cannot land
            // between a conversion trigger and the matching read.
            const BusLock lock(self->base.bus);
            self->oss = new_oss;

            return 0;
        }

    case BMP180_IOCTL_GET_OSS:
        {
            if (arg == nullptr)
            {
                return -EINVAL;
            }

            const BusLock lock(self->base.bus);
            *static_cast<bmp180_oss_t*>(arg) = self->oss;

            return 0;
        }

    case BMP180_IOCTL_SET_TEMP_INTERVAL:
        {
            if (arg == nullptr)
            {
                return -EINVAL;
            }

            const BusLock lock(self->base.bus);

            self->temp_interval_ms = *static_cast<const uint32_t*>(arg);

            // The new interval judges the age of a reading taken under the old
            // one, so a shortened interval must not be satisfied by a value
            // already older than it.
            self->ut_valid = false;

            return 0;
        }

    case BMP180_IOCTL_GET_TEMP_INTERVAL:
        {
            if (arg == nullptr)
            {
                return -EINVAL;
            }

            const BusLock lock(self->base.bus);
            *static_cast<uint32_t*>(arg) = self->temp_interval_ms;

            return 0;
        }

    case BMP180_IOCTL_SOFT_RESET:
        {
            // Covers the write, the calibration invalidation and the settling
            // wait: a measurement must not start against a resetting sensor.
            const BusLock lock(self->base.bus);

            const int rc = bmp180_write_reg(base, BMP180_REG_SOFT_RESET,
                                            BMP180_SOFT_RESET_VALUE);
            if (rc != 0)
            {
                return -EIO;
            }

            self->calib_loaded = false;
            self->ut_valid = false;

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

    dev->temp_interval_ms = BMP180_DEFAULT_TEMP_INTERVAL_MS;
    dev->cached_ut = 0;
    dev->cached_ut_ns = 0;
    dev->ut_valid = false;

    // No memset of dev->calib here: i2c_dev_alloc_and_init allocates with
    // calloc, so the block is already zero. Clearing it again implied a
    // guarantee this code had not actually checked.

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


int bmp::bmp180_unregister(const char* dev_path)
{
    // unlink runs the node's destroy handler, which is bmp180_destroy ->
    // i2c_dev_destroy_and_free. Freeing the device first would unpublish nothing
    // and leave the node addressable.
    return unlink(dev_path) == 0 ? 0 : -errno;
}


#ifdef BMP180_TEARDOWN_TEST
int bmp::bmp180_teardown_test(const char* bus_path, const char* dev_path)
{
    bool ok = true;

    const auto [outcome, dev] =
        bmp180_register(bus_path, dev_path, BMP180_OSS_ULTRA_LOW_POWER);
    ok = ok && outcome == RTEMS_SUCCESSFUL && dev != nullptr;

    if (ok)
    {
        const int fd = open(dev_path, O_RDWR);
        ok = fd >= 0;
        if (fd >= 0)
        {
            close(fd);
        }
    }

    const int rc = ok ? bmp180_unregister(dev_path) : -1;
    ok = ok && rc == 0;

    // The node must be gone, not merely closed: an open that still succeeds
    // means the device was freed while its node stayed published.
    if (ok)
    {
        const int fd = open(dev_path, O_RDWR);
        ok = fd < 0;
        if (fd >= 0)
        {
            close(fd);
        }
    }

    printf("[SELFTEST] teardown: unlink(%s) -> %d  -> %s\n",
           dev_path, rc, ok ? "PASS" : "FAIL");

    return ok ? 0 : -1;
}
#endif


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
