//
// Operator control over the console link.
//
// The same USART2 that carries telemetry out carries commands in, on PA3. This
// task owns the read side of it; nothing else in the image reads stdin.
//
// Like the telemetry task it is a consumer of bmp_app and holds no descriptor
// on the sensor: a command becomes a bmp_app_set_oss or a
// bmp_app_request_soft_reset, which the sampler applies at the top of its next
// cycle. The device therefore still has exactly one writer of `oss`.
//

#ifndef ES2025_TELEMETRY_CONTROL_H
#define ES2025_TELEMETRY_CONTROL_H

#include <rtems.h>

/**
 * @brief Control task entry point. Reads console lines forever and applies the
 *        commands it recognises.
 *
 * @details Wire syntax, one command per line:
 *
 *          | Line | Effect |
 *          |------|--------|
 *          | `O0`..`O3` | Request that oversampling mode |
 *          | `R` | Request a soft reset; device returns to power-on defaults |
 *
 *          Anything else is discarded in silence. The console is a telemetry
 *          stream with a frozen format, so this task must never write to it:
 *          an echo or an error message would appear in every capture as a line
 *          the parser has to skip.
 *
 *          Confirmation comes through the data instead. A mode change shows up
 *          as the `oss` field of the next samples, which is the mode they were
 *          actually taken at rather than the mode that was asked for.
 *
 *          The first accepted command calls @see telem_cancel_sweep. Run this
 *          task below the telemetry task: it is idle almost always, and when a
 *          command does arrive it must not preempt a publish being read.
 */
rtems_task bmp180_control_task(rtems_task_argument ignored);

#endif //ES2025_TELEMETRY_CONTROL_H
