#include <rtems.h>
#include <rtems/bspIo.h>
#include <stdio.h>

/*
 * Heartbeat task: prints "alive" over the BSP console (USART2)
 * once per second using rtems_task_wake_after().
 *
 * rtems_clock_get_ticks_per_second() gives us the tick rate
 * configured in CONFIGURE_MICROSECONDS_PER_TICK (default 1000 Hz),
 * so 1 second = that many ticks.
 */
rtems_task alive_task(rtems_task_argument ignored)
{
    rtems_interval ticks_per_sec = rtems_clock_get_ticks_per_second();

    while (1) {
        printf("[f] alive\n");
        // printk("[k] alive\n");
        rtems_task_wake_after(ticks_per_sec);
    }
}