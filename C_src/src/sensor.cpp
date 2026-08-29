#include <rtems.h>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "bmp.h"
#include "bmp180_ioctls.h"
#include "telemetry.h"

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

    for (int oss = 0; oss <= 3; ++oss)
    {
        constexpr int WARMUP = 3;
        constexpr int SWEEP_SAMPLES = 500;
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


#ifdef BMP180_CONCURRENCY_TEST
/**
 * @brief Second reader, used only to verify I1 (atomic measurement).
 *
 * @details Opens the same device node independently and reads back to back.
 *          With the bus lock in place the two tasks serialise and both see
 *          coherent pressure; without it they trigger conversions into each
 *          other's sleep windows and read each other's results, which shows up
 *          as gross outliers in the stream.
 *
 *          Marks its samples with oss=7 - outside the valid 0-3 range - so the
 *          host tooling can separate the two readers without a format change.
 */
rtems_task bmp180_second_reader_task(const rtems_task_argument ignored)
{
    (void)ignored;

    const int fd = open("/dev/bmp180-0", O_RDWR);
    if (fd < 0)
    {
        telem_push_error(telem_now_us(), errno);
        rtems_task_delete(RTEMS_SELF);

        return;
    }

    while (true)
    {
        bmp180_measurement_t m;

        if (ioctl(fd, BMP180_IOCTL_READ_MEASUREMENT, &m) != 0)
        {
            telem_push_error(telem_now_us(), errno);
            continue;
        }

        telem_push_sample(telem_now_us(), m.temperature_cdeg, m.pressure_pa, 7);
    }
}
#endif
