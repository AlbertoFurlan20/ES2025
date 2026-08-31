//
// Application layer over the BMP180 driver.
//
// One sampler task owns /dev/bmp180-0 and publishes each measurement into a
// snapshot. Any task reads that snapshot without blocking and without adding
// bus traffic, and changes the runtime parameters through the setters below,
// which the sampler applies at the top of its next cycle.
//
// This is what gives `oss` a single writer: consumers never hold an fd on the
// device, so they cannot fight over the mode.
//

#ifndef ES2025_BMP_APP_H
#define ES2025_BMP_APP_H

#include <rtems.h>

#include "bmp_app_snapshot.h"
#include "bmp_types.h"

/* ── Read surface ─────────────────────────────────────────────── */

/**
 * @brief Copy the latest published measurement. Never blocks, no bus traffic.
 *
 * @param out receives the sample; untouched unless this returns true.
 *
 * @return false if nothing has been published yet, if @p out is null, or if a
 *         higher-priority caller exhausted its retry budget against a publish
 *         in progress. Retry on the next cycle.
 */
bool bmp_app_read(bmp_app_sample_t* out);

/**
 * @brief errno of the most recent failed sampler cycle, 0 if the last cycle
 *        succeeded.
 *
 * @details A failed cycle does not republish, so the snapshot keeps its
 *          previous values and its previous timestamp. Read this together with
 *          `t_us` to tell a stale reading from a fresh one.
 */
int bmp_app_last_error();

/* ── Control surface ──────────────────────────────────────────── */

/**
 * @brief Request an oversampling mode. Validated here, applied by the sampler
 *        at the top of its next cycle.
 *
 * @return 0, or -EINVAL if @p oss is not one of the four modes.
 */
int bmp_app_set_oss(bmp180_oss_t oss);

/** @brief The last requested mode. The mode a given sample was taken at is in
 *         that sample's `oss` field, which can differ for one cycle. */
bmp180_oss_t bmp_app_get_oss();

/**
 * @brief Request a temperature re-conversion interval in milliseconds.
 *        0 restores a temperature conversion per measurement.
 *
 * @return 0. Every value is legal; the driver treats 0 as "no cache".
 */
int bmp_app_set_temp_interval_ms(uint32_t ms);

/** @brief The last requested temperature re-conversion interval. */
uint32_t bmp_app_get_temp_interval_ms();

/** @brief Ask the sampler to issue a soft reset before its next measurement.
 *         One-shot: repeated calls before the next cycle collapse to one. */
void bmp_app_request_soft_reset();

/* ── Tasks ────────────────────────────────────────────────────── */

/**
 * @brief Sampler task entry point. Opens the device once, then loops applying
 *        pending control and publishing measurements. Deletes itself if the
 *        open fails.
 */
rtems_task bmp_app_sampler_task(rtems_task_argument ignored);

#ifdef BMP_APP_TEST
/**
 * @brief Demo consumer, built only under -DBMP_APP_TEST. Polls the read
 *        surface, pushes each new sample to telemetry, and steps the mode
 *        through the four settings so the control surface is exercised from a
 *        consumer rather than from the sampler itself.
 */
rtems_task bmp_app_demo_reader_task(rtems_task_argument ignored);
#endif

#endif //ES2025_BMP_APP_H
