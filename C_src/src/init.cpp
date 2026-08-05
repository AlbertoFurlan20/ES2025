#include <rtems.h>
#include <rtems/bspIo.h>
#include <cerrno>
#include <cstdio>

#include "constants.h"
#include "i2c.h"
#include "bmp.h"
#include "telemetry.h"

#define DRIVER_VERSION "1.2.1"

rtems_task bmp180_telemetry_task(rtems_task_argument ignored);

template <typename TaskType>
void setupTask(rtems_id task_id, const char title[4], const int prio,
               const size_t stack_size, TaskType taskRrf)
{
    rtems_status_code task = rtems_task_create(
        rtems_build_name(title[0], title[1], title[2], title[3]),
        prio,
        stack_size,
        RTEMS_DEFAULT_MODES,
        RTEMS_DEFAULT_ATTRIBUTES,
        &task_id
    );

    if (task != RTEMS_SUCCESSFUL)
    {
        printf("[DEBUG] [ERROR] Failed to create sensor task (%s)...\n", rtems_status_text(task));
        printf("%s %s %s\n", ERROR, CREATE_FAIL, SENSOR_TASK_TITLE);

        rtems_task_suspend(RTEMS_SELF);
    }

    task = rtems_task_start(
        task_id,
        taskRrf,
        0
    );

    if (task != RTEMS_SUCCESSFUL)
    {
        printf("[DEBUG] [ERROR] Failed to start sensor task (%s)...\n", rtems_status_text(task));
    }
}

rtems_task Entrypoint(const rtems_task_argument ignored)
{
    (void)ignored;

    printf("%s %s %s\n", DEBUG_TITLE, STARTING_TITLE, SENSOR_TASK_TITLE);

    // Hardware-independent check of the Bosch compensation math.
    bmp::bmp180_selftest();

    // Bring up the hardware I2C1 bus the BMP180 driver depends on.
    if (stm32f4_register_i2c("/dev/i2c-1", STM32F4_I2C1_HW) != 0)
    {
        printf("[DEBUG] [ERROR] Failed to register I2C1 bus, killing init...\n");
        rtems_task_suspend(RTEMS_SELF);
    }

    // Session header goes out synchronously, before any record can be emitted -
    // including the registration error below, so the stream is never headerless.
    telem_emit_header(DRIVER_VERSION, 0);

    // Register the BMP180 device node once on top of the I2C1 bus. This is the
    // first real I2C traffic: a chip-id read. An IO error here now means the
    // sensor wiring/pins, not the software layers below.
    const auto [reg_outcome, dev] =
        bmp::bmp180_register("/dev/i2c-1", "/dev/bmp180-0", BMP180_OSS_HIGH_RESOLUTION);
    if (reg_outcome != RTEMS_SUCCESSFUL or dev == nullptr)
    {
        // Non-fatal, but it must not be silent: a capture keeps the telemetry
        // stream and nothing else, so the failure is recorded there rather than
        // left to console output nobody is reading. Queued now, drained once the
        // emitter starts a few lines below.
        telem_push_error(telem_now_us(), ENODEV);

        printf("[DEBUG] [ERROR] BMP180 registration failed (%s)\n",
               rtems_status_text(reg_outcome));
    }
    else
    {
        printf("%s BMP180 registered on /dev/bmp180-0\n", DEBUG_TITLE);
    }

    constexpr rtems_id sensor_task_id = 0;
    constexpr rtems_id emitter_task_id = 0;

    // Acquisition runs at higher priority (lower number) than emission, so the
    // console can never delay a measurement. The emitter gets the CPU during the
    // conversion sleeps, which is most of every acquisition cycle.
    setupTask(sensor_task_id, "TELE", 2, 4 * 1024, bmp180_telemetry_task);
    setupTask(emitter_task_id, "EMIT", 5, 4 * 1024, telem_emitter_task);

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
