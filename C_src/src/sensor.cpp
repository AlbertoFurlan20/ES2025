#include <rtems.h>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <stdexcept>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "bmp.h"
#include "bmp180_ioctls.h"
#include "telemetry.h"

/**
 * @brief Main bmp180 task that follows performs continuous sensor reading
 *
 * @details Steps:
 *          1. Opens the device node for the sensor
 *          2. Configures the sensor (oversampling ratio)
 *          3. Trigger a read cc every @see rtems_clock_get_ticks_per_second() seconds
 *
 * @param ignored input param to task is ignored
 *
 * @warning The task is killed if open fails
 * @warning The task is NOT killed if the sensor config fails (should default to mode 0)
 * @warning The task is killed if 3 consecutive reads fail (e.g. sensor disconnected)
 */
rtems_task bmp180_task(const rtems_task_argument ignored)
{
    (void)ignored;

    const int fd = open("/dev/bmp180-0", O_RDWR);
    if (fd < 0)
    {
        // TODO - check if stderr is routed to USART 3
        // - eventually go for a basic printf a let go of the error msg
        perror("open /dev/bmp180-0");
        rtems_task_delete(RTEMS_SELF);

        return;
    }

    bmp180_oss_t oss = BMP180_OSS_HIGH_RESOLUTION;
    if (ioctl(fd, BMP180_IOCTL_SET_OSS, &oss) != 0)
    {
        // TODO - check if stderr is routed to USART 3
        // - eventually go for a basic printf a let go of the error msg
        perror("BMP180_IOCTL_SET_OSS");
    }

    const rtems_interval ticks_per_sec = rtems_clock_get_ticks_per_second();

    unsigned int fail_threshold = 0;

    // ReSharper disable once CppDFAEndlessLoop
    while (true)
    {
        bmp180_measurement_t m;

        if (ioctl(fd, BMP180_IOCTL_READ_MEASUREMENT, &m) != 0)
        {
            // TODO - check if stderr is routed to USART 3
            // - eventually go for a basic printf a let go of the error msg
            perror("BMP180_IOCTL_READ_MEASUREMENT");
            fail_threshold++;
        }
        else
        {
            fail_threshold = 0; // reset the threshold so to capture consecutive failures only
            printf("Temperature: %d.%d degC   Pressure: %ld Pa\n",
                   static_cast<int>(m.temperature_cdeg / 10),
                   static_cast<int>(m.temperature_cdeg % 10),
                   static_cast<long>(m.pressure_pa));
        }

        if (fail_threshold > 3)
        {
            printf("Too many consecutive failures [%d], killing task...\n", fail_threshold);

            rtems_task_delete(RTEMS_SELF);
        }

        rtems_task_wake_after(ticks_per_sec);
    }
}


/**
 * @brief Manual bmp180 task that performs manual sensor reading
 *
 * @details Steps:
 *          1. Register the sensor
 *          2. Read calibration sensor settings (calibration reads is done on registration)
 *          3. [IF config is not loaded] Config the sensor via manual bits setting
 *          4. Loop over true and continue measuring
 *             4.1. Read uncompensated sensor temperature
 *             4.2. Read uncompensated sensor pressure
 *             4.3. Compensate + compute "true" values
 *             4.4. Jump to step [4.1]
 *
 * @param ignored input param to task is ignored
 *
 * @todo check manual @see i2c_dev destruction doesn't fuck things up
 * @todo also check if there are other ways to perform @see i2c_dev destruction
 */
