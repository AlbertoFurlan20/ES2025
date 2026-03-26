#include <rtems.h>
#include <rtems/bspIo.h>
#include <stdio.h>

#include "constants.h"

rtems_task sensor_task(rtems_task_argument ignored);
rtems_task alive_task(rtems_task_argument ignored);

void setupTask(rtems_id task_id, const char title[4], const int prio, rtems_task* taskRrf)
{
    rtems_status_code task = rtems_task_create(
        rtems_build_name(title[0], title[1], title[2],title[3]),
        prio,
        RTEMS_MINIMUM_STACK_SIZE,
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

rtems_task Entrypoint(rtems_task_argument ignored)
{
    printf("%s %s %s\n", DEBUG_TITLE, STARTING_TITLE, SENSOR_TASK_TITLE);

    rtems_id sensor_task_id;
    rtems_id heartbeat_task_id;

    setupTask(sensor_task_id, "SENS", 1, sensor_task);
    setupTask(heartbeat_task_id, "ALVE", 1, alive_task);

    rtems_task_suspend(RTEMS_SELF);
}

/* ── RTEMS configuration ─────────────────────────────────────── */
#define CONFIGURE_APPLICATION_NEEDS_CLOCK_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_CONSOLE_DRIVER

#define CONFIGURE_MICROSECONDS_PER_TICK     1000   /* 1 ms tick → 1000 ticks/sec */
#define CONFIGURE_MAXIMUM_TASKS             4

#define CONFIGURE_RTEMS_INIT_TASKS_TABLE
#define CONFIGURE_INIT_TASK_ENTRY_POINT     Entrypoint
#define CONFIGURE_INIT_TASK_STACK_SIZE      (4 * 1024)
#define CONFIGURE_INIT_TASK_PRIORITY        1
#define CONFIGURE_INIT_TASK_INITIAL_MODES   RTEMS_DEFAULT_MODES

#define CONFIGURE_INIT
#include <rtems/confdefs.h>
