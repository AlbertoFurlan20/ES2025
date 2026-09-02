//
// Telemetry output: the wire format, and the synchronous writers for it.
//
// There is no queue and no emitter task. Acquisition lives in
// bmp_app_sampler_task and never calls anything here; the consumer that reads
// the application layer is the same task that formats and writes, so a record
// it builds it also puts on the wire.
//
// The consequence is deliberate and worth knowing: a stalled console stalls the
// consumer, which is then not running to observe publishes, and the snapshot is
// one deep. Samples lost that way are not silent - the consumer watches `seq`
// and emits a 'D' record for the hole - but they are lost. Acquisition timing
// is untouched either way, which is the property that matters.
//
// Wire format is frozen at v1.
//

#ifndef ES2025_TELEMETRY_WIRE_H
#define ES2025_TELEMETRY_WIRE_H

#include <cstdint>

/**
 * @brief Emit the session header. Call once, before any record.
 *
 * @param fw       firmware version string, e.g. "1.0.0"
 * @param temp_ms  temperature re-conversion interval in ms; 0 means every sample
 */
void telem_emit_header(const char* fw, uint32_t temp_ms);

/**
 * @brief Monotonic microseconds since boot.
 *
 * @details Derived from rtems_clock_get_uptime_nanoseconds(), which reads the
 *          hardware timecounter and therefore resolves finer than the 1 ms tick.
 */
uint64_t telem_now_us();

/**
 * @brief Write one measurement as an 'S' record.
 *
 * @param t_us must be the timestamp the sampler took at acquisition, not the
 *             time this call happens to run. The host derives the achieved rate
 *             and the jitter from differences between consecutive 'S' records,
 *             so substituting a write-time clock would put this task's
 *             scheduling into the measurement.
 */
void telem_emit_sample(uint64_t t_us, int32_t t_cdeg, int32_t p_pa, uint8_t oss);

/** @brief Write an 'E' record carrying @p err. */
void telem_emit_error(uint64_t t_us, int32_t err);

/**
 * @brief Write a 'D' record reporting @p lost samples missing from the stream.
 *
 * @details Raised when the publication sequence skips: the snapshot holds one
 *          measurement, so a consumer that misses a publish never sees it. The
 *          analysis needs this to tell a real interval from one that spans a
 *          hole.
 */
void telem_emit_drop(uint64_t t_us, int32_t lost);

#endif //ES2025_TELEMETRY_WIRE_H
