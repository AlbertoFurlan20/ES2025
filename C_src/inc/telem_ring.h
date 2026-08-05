//
// Single-producer / single-consumer ring buffer for telemetry records.
//
// The producer is the acquisition task; the consumer is the emitter task.
// Only the producer advances the tail and only the consumer advances the head,
// so no mutex is required - the acquire/release pairs below are sufficient.
//
// Deliberately free of RTEMS dependencies so it can be unit-tested on the host.
//

#ifndef ES2025_TELEM_RING_H
#define ES2025_TELEM_RING_H

#include <atomic>

/**
 * @brief Record kind. Maps to the single-character wire tag.
 */
enum telem_kind : uint8_t
{
    TELEM_SAMPLE = 0, /**< 'S' */
    TELEM_ERROR  = 1, /**< 'E' */
    TELEM_DROP   = 2  /**< 'D' */
};

/**
 * @brief One telemetry record, fixed size so the ring needs no allocation.
 *
 * @param t_us   monotonic microseconds since boot
 * @param t_cdeg SAMPLE: temperature in 0.1 degC; otherwise unused
 * @param p_pa   SAMPLE: pressure in Pa; otherwise unused
 * @param aux    ERROR: errno; DROP: number of records lost; SAMPLE: unused
 * @param oss    SAMPLE: oversampling setting 0..3; otherwise 0
 * @param kind   @see telem_kind
 */
struct telem_rec_t
{
    uint64_t t_us;
    int32_t  t_cdeg;
    int32_t  p_pa;
    int32_t  aux;
    uint8_t  oss;
    uint8_t  kind;
};

/**
 * @brief Lock-free SPSC ring holding @p N slots (one is reserved to
 *        distinguish full from empty, so usable capacity is @p N - 1).
 *
 * @tparam N slot count, must be a power of two
 */
template <uint32_t N>
class TelemRing
{
    static_assert(N >= 2, "ring needs at least two slots");
    static_assert((N & (N - 1)) == 0, "N must be a power of two");

public:
    /**
     * @brief Producer side. Never blocks.
     *
     * @return true if stored; false if the ring was full, in which case the
     *         drop counter is incremented and the record is discarded. The
     *         NEW record is dropped - already-queued records are never
     *         overwritten, so the surviving data stays contiguous in time.
     */
    bool push(const telem_rec_t& rec) noexcept
    {
        const uint32_t tail = tail_.load(std::memory_order_relaxed);
        const uint32_t next = (tail + 1) & (N - 1);

        if (next == head_.load(std::memory_order_acquire))
        {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        buf_[tail] = rec;
        tail_.store(next, std::memory_order_release);
        return true;
    }

    /**
     * @brief Consumer side. Never blocks.
     *
     * @return true if a record was written to @p out, false if the ring is empty.
     */
    bool pop(telem_rec_t& out) noexcept
    {
        const uint32_t head = head_.load(std::memory_order_relaxed);

        if (head == tail_.load(std::memory_order_acquire))
        {
            return false;
        }

        out = buf_[head];
        head_.store((head + 1) & (N - 1), std::memory_order_release);
        return true;
    }

    /**
     * @brief Read and clear the drop counter atomically.
     */
    uint32_t take_dropped() noexcept
    {
        return dropped_.exchange(0, std::memory_order_relaxed);
    }

    /** @brief Usable capacity (one slot is reserved). */
    static constexpr uint32_t capacity() noexcept { return N - 1; }

private:
    telem_rec_t           buf_[N]{};
    std::atomic<uint32_t> head_{0};
    std::atomic<uint32_t> tail_{0};
    std::atomic<uint32_t> dropped_{0};
};

#endif //ES2025_TELEM_RING_H
