#include <rtems.h>
#include <stdio.h>


rtems_task alive_task(rtems_task_argument ignored)
{
    rtems_interval ticks_per_sec = rtems_clock_get_ticks_per_second();

    while (1) {
        printf("[f] alive\n");

        rtems_task_wake_after(ticks_per_sec);
    }
}