#include <rtems.h>
#include <rtems/bspIo.h>
#include <stdio.h>

/* Forward declarations */
rtems_task Init(rtems_task_argument ignored);
rtems_task alive_task(rtems_task_argument ignored);

/* Task ID — needed to spawn alive_task from Init */
rtems_id alive_task_id;

rtems_task Init(rtems_task_argument ignored)
{
    rtems_status_code sc;

    printf("*** [f] RTEMS alive driver starting ***\n");
    printk("*** [k] RTEMS alive driver starting ***\n");

    /* Create the heartbeat task */
    sc = rtems_task_create(
        rtems_build_name('A', 'L', 'V', 'E'),  /* 4-char name */
        1,                                       /* priority */
        RTEMS_MINIMUM_STACK_SIZE,
        RTEMS_DEFAULT_MODES,
        RTEMS_DEFAULT_ATTRIBUTES,
        &alive_task_id
    );
    if (sc != RTEMS_SUCCESSFUL) {
        printf("[f] Failed to create alive_task: %s\n", rtems_status_text(sc));
        printk("[k] Failed to create alive_task: %s\n", rtems_status_text(sc));
        rtems_task_suspend(RTEMS_SELF);
    }

    /* Start it */
    sc = rtems_task_start(alive_task_id, alive_task, 0);
    if (sc != RTEMS_SUCCESSFUL) {
        printf("[f] Failed to start alive_task: %s\n", rtems_status_text(sc));
        printk("[k] Failed to start alive_task: %s\n", rtems_status_text(sc));
    }

    /* Init task suspends itself — alive_task takes over */
    rtems_task_suspend(RTEMS_SELF);
}

/* ── RTEMS configuration ─────────────────────────────────────── */
#define CONFIGURE_APPLICATION_NEEDS_CLOCK_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_CONSOLE_DRIVER

#define CONFIGURE_MICROSECONDS_PER_TICK     1000   /* 1 ms tick → 1000 ticks/sec */
#define CONFIGURE_MAXIMUM_TASKS             4

#define CONFIGURE_RTEMS_INIT_TASKS_TABLE
#define CONFIGURE_INIT_TASK_ENTRY_POINT     Init
#define CONFIGURE_INIT_TASK_STACK_SIZE      (4 * 1024)
#define CONFIGURE_INIT_TASK_PRIORITY        1
#define CONFIGURE_INIT_TASK_INITIAL_MODES   RTEMS_DEFAULT_MODES

#define CONFIGURE_INIT
#include <rtems/confdefs.h>