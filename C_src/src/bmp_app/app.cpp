#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <rtems.h>

#include "bmp_app/app.h"
#include "bmp180/ioctls.h"

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

    // Task woken after each publish, 0 if nobody subscribed. rtems_id is a
    // 32-bit word, so the atomic is lock-free on Cortex-M4.
    std::atomic<uint32_t> g_subscriber{0};

    /**
     * @brief Monotonic microseconds since boot.
     *
     * @details Duplicated from telem_now_us rather than shared, so that this
     *          module keeps no telemetry dependency. Both must stay on the
     *          uptime clock: a sample's `t_us` travels into the stream unchanged
     *          and is differenced against timestamps the emitter writes.
     */
    uint64_t now_us()
    {
        return rtems_clock_get_uptime_nanoseconds() / 1000u;
    }

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

void bmp_app_subscribe(const rtems_id task_id)
{
    g_subscriber.store(task_id, std::memory_order_relaxed);
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
        // Left in last_error and nowhere else. This module does not know what a
        // telemetry stream is; the subscriber polls last_error on its wait
        // timeout, so the failure still reaches a capture even though no sample
        // is ever published and no event is ever sent.
        g_last_error.store(errno, std::memory_order_relaxed);
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
            const int err = errno;

            // Do not publish. The snapshot keeps the last good values and the
            // last good timestamp, so a reader sees the true age of the data
            // instead of a fresh timestamp on a stale reading.
            g_last_error.store(err, std::memory_order_relaxed);

            // Yield for a tick. A successful cycle sleeps inside the conversion
            // wait, so it gives up the CPU on its own; a failing one returns
            // immediately, and without this the loop spins at priority 2 and
            // starves every lower-priority task in the system. Measured: pulling
            // SDA produced 2.70 s of total console silence, emitter included,
            // because nothing below this task could run to report the fault.
            rtems_task_wake_after(RTEMS_MILLISECONDS_TO_TICKS(1));
            continue;
        }

        g_last_error.store(0, std::memory_order_relaxed);
        g_snapshot.publish(now_us(), m.temperature_cdeg, m.pressure_pa,
                           active_oss);

        // After the publish, never before: the payload is stable by the time
        // the subscriber can act on the event, so it never spends its retry
        // budget on a write in progress.
        if (const rtems_id sub = g_subscriber.load(std::memory_order_relaxed);
            sub != 0)
        {
            rtems_event_send(sub, BMP_APP_EVENT_SAMPLE);
        }
    }
}
