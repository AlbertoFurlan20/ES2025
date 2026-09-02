# RTEMS I2C driver for the Bosch BMP180 digital pressure sensor.
Datasheet: BST-BMP180-DS000-09 Rev 2.5, April 2013

## Layout

One folder per module, mirrored across `src/`, `inc/` and `tests/`. Only `inc/`
is on the include path, never `inc/<module>/`, so every include names the module
it reaches into — `#include "bmp_app/app.h"`, not `#include "app.h"`. Filenames
inside a module carry no prefix, because the folder already says it.

| Module | What it is |
|--------|------------|
| `bmp180/` | the driver — the scope of the project |
| `i2c/` | the polled STM32F4 master it sits on, since the BSP ships none |
| `bmp_app/` | the sampler task: owns the device, publishes the snapshot, holds the control surface |
| `telemetry/` | the telemetry task: reads that surface, formats the wire, writes USART2 |

```
src/                       inc/                      tests/
  init.cpp                   constants.h               Makefile
  bmp180/driver.cpp          bmp180/driver.h           bmp_app/test_snapshot.cpp
  i2c/i2c.cpp                bmp180/ioctls.h           telemetry/test_fmt.cpp
  bmp_app/app.cpp            bmp180/regs.h
  telemetry/wire.cpp         bmp180/types.h
  telemetry/task.cpp         i2c/i2c.h
                             bmp_app/app.h
                             bmp_app/snapshot.h
                             telemetry/wire.h
                             telemetry/fmt.h
                             telemetry/task.h
```

`init.cpp` and `constants.h` stay at the top: they belong to no module, they
belong to the image. Dependencies only ever point down that table — `bmp180/`
knows nothing of `bmp_app/`, and `bmp_app/` knows nothing of `telemetry/`.

## Architecture

The driver plugs into the RTEMS generic **I2C** framework (_libdev/i2c_).
Each sensor instance is represented by a `bmp180_dev_t` whose first member is an `i2c_dev` so that the framework can upcast safely.

The public interface exposed via the /dev node is:
```cpp
  ioctl(fd, BMP180_IOCTL_READ_MEASUREMENT,  &result)   // bmp180_measurement_t
  ioctl(fd, BMP180_IOCTL_SET_OSS,           &oss)      // bmp180_oss_t
  ioctl(fd, BMP180_IOCTL_GET_OSS,           &oss)      // bmp180_oss_t
  ioctl(fd, BMP180_IOCTL_SOFT_RESET)                   // no payload
  ioctl(fd, BMP180_IOCTL_SET_TEMP_INTERVAL, &ms)       // uint32_t
  ioctl(fd, BMP180_IOCTL_GET_TEMP_INTERVAL, &ms)       // uint32_t
```

Calibration coefficients are loaded once on the first measurement and cached in the device context.
`bmp180_register` reads only the chip ID; a registration that succeeds therefore proves the wiring, not the calibration.

The Bosch compensation algorithm is implemented verbatim from the datasheet section 3.5 (Figure 4) using `int32_t` / `int64_t` arithmetic to avoid floating-point dependencies.

The bus below the driver is `src/i2c/`, a polled STM32F4 master for the same framework, since the BSP ships none.
It carries the bus recovery sequence: a slave left holding SDA by an interrupted transfer is clocked free rather than needing a power cycle.

## Sensor Conceptual flow highlighted in the docs
![img.png](readme_assets/img.png)

## Conceptual flow that the task follows

`bmp_app_sampler_task` (`src/bmp_app/app.cpp`) is the only thing that ever
opens `/dev/bmp180-0`. `bmp180_telemetry_task` (`src/telemetry/task.cpp`) drives
the profile below and feeds the stream, but it does so entirely through the
application layer's read and control surfaces - it holds no descriptor and
issues no ioctl.

Code flow:
1. Device init: open the device in RW mode (`O_RDWR` from datasheet).
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
   - Sensor configuration handling is done via IOCTL calls (see section above).
3. Oversampling sweep: for each of the four modes, `SET_OSS`, then 3 discarded
   warm-up samples and 500 measured ones.
4. Continuous run at `BMP180_OSS_HIGH_RESOLUTION` for drift, jitter and
   self-heating analysis, until the board is reset.
5. Data Handling:
   - On failure: the errno is pushed as an `E` record and the loop continues.
     A read failure is data, not a reason to stop.
   - On success: timestamp, temperature, pressure and mode are pushed as an `S`
     record.

There is no inter-sample sleep and no print in the acquisition path, so
consecutive timestamps bound the true acquisition cycle time. The sampler never
touches the console at all: records are formatted and written by
`bmp180_telemetry_task`, one priority below it, so the UART can never delay a
measurement. The wire format and what a console stall does cost are in
`inc/telemetry/wire.h`. Statistics are computed host-side — see `TESTING.md` §4.

## Application layer

`bmp_app` (`src/bmp_app/`) sits above the driver so that more than one task
can use the sensor. One sampler task owns `/dev/bmp180-0` and publishes each
measurement into a snapshot; consumers read that snapshot without blocking and
without adding bus traffic, and change the mode or the temperature interval
through setters the sampler applies at the top of its next cycle.

That indirection is the point: consumers never hold an fd on the device, so
`oss` has exactly one writer and two consumers cannot fight over the mode.

```cpp
bmp_app_sample_t s;
if (bmp_app_read(&s)) { /* s.pressure_pa, s.t_us, s.oss, s.seq */ }

bmp_app_set_oss(BMP180_OSS_ULTRA_HIGH_RES);   /* applied next cycle */
```

**A successful read does not mean a fresh reading.** A failed cycle does not
publish, so a sensor that stops answering leaves the last good sample in place
and `bmp_app_read` goes on returning it, indefinitely and plausibly. That is
deliberate — the alternative is stamping stale values with a fresh timestamp,
which is the class of defect B1 was — but it puts the staleness check on the
consumer. Compare `seq` against your previous read to see whether anything is
new, or `t_us` against the current uptime if you need a bound on age, and call
`bmp_app_last_error()` to find out why the data stopped moving.

This is the default path, not a build flag. `bmp180_telemetry_task` subscribes
to the sampler, forwards each published sample to telemetry with the sampler's
own timestamp, and reports acquisition errors on the edge. It is the only
producer of `S` records. See `TESTING.md` §10.

## Build

Toolchain path comes from `../local.cmake` only (copy `local.cmake.example`).
`compile.sh`, `Makefile` and the root `CMakeLists.txt` all read it, and all
build with `-Wall -Wextra -Werror -std=c++17 -fno-exceptions -fno-rtti`.

```bash
./compile.sh          # or: make
make flash            # build + program the board over OpenOCD
make -C tests run     # host-side unit tests, no toolchain needed
```

Three build flags exist for tests that a normal build must not carry:
`-DBMP180_TEARDOWN_TEST`, `-DBMP180_NO_BUS_LOCK` and `-DI2C_RECOVERY_TEST`. See
`TESTING.md`.

`-DBMP180_CONCURRENCY_TEST` still parses but selects nothing: the second-reader
task it built lived in `sensor.cpp`, deleted when the application layer became
the only path to the device. `-DBMP180_NO_BUS_LOCK` therefore has nothing left
to race against — the I1 test needs a second reader restored before it can run.
