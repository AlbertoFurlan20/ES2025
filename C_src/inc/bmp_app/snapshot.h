//
// Latest-measurement snapshot for the BMP180 application layer.
//
// One writer (the sampler task) publishes; any number of readers copy. A
// seqlock rather than a mutex, because a reader must never be able to delay
// acquisition: an RTEMS mutex held by a low-priority reader would block the
// sampler until the priority-inheritance handoff completed, whereas here the
// writer never waits at all.
//
// Deliberately free of RTEMS dependencies so it can be unit-tested on the host
//

#ifndef ES2025_BMP_APP_SNAPSHOT_H
#define ES2025_BMP_APP_SNAPSHOT_H

#include <atomic>

/**
 * @brief One published measurement.
 *
 * @param seq   count of samples published so far; compare it against the value
 *              from a previous read to detect a new sample. Never 0 on a
 *              successful read.
 * @param t_us  uptime in microseconds when the sample was taken. A reader
 *              compares it against the current uptime to learn the true age of
 *              the data - a failed cycle does not republish, so this timestamp
 *              stops advancing when the sensor stops answering.
 * @param oss   the oversampling mode this sample was actually taken at, which
 *              is not necessarily the mode currently requested: a control
 *              change applies at the top of the sampler's next cycle.
 */
struct bmp_app_sample_t
{
    uint32_t seq;
    uint64_t t_us;
    int32_t  temperature_cdeg; /**< deci-degrees, despite the name; see I17b */
    int32_t  pressure_pa;
    uint8_t  oss;
};

/**
 * @brief Single-writer, many-reader snapshot of the latest measurement.
 *
 * @details The sequence counter is odd while a publish is in progress and even
 *          when the payload is stable. A reader takes the counter, copies the
 *          payload, and takes the counter again; if it moved, the copy may be a
 *          mix of two generations and is discarded.
 *
 *          The counter is `uint32_t` and not `uint64_t` on purpose:
 *          `std::atomic<uint64_t>` is not lock-free on ARMv7-M and lowers to a
 *          libatomic call that takes a lock, which is precisely what this class
 *          exists to avoid. At the fastest mode the sensor publishes about 200
 *          samples a second, so the counter wraps after about 124 days of
 *          continuous running. That is harmless: a consumer compares `seq` for
 *          inequality to detect a new sample, never for ordering.
 *
 *          The payload fields are plain rather than atomic for the same reason -
 *          `t_us` is 64-bit - and are ordered by the fences around them. A
 *          reader that observes a partially written payload discards it, so the
 *          only requirement on those fields is that the compiler and the CPU do
 *          not move them across the sequence stores, which the fences enforce.
 */
class BmpAppSnapshot
{
public:
    /**
     * @brief Retry budget for a reader.
     *
     * @details A reader running at a HIGHER priority than the sampler can
     *          preempt it mid-publish, and would then spin forever waiting for
     *          a writer that cannot run. Bounding the retries turns that
     *          deadlock into a `false` return the caller can handle. A reader at
     *          lower priority than the sampler never observes an in-progress
     *          publish at all and never needs more than one attempt.
     */
    static constexpr unsigned MAX_ATTEMPTS = 8;

    /** @brief Writer side. Single writer only. Never blocks. */
    void publish(const uint64_t t_us, const int32_t temperature_cdeg,
                 const int32_t pressure_pa, const uint8_t oss) noexcept
    {
        const uint32_t s = seq_.load(std::memory_order_relaxed);

        seq_.store(s + 1, std::memory_order_relaxed); // odd: write in progress
        std::atomic_thread_fence(std::memory_order_release);

        t_us_ = t_us;
        temperature_cdeg_ = temperature_cdeg;
        pressure_pa_ = pressure_pa;
        oss_ = oss;

        std::atomic_thread_fence(std::memory_order_release);
        seq_.store(s + 2, std::memory_order_relaxed); // even: payload stable
    }

    /**
     * @brief Reader side. Never blocks, never touches the bus.
     *
     * @return true if @p out holds a coherent sample. false if nothing has been
     *         published yet, or if the retry budget ran out; @p out is left
     *         untouched in both cases.
     */
    bool read(bmp_app_sample_t& out) const noexcept
    {
        for (unsigned attempt = 0; attempt < MAX_ATTEMPTS; ++attempt)
        {
            const uint32_t s1 = seq_.load(std::memory_order_relaxed);

            if ((s1 & 1u) != 0u)
            {
                continue; // a publish is in progress
            }
            if (s1 == 0u)
            {
                return false; // nothing published yet
            }

            std::atomic_thread_fence(std::memory_order_acquire);

            const uint64_t t_us = t_us_;
            const int32_t  temperature_cdeg = temperature_cdeg_;
            const int32_t  pressure_pa = pressure_pa_;
            const uint8_t  oss = oss_;

            std::atomic_thread_fence(std::memory_order_acquire);

            if (seq_.load(std::memory_order_relaxed) != s1)
            {
                continue; // a publish landed mid-copy; the copy may be torn
            }

            out.seq = s1 >> 1;
            out.t_us = t_us;
            out.temperature_cdeg = temperature_cdeg;
            out.pressure_pa = pressure_pa;
            out.oss = oss;
            return true;
        }

        return false;
    }

private:
    std::atomic<uint32_t> seq_{0};

    uint64_t t_us_{0};
    int32_t  temperature_cdeg_{0};
    int32_t  pressure_pa_{0};
    uint8_t  oss_{0};
};

#endif //ES2025_BMP_APP_SNAPSHOT_H
