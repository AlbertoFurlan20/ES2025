//
// The telemetry task: the only consumer of the BMP180 application layer in a
// normal build, and the only thing that puts records on the wire.
//
// It holds no file descriptor on the sensor and issues no ioctl. It subscribes
// to the sampler, reads published measurements through bmp_app_read, drives the
// oversampling sweep through bmp_app_set_oss, and writes each record straight
// to the console - which is USART2, and therefore the USB serial adapter on the
// development machine.
//
// Reading and writing live in one task on purpose. Splitting them would buy a
// buffer between two halves that already run in lockstep: the snapshot is one
// deep, so there is never more than one new sample per wake-up to absorb.
//

#ifndef ES2025_TELEMETRY_TASK_H
#define ES2025_TELEMETRY_TASK_H

#include <rtems.h>

/**
 * @brief Telemetry task entry point. Subscribes to the sampler, then forwards
 *        each new sample and each new error to the console, forever.
 *
 * @details Run it below the sampler's priority. A higher-priority reader can
 *          preempt a publish in progress and burn its retry budget for nothing;
 *          a lower-priority one is guaranteed to see a stable payload, and gets
 *          the CPU anyway because the sampler sleeps inside every conversion
 *          wait.
 *
 *          Because this task both reads and writes, a stalled console keeps it
 *          off the CPU while the sampler goes on publishing into a one-deep
 *          snapshot. Samples lost that way are reported as 'D' records rather
 *          than passed off as a longer cycle time.
 */
rtems_task bmp180_telemetry_task(rtems_task_argument ignored);

#endif //ES2025_TELEMETRY_TASK_H
