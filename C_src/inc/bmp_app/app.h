//
// Application layer over the BMP180 driver.
//
// One sampler task owns /dev/bmp180-0 and publishes each measurement into a
// snapshot. Any task reads that snapshot without blocking and without adding
// bus traffic, and changes the runtime parameters through the setters below,
// which the sampler applies at the top of its next cycle.
//
// This is what gives `oss` a single writer: consumers never hold an fd on the
// device, so they cannot fight over the mode. Nothing outside this module opens
// /dev/bmp180-0, telemetry included - the stream is fed by a consumer that
// reads this surface like any other.
//

#ifndef ES2025_BMP_APP_APP_H
#define ES2025_BMP_APP_APP_H

#include <rtems.h>

#include "bmp_app/snapshot.h"
#include "bmp180/types.h"

/* ── Read surface ─────────────────────────────────────────────── */

/**
 * @brief Copy the latest published measurement. Never blocks, no bus traffic.
 *
 * @warning **A successful return does not mean the sample is fresh.** This hands
 *          back whatever was published last, and a failed cycle does not
 *          publish, so a sensor that stopped answering - or a sampler task that
 *          died on a failed open - leaves the same sample here indefinitely.
 *          The call keeps succeeding and the values keep looking plausible.
 *
 *          A consumer must decide for itself what counts as stale, using one of
 *          the two fields provided for it:
 *
 *          - `seq` unchanged since the last read means no new sample. This is
 *            the cheap check and it is what the telemetry bridge uses.
 *          - `t_us` against the current uptime gives the true age in
 *            microseconds. Use this one if a bound on age matters.
 *
 *          Pair either with @see bmp_app_last_error to learn *why* the data
 *          stopped moving. The layer deliberately does not fabricate freshness,
 *          but it cannot force a caller to look.
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

/* ── Publication ──────────────────────────────────────────────── */

/**
 * @brief Event the sampler sends to the subscriber after each publish.
 */
#define BMP_APP_EVENT_SAMPLE RTEMS_EVENT_0

/**
 * @brief Register @p task_id to be woken with BMP_APP_EVENT_SAMPLE after every
 *        publish. Pass 0 to unsubscribe.
 *
 * @details One subscriber, because the only consumer that must not miss a
 *          sample is the telemetry bridge; anything else can poll
 *          @see bmp_app_read on its own schedule. The event is a wake-up, not
 *          the data: the subscriber still reads the snapshot, and still has to
 *          check `seq` because an event coalesces if two publishes land before
 *          it runs.
 *
 *          Subscribing late loses nothing that matters - the first read after
 *          subscribing returns whatever is current.
 */
void bmp_app_subscribe(rtems_id task_id);

/* ── Tasks ────────────────────────────────────────────────────── */

/**
 * @brief Sampler task entry point. Opens the device once, then loops applying
 *        pending control and publishing measurements. Deletes itself if the
 *        open fails.
 */
rtems_task bmp_app_sampler_task(rtems_task_argument ignored);

#endif //ES2025_BMP_APP_APP_H
