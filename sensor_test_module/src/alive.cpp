#include <rtems.h>
#include <cstdio>


rtems_task alive_task(rtems_task_argument ignored)
{
    const rtems_interval ticks_per_sec = rtems_clock_get_ticks_per_second();

    // ReSharper disable once CppDFAEndlessLoop
    while (true) {
        printf("[f] alive\n");

        rtems_task_wake_after(ticks_per_sec);
    }
}