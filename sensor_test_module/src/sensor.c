#include <rtems.h>
#include <stdio.h>

#include "../inc/constants.h"

rtems_task sensor_task(rtems_task_argument ignored)
{
    printf("%s %s - Placeholder\n", DEBUG_TITLE, SENSOR_TASK_TITLE);
}