rtems_task bmp180_task_manual(const rtems_task_argument ignored)
{
    using namespace bmp;

    // ReSharper disable once CppTooWideScope
    const auto bus_path = "/dev/bmp180-0";
    // ReSharper disable once CppTooWideScope
    const auto dev_path = "/dev/bmp180-0";

    bmp180_dev_t* dev_ptr = nullptr;

    try
    {
        const auto [reg_outcome, dev] = bmp180_register(bus_path, dev_path, BMP180_OSS_HIGH_RESOLUTION);

        dev_ptr = dev;

        if (reg_outcome != RTEMS_SUCCESSFUL or dev == nullptr)
        {
            printf("Failed sensor registration, killing task...\n");
            rtems_task_delete(RTEMS_SELF);

            return;
        }

#ifndef DEBUG
        printf("Sensor registered successfully, starting manual read loop...\n");
        const auto cal_is_loaded = dev->calib_loaded;
        printf("Calibration is loaded: %d", cal_is_loaded);
        const auto [AC1, AC2, AC3, AC4, AC5, AC6, B1, B2, MB, MC, MD] = dev->calib;
        printf("\nCalibration settings:\n");
        printf(" - AC1: %d\n", AC1);
        printf(" - AC2: %d\n", AC2);
        printf(" - AC3: %d\n", AC3);
        printf(" - AC4: %d\n", AC4);
        printf(" - AC5: %d\n", AC5);
        printf(" - AC6: %d\n", AC6);
        printf(" - B1: %d\n", B1);
        printf(" - B2: %d\n", B2);
        printf(" - MB: %d\n", MB);
        printf(" - MC: %d\n", MC);
        printf(" - MD: %d\n", MD);
        printf("\nOversampling settings:");
        printf(" - Mode: %d\n", dev->oss);
#endif

        if (not dev->calib_loaded)
        {
            try
            {
                if (bmp180_load_calibration(dev) != RTEMS_SUCCESSFUL)
                {
                    printf("Failed to load calibration\n");
                    dev_ptr->base.destroy(&dev_ptr->base);
                    rtems_task_delete(RTEMS_SELF);

                    return;
                }

                printf("Sensor re-calibrated successfully, starting manual read loop...\n");
            }
            catch (std::exception& e)
            {
                printf("Sensor re-calibration failed: %s\n", e.what());
                rtems_task_delete(RTEMS_SELF);

                return;
            }
        }
    }
    catch (const std::exception& e)
    {
        printf("Exception on sensor registration, killing task...\n%s", e.what());
        dev_ptr->base.destroy(&dev_ptr->base);
        rtems_task_delete(RTEMS_SELF);

        return;
    }

    unsigned int fail_threshold = 0;

    while (true)
    {
        bmp180_measurement_t m;

        try
        {
            if (const auto return_code = bmp180_do_measurement(dev_ptr, &m); return_code != 0)
            {
                throw std::exception();
            }

            fail_threshold = 0;

            printf("Sensor measurements\n - Pressure: %d\n - Temperature: %d\n", m.pressure_pa, m.temperature_cdeg);
        }
        catch (std::exception& e)
        {
            fail_threshold++;
            printf("Exception on sensor measurement [%d]\n", fail_threshold);
#ifndef DEBUG
            printf("Logs: %s\n", e.what());
#endif
        }

        if (fail_threshold > 3)
        {
            printf("Too many consecutive failures [%d], killing task...\n", fail_threshold);
            dev_ptr->base.destroy(&dev_ptr->base);
            rtems_task_delete(RTEMS_SELF);

            return;
        }
    }
}


/**
 * @brief Baseline telemetry task: sweeps the four oversampling modes, then
 *        settles into a continuous run at high resolution.
 *
 * @details Emits one raw record per measurement. No statistics are computed on
 *          the device - mean, stddev, peak-to-peak and achieved rate are all
 *          derived host-side from the timestamps and values in the stream.
 *
 *          Sampling is back-to-back with no inter-sample sleep, so consecutive
 *          timestamps bound the true acquisition cycle time.
 *
 * @note The first inter-sample delta of each OSS block spans the mode change and
 *       the warm-up, so it is not a cycle time. metrics.py discards it.
 *
 * @param ignored input param to task is ignored
 */
rtems_task bmp180_telemetry_task(const rtems_task_argument ignored)
{
    (void)ignored;

    const int fd = open("/dev/bmp180-0", O_RDWR);
    if (fd < 0)
    {
        perror("open /dev/bmp180-0");
        rtems_task_delete(RTEMS_SELF);

        return;
    }

    constexpr int SWEEP_SAMPLES = 500; // per oversampling mode
    constexpr int WARMUP = 3;          // discarded settling samples

    for (int oss = 0; oss <= 3; ++oss)
    {
        bmp180_oss_t mode = static_cast<bmp180_oss_t>(oss);
        if (ioctl(fd, BMP180_IOCTL_SET_OSS, &mode) != 0)
        {
            telem_push_error(telem_now_us(), errno);
            continue;
        }

        bmp180_measurement_t m;
        for (int i = 0; i < WARMUP; ++i)
        {
            ioctl(fd, BMP180_IOCTL_READ_MEASUREMENT, &m);
        }

        for (int i = 0; i < SWEEP_SAMPLES; ++i)
        {
            if (ioctl(fd, BMP180_IOCTL_READ_MEASUREMENT, &m) != 0)
            {
                telem_push_error(telem_now_us(), errno);
                continue;
            }

            telem_push_sample(telem_now_us(), m.temperature_cdeg, m.pressure_pa,
                              static_cast<uint8_t>(oss));
        }
    }

    // Continuous run for drift, jitter and self-heating analysis.
    bmp180_oss_t hi = BMP180_OSS_HIGH_RESOLUTION;
    if (ioctl(fd, BMP180_IOCTL_SET_OSS, &hi) != 0)
    {
        telem_push_error(telem_now_us(), errno);
    }

    while (true)
    {
        bmp180_measurement_t m;

        if (ioctl(fd, BMP180_IOCTL_READ_MEASUREMENT, &m) != 0)
        {
            telem_push_error(telem_now_us(), errno);
            continue;
        }

        telem_push_sample(telem_now_us(), m.temperature_cdeg, m.pressure_pa,
                          static_cast<uint8_t>(BMP180_OSS_HIGH_RESOLUTION));
    }
}
