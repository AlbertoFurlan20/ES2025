#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "bmp_app.h"
#include "bmp180_ioctls.h"
#include "telemetry.h"

namespace
{
    BmpAppSnapshot g_snapshot;

    // errno of the last failed cycle, 0 if the last cycle worked. Read by any
    // task, written only by the sampler.
    std::atomic<int> g_last_error{0};

    // Requested parameter values plus a pending bit each. One bit per
    // parameter, not one shared bit, so setting the mode does not cause the
    // temperature interval to be re-issued as a second ioctl next cycle.
    //
    // Stored as uint32_t rather than the enum so the atomic is a plain 32-bit
    // word, which is lock-free on Cortex-M4.
    std::atomic<uint32_t> g_req_oss{
        static_cast<uint32_t>(BMP180_OSS_HIGH_RESOLUTION)};
    std::atomic<bool>     g_oss_pending{false};

    std::atomic<uint32_t> g_req_temp_ms{BMP180_DEFAULT_TEMP_INTERVAL_MS};
    std::atomic<bool>     g_temp_pending{false};

    std::atomic<bool>     g_reset_pending{false};

    /**
     * @brief Apply whatever control changes are pending, in the order
     *        reset, mode, temperature interval.
     *
     * @details Reset goes first because it returns the device to its power-on
     *          defaults, so a mode set in the same cycle must land after it or
     *          it would be thrown away.
     *
     * @param fd         the sampler's device descriptor
     * @param active_oss the mode currently in force
     *
     * @return the mode in force after this call, which is what the next
     *         published sample was actually taken at.
     */
    uint8_t apply_pending(const int fd, const uint8_t active_oss)
    {
        uint8_t now_active = active_oss;

        if (g_reset_pending.exchange(false, std::memory_order_acq_rel))
        {
            if (ioctl(fd, BMP180_IOCTL_SOFT_RESET) != 0)
            {
                g_last_error.store(errno, std::memory_order_relaxed);
            }
        }

        if (g_oss_pending.exchange(false, std::memory_order_acq_rel))
        {
            bmp180_oss_t mode = static_cast<bmp180_oss_t>(
                g_req_oss.load(std::memory_order_relaxed));

            if (ioctl(fd, BMP180_IOCTL_SET_OSS, &mode) == 0)
            {
                now_active = static_cast<uint8_t>(mode);
            }
            else
            {
                g_last_error.store(errno, std::memory_order_relaxed);
            }
        }

        if (g_temp_pending.exchange(false, std::memory_order_acq_rel))
        {
            uint32_t ms = g_req_temp_ms.load(std::memory_order_relaxed);

            if (ioctl(fd, BMP180_IOCTL_SET_TEMP_INTERVAL, &ms) != 0)
            {
                g_last_error.store(errno, std::memory_order_relaxed);
            }
        }

        return now_active;
    }
}

bool bmp_app_read(bmp_app_sample_t* out)
{
    return out != nullptr && g_snapshot.read(*out);
}

int bmp_app_last_error()
{
    return g_last_error.load(std::memory_order_relaxed);
}

int bmp_app_set_oss(const bmp180_oss_t oss)
{
    // Same range check the driver applies, done here so a bad value never
    // reaches a cycle and never costs the caller a wait to find out.
    if (static_cast<unsigned>(oss) > static_cast<unsigned>(BMP180_OSS_ULTRA_HIGH_RES))
    {
        return -EINVAL;
    }

    g_req_oss.store(static_cast<uint32_t>(oss), std::memory_order_relaxed);
    g_oss_pending.store(true, std::memory_order_release);
    return 0;
}

bmp180_oss_t bmp_app_get_oss()
{
    return static_cast<bmp180_oss_t>(g_req_oss.load(std::memory_order_relaxed));
}

int bmp_app_set_temp_interval_ms(const uint32_t ms)
{
    g_req_temp_ms.store(ms, std::memory_order_relaxed);
    g_temp_pending.store(true, std::memory_order_release);
    return 0;
}

uint32_t bmp_app_get_temp_interval_ms()
{
    return g_req_temp_ms.load(std::memory_order_relaxed);
}

void bmp_app_request_soft_reset()
{
    g_reset_pending.store(true, std::memory_order_release);
}

