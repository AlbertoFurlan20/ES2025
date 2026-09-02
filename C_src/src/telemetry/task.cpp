//
// See telemetry/task.h for why this task exists rather than a push from the
// sampler, and why it writes the wire itself rather than handing records to an
// emitter.
//

#include <rtems.h>

#include "telemetry/task.h"
#include "bmp_app/app.h"
#include "telemetry/wire.h"

namespace
{
    // Cap on the wait for a sample event. The sampler sends no event when it
    // fails - and none at all if its open() failed and it deleted itself - so
    // the wait has to time out or an errno sitting in the app layer would never
    // reach the stream.
    constexpr uint32_t ERROR_POLL_MS = 100;

    // Sweep profile. Unchanged from the pre-app-layer task, so captures stay
    // comparable against the v1.5.0 gate figures in TESTING.md sections 4 and 8.
    constexpr int SWEEP_SAMPLES = 500;
    constexpr int WARMUP = 3;

    /** @brief Mode the continuous run settles at once the sweep is done. */
    constexpr uint8_t CONTINUOUS_OSS =
        static_cast<uint8_t>(BMP180_OSS_HIGH_RESOLUTION);
}

rtems_task bmp180_telemetry_task(const rtems_task_argument ignored)
{
    (void)ignored;

    bmp_app_subscribe(rtems_task_self());

    // Requested through the control surface, not through an ioctl: this task
    // holds no fd, and the sampler applies the change at the top of its next
    // cycle. Which mode a given sample was actually taken at comes back in the
    // sample's own `oss`, which is what the phase logic below keys on rather
    // than assuming the change landed immediately.
    uint8_t target_oss = static_cast<uint8_t>(BMP180_OSS_ULTRA_LOW_POWER);
    bmp_app_set_oss(static_cast<bmp180_oss_t>(target_oss));

    bool sweeping = true;
    int  warmup_left = WARMUP;
    int  recorded = 0;

    uint32_t seen_seq = 0; // last sample looked at, recorded or not
    uint32_t run_seq = 0;  // last sample recorded; 0 = no gap baseline
    int      last_err = 0;

    while (true)
    {
        rtems_event_set events = 0;
        (void)rtems_event_receive(BMP_APP_EVENT_SAMPLE,
                                  RTEMS_EVENT_ANY | RTEMS_WAIT,
                                  RTEMS_MILLISECONDS_TO_TICKS(ERROR_POLL_MS),
                                  &events);
        (void)events;

        // Checked on both paths - event and timeout - because the sampler stops
        // sending events exactly when it starts failing. Edge, not level: a
        // disconnected sensor fails hundreds of times a second and would flood
        // the console. Recovery is not reported; a resumed run of 'S' records
        // already says the fault cleared.
        //
        // A control ioctl that fails and is followed by a successful
        // measurement in the same cycle is not seen here, because the sampler
        // clears the errno before yielding. That was equally true when the
        // sampler reported errors itself.
        if (const int err = bmp_app_last_error(); err != last_err)
        {
            if (err != 0)
            {
                telem_emit_error(telem_now_us(), err);
            }
            last_err = err;
        }

        bmp_app_sample_t sample{};
        if (!bmp_app_read(&sample) || sample.seq == seen_seq)
        {
            continue;
        }
        seen_seq = sample.seq;

        // Deliberate discards: samples taken before the requested mode landed,
        // and the warm-up after it did. Both clear the gap baseline, so a hole
        // this task made on purpose is never reported as one it lost.
        if (sample.oss != target_oss || warmup_left > 0)
        {
            if (sample.oss == target_oss)
            {
                --warmup_left;
            }
            run_seq = 0;
            continue;
        }

        // The event is a bit, not a count, and the snapshot is one deep: two
        // publishes landing before this task runs coalesce into one wake-up and
        // the older sample is gone for good - which is also what a console
        // stall costs, since the write below is what keeps this task off the
        // CPU. Say so, or the analysis reads the doubled interval as real
        // jitter.
        if (run_seq != 0 && sample.seq != run_seq + 1)
        {
            telem_emit_drop(sample.t_us,
                            static_cast<int32_t>(sample.seq - run_seq - 1));
        }

        // The sample's own timestamp, taken by the sampler at acquisition time.
        // Forwarding it unchanged is what keeps this task's scheduling - and
        // the duration of the write itself - out of the intervals the analysis
        // measures.
        telem_emit_sample(sample.t_us, sample.temperature_cdeg,
                          sample.pressure_pa, sample.oss);
        run_seq = sample.seq;

        if (!sweeping || ++recorded < SWEEP_SAMPLES)
        {
            continue;
        }

        // Block complete: step to the next mode, or settle into the continuous
        // run the drift and self-heating analysis needs.
        recorded = 0;
        run_seq = 0;

        if (target_oss < static_cast<uint8_t>(BMP180_OSS_ULTRA_HIGH_RES))
        {
            ++target_oss;
            warmup_left = WARMUP;
        }
        else
        {
            sweeping = false;
            target_oss = CONTINUOUS_OSS;
            warmup_left = 0;
        }

        bmp_app_set_oss(static_cast<bmp180_oss_t>(target_oss));
    }
}
