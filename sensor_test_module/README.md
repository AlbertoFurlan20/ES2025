# RTEMS I2C driver for the Bosch BMP180 digital pressure sensor.
Datasheet: BST-BMP180-DS000-09 Rev 2.5, April 2013

## Architecture

The driver plugs into the RTEMS generic **I2C** framework (_libdev/i2c_).
Each sensor instance is represented by a `bmp180_dev_t` whose first member is an `i2c_dev` so that the framework can upcast safely.

The public interface exposed via the /dev node is:
```cpp
  ioctl(fd, BMP180_IOCTL_READ_MEASUREMENT, &result)
  ioctl(fd, BMP180_IOCTL_SET_OSS,          &oss)
  ioctl(fd, BMP180_IOCTL_GET_OSS,          &oss)
  ioctl(fd, BMP180_IOCTL_SOFT_RESET)
```

Calibration coefficients are loaded once on the first measurement and cached in the device context.

**[Claude GENERATED]** The Bosch compensation algorithm is implemented verbatim from the datasheet section 3.5 (Figure 4) using `int32_t` / `int64_t` arithmetic to avoid floating-point dependencies.

## Sensor Conceptual flow highlighted in the docs
![img.png](readme_assets/img.png)

## Conceptual flow that the task has to follow
The whole thing is placed within the classic while true loop so that i can continuosly query the sensor. 

Code flow:
1. Device init: open device in RW mode (`O_RDWR` from datasheet).
   - Early stop on error + `rtems_task_delete(RTEMS_SELF)` as usual.
2. Sensor Configuration:
   - The datasheet shows 4 possible run modes that i coded into a basic struct.
     ```cpp
     typedef enum {
       BMP180_OSS_ULTRA_LOW_POWER  = 0,
       BMP180_OSS_STANDARD         = 1,
       BMP180_OSS_HIGH_RESOLUTION  = 2,
       BMP180_OSS_ULTRA_HIGH_RES   = 3
     } bmp180_oss_t;
     ```
   - Sensor configuration handling is done via IOCTL calls (see section below).
3. Time Setup:
   - It stores the number of system clock ticks per second using rtems_clock_get_ticks_per_second() to maintain a consistent execution interval in the measurement loop.
4. Continuous Measurement Loop (Infinite Loop):
   - Measurements Query: It passes a bmp180_measurement_t data structure down to the driver via the BMP180_IOCTL_READ_MEASUREMENT IOCTL call.
5. Data Handling:
   - On failure: It logs an error using perror() and proceeds to the next cycle.
   - On success: It prints the measured Temperature and Pressure out to the standard output. Because the measurement expresses temperature in centi-degrees Celsius, it is formatted to normal degrees through integer division and modulo operations.
6. . Task Suspension: Finally, it suspends execution for one second using rtems_task_wake_after(ticks_per_sec), yielding processing time to other RTEMS tasks, before the loop evaluates again.

### IOCTL commands 
Are: `_IO`, _`IOR`, `_IOW`.
- _IOW('B', 0x01, bmp180_measurement_t) (BMP180_IOCTL_READ_MEASUREMENT)
- _IOR('B', 0x03, bmp180_oss_t) (BMP180_IOCTL_GET_OSS)
- _IO('B', 0x04) (BMP180_IOCTL_SOFT_RESET)