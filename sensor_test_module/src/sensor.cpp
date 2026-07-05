#include <rtems.h>
#include <cstdio>
#include <cstdint>
#include <stdexcept>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "bmp.h"
#include "bmp180_ioctls.h"

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
 * @brief Integer square root (no FPU/float-printf dependency).
 */
static uint32_t isqrt32(uint64_t v)
{
    uint64_t rem = v;
    uint64_t root = 0;
    uint64_t bit = 1ULL << 62;

    while (bit > rem)
    {
        bit >>= 2;
    }
    while (bit != 0)
    {
        if (rem >= root + bit)
        {
            rem -= root + bit;
            root = (root >> 1) + bit;
        }
        else
        {
            root >>= 1;
        }
        bit >>= 2;
    }
    return static_cast<uint32_t>(root);
}


/**
 * @brief OSS sweep + noise logger. For each oversampling mode it takes a burst
 *        of back-to-back samples and reports mean / stddev / peak-to-peak
 *        pressure, mean temperature and wall-clock ms per sample. After the
 *        sweep it falls into a normal 1 Hz read loop at OSS=HIGH_RESOLUTION.
 *
 * @details Sampling is back-to-back (no inter-sample sleep) so the timing
 *          column reflects the sensor's conversion time, and the noise figures
 *          are not aliased by the 1 s scheduler tick.
 */
rtems_task bmp180_oss_sweep_task(const rtems_task_argument ignored)
{
    (void)ignored;

    const int fd = open("/dev/bmp180-0", O_RDWR);
    if (fd < 0)
    {
        perror("open /dev/bmp180-0");
        rtems_task_delete(RTEMS_SELF);

        return;
    }

    constexpr int N = 32;            // samples per OSS mode
    constexpr int WARMUP = 3;        // discarded settling samples
    const rtems_interval tps = rtems_clock_get_ticks_per_second();

    printf("\n=== BMP180 OSS sweep + noise (%d samples/mode, back-to-back) ===\n", N);
    printf("OSS  mean_Pa  std_Pa  p2p_Pa  mean_degC  ms/smp\n");

    for (int oss = 0; oss <= 3; ++oss)
    {
        bmp180_oss_t mode = static_cast<bmp180_oss_t>(oss);
        if (ioctl(fd, BMP180_IOCTL_SET_OSS, &mode) != 0)
        {
            perror("BMP180_IOCTL_SET_OSS");
            continue;
        }

        bmp180_measurement_t m;
        for (int i = 0; i < WARMUP; ++i)
        {
            ioctl(fd, BMP180_IOCTL_READ_MEASUREMENT, &m);
        }

        int32_t samples[N];
        int64_t sum_p = 0;
        int64_t sum_t = 0;
        int32_t pmin = INT32_MAX;
        int32_t pmax = INT32_MIN;
        int got = 0;

        const rtems_interval t0 = rtems_clock_get_ticks_since_boot();
        for (int i = 0; i < N; ++i)
        {
            if (ioctl(fd, BMP180_IOCTL_READ_MEASUREMENT, &m) != 0)
            {
                perror("BMP180_IOCTL_READ_MEASUREMENT");
                continue;
            }
            samples[got] = m.pressure_pa;
            sum_p += m.pressure_pa;
            sum_t += m.temperature_cdeg;
            if (m.pressure_pa < pmin) pmin = m.pressure_pa;
            if (m.pressure_pa > pmax) pmax = m.pressure_pa;
            ++got;
        }
        const rtems_interval t1 = rtems_clock_get_ticks_since_boot();

        if (got == 0)
        {
            printf("%3d  (no samples)\n", oss);
            continue;
        }

        const int32_t mean_p = static_cast<int32_t>(sum_p / got);
        int64_t var_acc = 0;
        for (int i = 0; i < got; ++i)
        {
            const int64_t d = samples[i] - mean_p;
            var_acc += d * d;
        }
        const uint32_t std_p = isqrt32(static_cast<uint64_t>(var_acc / got));
        const int32_t mean_t = static_cast<int32_t>(sum_t / got); // 0.1 degC
        const uint32_t ms_per =
            static_cast<uint32_t>((static_cast<uint64_t>(t1 - t0) * 1000u / tps) / got);

        printf("%3d  %7ld  %6lu  %6ld  %4ld.%ld     %4lu\n",
               oss,
               static_cast<long>(mean_p),
               static_cast<unsigned long>(std_p),
               static_cast<long>(pmax - pmin),
               static_cast<long>(mean_t / 10), static_cast<long>(mean_t % 10),
               static_cast<unsigned long>(ms_per));
    }

    printf("=== sweep done; resuming 1 Hz read at OSS=HIGH_RESOLUTION ===\n\n");

    bmp180_oss_t hi = BMP180_OSS_HIGH_RESOLUTION;
    ioctl(fd, BMP180_IOCTL_SET_OSS, &hi);

    while (true)
    {
        bmp180_measurement_t m;
        if (ioctl(fd, BMP180_IOCTL_READ_MEASUREMENT, &m) == 0)
        {
            printf("Temperature: %ld.%ld degC   Pressure: %ld Pa\n",
                   static_cast<long>(m.temperature_cdeg / 10),
                   static_cast<long>(m.temperature_cdeg % 10),
                   static_cast<long>(m.pressure_pa));
        }
        else
        {
            perror("BMP180_IOCTL_READ_MEASUREMENT");
        }

        rtems_task_wake_after(tps);
    }
}
