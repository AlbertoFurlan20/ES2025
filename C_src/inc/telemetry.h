//
// Telemetry front-end: acquisition-side push helpers plus the emitter task that
// drains them to the console.
//
// The split exists so that the UART can never stall acquisition. Pushes are
// O(1) and non-blocking; all formatting and all blocking I/O happen in the
// lower-priority emitter task. On overflow the producer drops and counts rather
// than waiting, and the count is emitted as a 'D' record so loss is visible in
// the captured data instead of silently biasing it.
//
// Wire format is frozen at v1.
//

#ifndef ES2025_TELEMETRY_H
#define ES2025_TELEMETRY_H

#include <rtems.h>

/**
 * @brief Emit the session header. Call once, before starting the emitter task.
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
 * @brief Queue a measurement. Non-blocking.
 *
 * @return false if the ring was full and the record was dropped.
 */
bool telem_push_sample(uint64_t t_us, int32_t t_cdeg, int32_t p_pa, uint8_t oss);

/**
 * @brief Queue an error. Non-blocking.
 *
 * @return false if the ring was full and the record was dropped.
 */
bool telem_push_error(uint64_t t_us, int32_t err);

/**
 * @brief Emitter task entry point. Drains the ring to the console forever,
 *        sleeping briefly whenever it finds the ring empty.
 */
rtems_task telem_emitter_task(rtems_task_argument ignored);

#endif //ES2025_TELEMETRY_H