rtems_task bmp_app_sampler_task(const rtems_task_argument ignored)
{
    (void)ignored;

    const int fd = open("/dev/bmp180-0", O_RDWR);
    if (fd < 0)
    {
        // Recorded in both places: last_error for any task that asks, and the
        // telemetry stream because a capture keeps nothing else.
        g_last_error.store(errno, std::memory_order_relaxed);
        telem_push_error(telem_now_us(), errno);
        rtems_task_delete(RTEMS_SELF);

        return;
    }

    // Adopt the device's current settings rather than imposing this module's
    // defaults, so bmp_app_get_oss() is truthful from the first call and no
    // ioctl is issued for a change nobody asked for.
    bmp180_oss_t dev_oss = BMP180_OSS_HIGH_RESOLUTION;
    if (ioctl(fd, BMP180_IOCTL_GET_OSS, &dev_oss) == 0)
    {
        g_req_oss.store(static_cast<uint32_t>(dev_oss), std::memory_order_relaxed);
    }

    uint32_t dev_temp_ms = BMP180_DEFAULT_TEMP_INTERVAL_MS;
    if (ioctl(fd, BMP180_IOCTL_GET_TEMP_INTERVAL, &dev_temp_ms) == 0)
    {
        g_req_temp_ms.store(dev_temp_ms, std::memory_order_relaxed);
    }

    uint8_t active_oss = static_cast<uint8_t>(dev_oss);

    while (true)
    {
        active_oss = apply_pending(fd, active_oss);

        bmp180_measurement_t m;
        if (ioctl(fd, BMP180_IOCTL_READ_MEASUREMENT, &m) != 0)
        {
            // Do not publish. The snapshot keeps the last good values and the
            // last good timestamp, so a reader sees the true age of the data
            // instead of a fresh timestamp on a stale reading.
            g_last_error.store(errno, std::memory_order_relaxed);
            continue;
        }

        g_last_error.store(0, std::memory_order_relaxed);
        g_snapshot.publish(telem_now_us(), m.temperature_cdeg, m.pressure_pa,
                           active_oss);
    }
}

#ifdef BMP_APP_TEST
rtems_task bmp_app_demo_reader_task(const rtems_task_argument ignored)
{
    (void)ignored;

    // Reproduces bmp180_telemetry_task's profile exactly - 3 discarded warm-up
    // samples then 500 measured ones per mode, then a continuous run at high
    // resolution - but drives it through the application layer instead of an
    // fd. That is what makes a capture under this flag directly comparable
    // against the acceptance captures in TESTING.md sections 4 and 8.
    constexpr uint32_t SWEEP_SAMPLES = 500;
    constexpr uint32_t WARMUP = 3;

    uint32_t last_seq = 0;
    uint8_t  mode = 0;
    uint32_t pushed = 0;
    uint32_t warmup_left = WARMUP;
    bool     sweeping = true;

    // Start at OSS0 rather than wherever the device happens to be.
    bmp_app_set_oss(BMP180_OSS_ULTRA_LOW_POWER);

    while (true)
    {
        bmp_app_sample_t s{};

        if (bmp_app_read(&s) && s.seq != last_seq)
        {
            last_seq = s.seq;

            // A mode change applies at the top of the sampler's next cycle, so
            // samples still carrying the previous mode are in flight when this
            // reader asks for a new one. Dropping them is what keeps each block
            // one mode wide; it is also why the sample carries the mode it was
            // taken at rather than the mode currently requested.
            if (s.oss == mode)
            {
                if (warmup_left > 0)
                {
                    --warmup_left;
                }
                else
                {
                    telem_push_sample(s.t_us, s.temperature_cdeg, s.pressure_pa,
                                      s.oss);
                    ++pushed;

                    if (sweeping && pushed >= SWEEP_SAMPLES)
                    {
                        pushed = 0;
                        warmup_left = WARMUP;

                        if (mode < 3)
                        {
                            ++mode;
                        }
                        else
                        {
                            // Sweep done: settle where the sweep task settles.
                            sweeping = false;
                            mode = static_cast<uint8_t>(BMP180_OSS_HIGH_RESOLUTION);
                        }

                        bmp_app_set_oss(static_cast<bmp180_oss_t>(mode));
                    }
                }
            }
        }

        // Poll faster than the sensor can produce, so no published sample is
        // ever missed: the shortest cycle is 5 ms at OSS0.
        rtems_task_wake_after(RTEMS_MILLISECONDS_TO_TICKS(1));
    }
}
#endif
