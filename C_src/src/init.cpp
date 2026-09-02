#include <rtems.h>
#include <rtems/bspIo.h>
#include <cerrno>
#include <cstdio>

#include "constants.h"
#include "i2c/i2c.h"
#include "bmp180/driver.h"
#include "bmp_app/app.h"
#include "telemetry/control.h"
#include "telemetry/task.h"
#include "telemetry/wire.h"

#define DRIVER_VERSION "2.0.0"

/**
 * @brief Create and start one task, returning its id through @p task_id.
 *
 * @details @p task_id is an out-parameter. Taking it by value would have
 *          rtems_task_create fill a local copy the caller never sees, leaving no
 *          way to delete, suspend or signal the task afterwards.
 */
template <typename TaskType>
void setupTask(rtems_id* task_id, const char title[4], const int prio,
               const size_t stack_size, TaskType taskRrf)
{
    rtems_status_code task = rtems_task_create(
        rtems_build_name(title[0], title[1], title[2], title[3]),
        prio,
        stack_size,
        RTEMS_DEFAULT_MODES,
        RTEMS_DEFAULT_ATTRIBUTES,
        task_id
    );

    if (task != RTEMS_SUCCESSFUL)
    {
        printf("%s %s %s (%s)\n", ES_ERROR, ES_CREATE_FAIL, ES_SENSOR_TASK_TITLE,
               rtems_status_text(task));

        rtems_task_suspend(RTEMS_SELF);
    }

    task = rtems_task_start(
        *task_id,
        taskRrf,
        0
    );

    if (task != RTEMS_SUCCESSFUL)
    {
        printf("%s Failed to start %s (%s)\n", ES_ERROR, ES_SENSOR_TASK_TITLE,
               rtems_status_text(task));
    }
}

rtems_task Entrypoint(const rtems_task_argument ignored)
{
    (void)ignored;

    printf("%s %s %s\n", ES_DEBUG_TITLE, ES_STARTING_TITLE, ES_SENSOR_TASK_TITLE);

    // Hardware-independent check of the Bosch compensation math.
    bmp::bmp180_selftest();

    // Bring up the hardware I2C1 bus the BMP180 driver depends on.
    if (stm32f4_register_i2c("/dev/i2c-1", STM32F4_I2C1_HW) != 0)
    {
        printf("%s Failed to register I2C1 bus, killing init\n", ES_ERROR);
        rtems_task_suspend(RTEMS_SELF);
    }

    // Session header goes out synchronously, before any record can be emitted -
    // including the registration error below, so the stream is never headerless.
    // temp_ms reports the temperature re-conversion interval the device will be
    // registered with, so captures taken either side of a change to it are
    // comparable without consulting the firmware.
    telem_emit_header(DRIVER_VERSION, BMP180_DEFAULT_TEMP_INTERVAL_MS);

#ifdef BMP180_TEARDOWN_TEST
    // Register, open, unlink and confirm the node is gone, before the device the
    // rest of the run uses is registered.
    bmp::bmp180_teardown_test("/dev/i2c-1", "/dev/bmp180-0");
#endif

    // Register the BMP180 device node once on top of the I2C1 bus. This is the
    // first real I2C traffic: a chip-id read. An IO error here now means the
    // sensor wiring/pins, not the software layers below.
    const auto [reg_outcome, dev] =
        bmp::bmp180_register("/dev/i2c-1", "/dev/bmp180-0", BMP180_OSS_HIGH_RESOLUTION);
    if (reg_outcome != RTEMS_SUCCESSFUL or dev == nullptr)
    {
        // Non-fatal, but it must not be silent: a capture keeps the telemetry
        // stream and nothing else, so the failure is recorded there rather than
        // left to console output nobody is reading. Written synchronously, like
        // the header above - there is no queue to hold it and no emitter task
        // to drain one.
        telem_emit_error(telem_now_us(), ENODEV);

        printf("%s BMP180 registration failed (%s)\n", ES_ERROR,
               rtems_status_text(reg_outcome));
    }
    else
    {
        printf("%s BMP180 registered on /dev/bmp180-0\n", ES_DEBUG_TITLE);
    }

    rtems_id sampler_task_id = 0;
    rtems_id telemetry_task_id = 0;
    rtems_id control_task_id = 0;

    // Priority order is the whole design. The sampler owns the device and must
    // never wait on a consumer. Telemetry sits below it, so it can only ever
    // read a snapshot that is already stable, and it gets the CPU during the
    // conversion wait the sampler sleeps through. Control is lowest: it is
    // blocked in read() almost always, and when a command does arrive it must
    // not preempt a publish being read one priority above it.
    setupTask(&sampler_task_id, "SAMP", 2, 4 * 1024, bmp_app_sampler_task);
    setupTask(&telemetry_task_id, "TELE", 3, 4 * 1024, bmp180_telemetry_task);
    setupTask(&control_task_id, "CTRL", 2, 4 * 1024, bmp180_control_task);


    rtems_task_suspend(RTEMS_SELF);
}

/* ── RTEMS configuration ─────────────────────────────────────── */
#define CONFIGURE_APPLICATION_NEEDS_CLOCK_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_CONSOLE_DRIVER

#define CONFIGURE_MICROSECONDS_PER_TICK     1000   /* 1 ms tick → 1000 ticks/sec */
#define CONFIGURE_MAXIMUM_TASKS             6

/* stdin/stdout/stderr take 3 fds; default max is 3 → any open() returns ENFILE.
 * Bump so the I2C bus node + device node can be opened. */
#define CONFIGURE_MAXIMUM_FILE_DESCRIPTORS  8

#define CONFIGURE_RTEMS_INIT_TASKS_TABLE
#define CONFIGURE_INIT_TASK_ENTRY_POINT     Entrypoint
#define CONFIGURE_INIT_TASK_STACK_SIZE      (4 * 1024)
#define CONFIGURE_INIT_TASK_PRIORITY        1
#define CONFIGURE_INIT_TASK_INITIAL_MODES   RTEMS_DEFAULT_MODES

#define CONFIGURE_INIT
#include <rtems/confdefs.h>
