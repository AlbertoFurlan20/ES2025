# Known Issues — v1.0.0

Audit of `C_src` against the BMP180 datasheet (BST-BMP180-DS000-09 Rev 2.5),
ST RM0090, and the RTEMS I2C framework sources
(`cpukit/dev/i2c/i2c-bus.c`, `cpukit/dev/i2c/i2c-dev.c`).

Two sections: **BUGS** (defects — the code does something other than what it
should) and **IMPROVEMENTS** (correct-but-weak — robustness, portability,
maintainability). Each is ordered by severity, highest first.

Severity meaning:

| Level | Meaning |
|-------|---------|
| **High** | Wrong output, memory corruption, or crash on a path that is reachable |
| **Medium** | Wrong behaviour under specific but realistic conditions |
| **Low** | Latent, cosmetic, or only reachable through code not currently wired in |

Legend: **[live]** = on the default boot path (`Entrypoint` → `bmp180_oss_sweep_task`).
**[latent]** = in code not currently reached, so it cannot bite until that code is wired in.

---

# BUGS

## High

### B1 — Conversion wait can expire before the conversion finishes [live]

`C_src/src/bmp180.cpp:104-106`, `C_src/src/bmp180.cpp:145-147`

`rtems_task_wake_after(n)` blocks for **between `n-1` and `n` ticks** — the call
can land anywhere inside the current tick, so the first tick is partial. With a
1 ms tick, every conversion delay in the driver is effectively one millisecond
shorter than intended in the worst case, and every one of them is specified at
exactly the datasheet maximum:

| Measurement | Datasheet max | Ticks requested | Worst-case actual |
|-------------|---------------|-----------------|-------------------|
| Temperature | 4.5 ms | 5 | **4.0 ms** |
| Pressure OSS0 | 4.5 ms | 5 | **4.0 ms** |
| Pressure OSS1 | 7.5 ms | 8 | **7.0 ms** |
| Pressure OSS2 | 13.5 ms | 14 | **13.0 ms** |
| Pressure OSS3 | 25.5 ms | 26 | **25.0 ms** |

All five can under-wait. Reading `0xF6` while the conversion is still running
returns the **previous** conversion result, so the failure is silent: no error
code, no NAK, just an occasional one-sample-stale reading. This directly
corrupts the noise figures the OSS sweep task is built to measure — inflated
`p2p_Pa` at higher OSS is the symptom to look for.

**Fix direction:** add one tick of margin to each constant, or poll the SCO bit
(see [I5](#i5--poll-the-sco-bit-instead-of-sleeping-a-fixed-time)).

### B2 — `bmp180_task_manual` passes the device path as the bus path [latent]

`C_src/src/sensor.cpp:110-112`

```cpp
const auto bus_path = "/dev/bmp180-0";
const auto dev_path = "/dev/bmp180-0";
```

`bus_path` must be the I2C **bus** node (`/dev/i2c-1`). `bmp180_register` passes
it to `i2c_dev_alloc_and_init`, which does `open(bus_path)` followed by
`ioctl(fd, I2C_BUS_GET_CONTROL)`. Opening the sensor's own device node instead
means that ioctl hits the BMP180 handler, which returns `-ENOTTY` for anything
outside its four commands. Registration can never succeed on this path.

### B3 — Null-pointer dereference in the registration failure handler [latent]

`C_src/src/sensor.cpp:175-182`

```cpp
catch (const std::exception& e)
{
    printf("Exception on sensor registration, killing task...\n%s", e.what());
    dev_ptr->base.destroy(&dev_ptr->base);   // dev_ptr may still be nullptr
```

`dev_ptr` is initialised to `nullptr` at line 114 and only assigned after
`bmp180_register` returns. Any exception thrown before that assignment reaches
this handler with `dev_ptr == nullptr`, and the cleanup faults. The error path
is strictly worse than no error path at all.

### B4 — Use-after-free: device freed while its `/dev` node is still published [latent]

`C_src/src/sensor.cpp:158`, `C_src/src/sensor.cpp:213`

Confirmed against RTEMS sources: `i2c_dev_alloc_and_init` installs
`i2c_dev_destroy_and_free` as the `destroy` handler, and that function does
`close(dev->bus_fd)` followed by **`free(dev)`**. Meanwhile `i2c_dev_register`
has already handed the same pointer to `IMFS_make_generic_node` as the node's
context.

Calling `dev_ptr->base.destroy(&dev_ptr->base)` directly therefore frees the
device while `/dev/bmp180-0` still resolves to it. Any subsequent `open()` or
`ioctl()` on that node — including one from another task — dereferences freed
memory.

**Fix direction:** tear down with `unlink(dev_path)`, which drives the IMFS node
destructor and reaches `destroy` in the right order. Never call `destroy`
directly on a registered device.

## Medium

### B5 — Debug output is gated on `#ifndef DEBUG` (inverted) [latent]

`C_src/src/sensor.cpp:130`, `C_src/src/sensor.cpp:205`

The blocks that dump calibration coefficients and exception text are wrapped in
`#ifndef DEBUG` — they compile in when `DEBUG` is **not** defined and vanish when
it is. `DEBUG` is not defined anywhere in `CMakeLists.txt`, `Makefile`, or
`compile.sh` (verified), so today these always compile. The intent is clearly
the opposite.

### B6 — Negative temperatures print malformed [live]

`C_src/src/sensor.cpp:69-72`, `C_src/src/sensor.cpp:359-361`

Both readers split the centi-degree value into whole and fractional parts with
`/ 10` and `% 10` — the live sweep task at line 359:

```cpp
printf("Temperature: %ld.%ld degC   Pressure: %ld Pa\n",
       static_cast<long>(m.temperature_cdeg / 10),
       static_cast<long>(m.temperature_cdeg % 10),
```

and `bmp180_task` at line 69 with the same expressions cast to `int`.

C integer division truncates toward zero and `%` keeps the sign of the dividend,
so `-24` (i.e. −2.4 °C) prints as `-2.-4`. The BMP180 is specified down to
−40 °C, so this is inside the operating range, not a corner case.

### B7 — Registration failure leaves a silently dead board [live]

`C_src/src/init.cpp:63-74` vs `C_src/src/init.cpp:80`

The registration failure path is deliberately non-fatal, justified by the
comment:

```cpp
// Keep going: the heartbeat still proves the system is alive.
```

But the heartbeat is commented out seventeen lines further down:

```cpp
//setupTask(heartbeat_task_id, "ALVE", 3, alive_task);
```

So when registration fails, the sweep task's `open("/dev/bmp180-0")` fails, the
task deletes itself, the init task suspends, and the board goes quiet with no
indication of why. The comment documents a safety net that no longer exists.

### B8 — `oss` is not validated in `bmp180_register` → out-of-bounds read [live]

`C_src/src/bmp180.cpp:324-345`, consumed at `C_src/src/bmp180.cpp:122-147`

`BMP180_IOCTL_SET_OSS` range-checks its argument (line 275), but the `oss`
parameter of `bmp180_register` is stored straight into `dev->oss` with no check.
`bmp180_read_up` then does `ctrl_vals[oss_idx]` and `wait_ms[oss_idx]` on
4-element arrays. A caller passing an out-of-range value — trivially possible,
since `bmp180_oss_t` is a plain unscoped enum — reads past both arrays and writes
a garbage control byte to the sensor.

### B9 — Division by zero in the compensation math [live]

`C_src/src/bmp180.cpp:173`

```cpp
int32_t X2 = (static_cast<int32_t>(cal->MC) << 11) / (X1 + static_cast<int32_t>(cal->MD));
```

There is no guard on `X1 + MD == 0`. The calibration sanity check rejects
coefficients that are all-zero or all-ones, but it cannot rule out a runtime
`X1` that happens to cancel `MD`. On Cortex-M4 the result depends on the `DIV_0_TRP`
bit in `SCB->CCR`: either a silent zero or a UsageFault. Reachable with corrupt
calibration data or a wild `UT` from [B1](#b1--conversion-wait-can-expire-before-the-conversion-finishes-live).

## Low

### B10 — `int` return compared against `rtems_status_code` [latent]

`C_src/src/sensor.cpp:155`

```cpp
if (bmp180_load_calibration(dev) != RTEMS_SUCCESSFUL)
```

`bmp180_load_calibration` returns an `int` carrying `0` or an `errno` value, not
an `rtems_status_code`. This works only because `RTEMS_SUCCESSFUL == 0`. It is a
type confusion that will mislead the next reader and breaks the moment anyone
maps the return onto RTEMS status semantics.

### B11 — `setupTask` takes `rtems_id` by value [live]

`C_src/src/init.cpp:15`

```cpp
void setupTask(rtems_id task_id, const char title[4], const int prio, TaskType taskRrf)
```

`rtems_task_create(&task_id)` writes into the function's local copy. It happens
to work because `rtems_task_start` runs inside the same function, but the caller
never receives the created task's id — so no task can ever be deleted, suspended,
or signalled from `Entrypoint`. The `constexpr rtems_id sensor_task_id = 0;`
at the call site is dead.

### B12 — Zero-length I2C message stalls for the full poll budget [latent]

`C_src/src/i2c1.cpp:112-124`, `C_src/src/i2c1.cpp:146-168`

An `i2c_msg` with `len == 0` falls into the write path (waits `BTF` that never
arrives) or the N>2 read path (waits `RxNE` that never arrives), burning all
100 000 poll iterations before returning `-ETIMEDOUT`. The BMP180 driver never
generates such a message, so this is only reachable if the bus is shared with
other device drivers. No memory is corrupted — `goto fail` fires before any
buffer write — but the bus mutex is held throughout.

### B13 — Strict-aliasing violation in the calibration sanity check [live]

`C_src/src/bmp180.cpp:81-88`

```cpp
const auto* words = reinterpret_cast<const uint16_t*>(&self->calib);
```

Reading a `bmp180_calib_t` through a `uint16_t*` is undefined behaviour. It works
at `-O0` and the struct genuinely has no padding (11 × 16-bit members, `sizeof`
= 22), but nothing enforces either property, and the build is one `-O2` away from
the optimiser being entitled to reorder these loads.

### B14 — Missing newline in the calibration print [latent]

`C_src/src/sensor.cpp:133`

`printf("Calibration is loaded: %d", cal_is_loaded);` — no `\n`, so the line runs
into the following `printf`.

---

# IMPROVEMENTS

## High

### I1 — A measurement is not atomic on the bus

`C_src/src/bmp180.cpp:216-244`

Verified in `cpukit/dev/i2c/i2c-bus.c`: `i2c_bus_do_transfer` obtains the bus
mutex at entry and releases it at exit, so the lock covers **one transfer**. A
BMP180 measurement is four transfers with `rtems_task_wake_after` sleeps between
them:

```
write ctrl_meas(temp) → sleep → read 0xF6 → write ctrl_meas(press) → sleep → read 0xF6
```

Two tasks measuring concurrently would each trigger conversions into the other's
sleep window and read each other's results. `self->oss` is likewise read and
written with no synchronisation.

The framework already exports the primitives to fix this — `i2c_bus_obtain` /
`i2c_bus_release` (`i2c-bus.c:45-58`) — or add a per-device mutex in
`bmp180_dev_t`. Single-task today, so this is a design gap rather than a live
failure, but it is the one that will be most expensive to retrofit later.

### I2 — Seven functions declared `static` in a shared header

`C_src/inc/bmp.h:20-128`

`bmp180_write_reg`, `bmp180_read_regs`, `bmp180_read_ut`, `bmp180_read_up`,
`bmp180_compensate`, `bmp180_ioctl` and `bmp180_destroy` are declared `static`
inside `namespace bmp` in a header included by three translation units. Each TU
gets its own internal-linkage declaration; only `bmp180.cpp` provides
definitions. `init.cpp` and `sensor.cpp` therefore carry declared-but-undefined
statics, which is exactly what `-Wunused-function` exists to catch.

These are implementation details: move them into an anonymous namespace inside
`bmp180.cpp` and delete them from the header.

### I3 — Three divergent toolchain-path mechanisms

`C_src/Makefile:1-2`, `C_src/compile.sh:6`, `CMakeLists.txt:10-14`

- `Makefile` hardcodes `/Users/albertofurlan/Developer/PoliMi/...`
- `compile.sh` sources `../.env/setup.env`
- CMake requires `RTEMS_LOCAL_PATH` from `local.cmake`

Only the last two are per-developer. The `Makefile` path is baked in and will not
build on any other machine — including this one, where the toolchain actually
lives under `/Volumes/POLI/tools/`. Collapse onto one mechanism.

### I4 — No warning flags anywhere in the build

`CMakeLists.txt:36-44`, `C_src/Makefile:8-16`, `C_src/compile.sh:13-19`

No `-Wall`, no `-Wextra`. A large share of the defects above are things the
compiler would have reported for free: the unused `heartbeat_task_id`, the
declared-but-undefined statics ([I2](#i2--seven-functions-declared-static-in-a-shared-header)),
and the by-value `rtems_id` ([B11](#b11--setuptask-takes-rtems_id-by-value-live)).
This is the highest ratio of defects-found to effort in the whole list.

## Medium

### I5 — Poll the SCO bit instead of sleeping a fixed time

`C_src/src/bmp180.cpp:104-106`, `C_src/src/bmp180.cpp:145-147`

The BMP180 exposes a start-of-conversion bit (`ctrl_meas` bit 5) that stays set
while a conversion is in flight. Polling it — with the current sleep as a
timeout — makes [B1](#b1--conversion-wait-can-expire-before-the-conversion-finishes-live)
structurally impossible rather than merely padded against, and returns typical-case
readings sooner than the datasheet maximum.

### I6 — `<bits/stl_pair.h>` is a libstdc++ internal header

`C_src/inc/bmp.h:9`

Include `<utility>`. `bits/` headers carry no stability guarantee across
libstdc++ versions and do not exist at all on libc++.

### I7 — C++ exceptions used to signal ordinary error codes

`C_src/src/sensor.cpp:4`, `C_src/src/sensor.cpp:190-208`

```cpp
if (const auto return_code = bmp180_do_measurement(dev_ptr, &m); return_code != 0)
{
    throw std::exception();     // caught three lines later
}
```

The thrown object carries no information — `e.what()` yields the generic
`std::exception` string, so the `printf` at line 206 logs nothing useful, and the
actual `return_code` is discarded. In exchange the binary links the unwinder and
`std::exception` machinery, which is real flash on a Cortex-M4. The driver layer
below already uses plain return codes; the task layer should match.

### I8 — `I2C_POLL_BUDGET` is an iteration count, not a timeout

`C_src/src/i2c1.cpp:27`

100 000 iterations is a different wall-clock duration at every optimisation
level and clock configuration, so the bound is not reviewable against any I2C
timing requirement. Worse, it is a tight busy-wait held under the bus mutex, so
a stuck bus spins the CPU at the caller's priority instead of yielding.

Express the budget in microseconds against `rtems_clock_get_uptime_nanoseconds()`,
or move the driver to interrupt-driven transfers.

### I9 — `READ_MEASUREMENT` is declared `_IOW` but writes back to the caller

`C_src/inc/bmp180_ioctls.h:31`

Direction should be `_IOR` (or `_IOWR`). RTEMS does not copy ioctl payloads based
on the encoded direction, so there is no runtime effect today — but the encoded
direction is part of the command number, so fixing this **changes the command
value** and is a breaking ABI change for anything already compiled against it.
Batch it with any other ioctl-numbering change.

### I10 — Documentation has drifted from the code

- `C_src/README.md:50-54` — the ioctl list omits `BMP180_IOCTL_SET_OSS`, though
  the surrounding prose says configuration is done through ioctl.
- `C_src/README.md:48` and `C_src/TESTING.md:70,83` — both describe a "1 Hz read
  loop", but the shipped sweep task runs at `BMP180_READ_FREQUENCY` = 5 Hz
  (`C_src/src/sensor.cpp:278`).
- `C_src/src/sensor.cpp:93` — "calibration reads is done on registration" is
  wrong; `bmp180_register` reads only the chip ID, and calibration is loaded
  lazily on first measurement.
- `C_src/README.md:19` — a `**[Claude GENERATED]**` marker left in the prose.

### I11 — Sweep task runs on `RTEMS_MINIMUM_STACK_SIZE`

`C_src/src/init.cpp:20`, `C_src/src/sensor.cpp:298`

Every task created through `setupTask` gets the minimum stack. The sweep task
puts a 128-byte sample array on it and calls newlib `printf`, whose formatting
buffers are not small. The init task was given `4 * 1024` explicitly
(`init.cpp:101`), which suggests the minimum was already found wanting once.

## Low

### I12 — `ERROR` is too generic a macro name

`C_src/inc/constants.h:10`

`#define ERROR "[[ERROR]]"` is an unprefixed, all-caps macro in a header. It will
collide with any vendor or system header that uses the same identifier. Prefix
these consistently (`ES_ERROR`, `LOG_ERROR`).

### I13 — Task policy constant lives in the ioctl header

`C_src/inc/bmp180_ioctls.h:13`

`BMP180_READ_FREQUENCY` is an application scheduling decision sitting in the
header that defines the driver's public ioctl ABI. It belongs with the task code.
`BMP180_CHIP_ID_VALUE` here also duplicates `BMP180_CHIP_ID_EXPECTED` in
`bmp_regs.h:36` — two names for one constant, and only the latter is used.

### I14 — No I2C bus recovery sequence

`C_src/src/i2c1.cpp`

If a slave is reset mid-transfer it can hold SDA low indefinitely, and the
peripheral will report `BUSY` forever. Every transfer then returns `-EBUSY` until
a power cycle. The standard remedy is to switch SCL to GPIO output, clock out up
to nine pulses until SDA releases, then issue a STOP and re-init.

### I15 — No teardown path

`C_src/src/bmp180.cpp`, `C_src/src/i2c1.cpp`

Nothing ever unlinks `/dev/bmp180-0` or `/dev/i2c-1`. Fine for a
boot-and-run-forever image, but it means the destroy handlers are dead code that
has never executed — see [B4](#b4--use-after-free-device-freed-while-its-dev-node-is-still-published-latent),
which is precisely the kind of defect that hides in an untested teardown path.

### I16 — Redundant `memset` after `calloc`

`C_src/src/bmp180.cpp:346`

`i2c_dev_alloc_and_init` allocates with `calloc` (verified in `i2c-dev.c`), so
`memset(&dev->calib, 0, sizeof(dev->calib))` clears already-zero memory.
Harmless, but it implies a guarantee the caller has not actually checked.

### I17 — Header guard style is inconsistent

`C_src/inc/constants.h:1` uses `#pragma once`; the other four headers use
`ES2025_*` include guards. Pick one.

### I18 — Dead code carried in the build

`alive_task`, `bmp180_task` and `bmp180_task_manual` are all compiled but
unreachable, and `heartbeat_task_id` (`C_src/src/init.cpp:77`) is unused. Two of
the three latent High-severity bugs above ([B2](#b2--bmp180_task_manual-passes-the-device-path-as-the-bus-path-latent),
[B3](#b3--null-pointer-dereference-in-the-registration-failure-handler-latent),
[B4](#b4--use-after-free-device-freed-while-its-dev-node-is-still-published-latent))
live in `bmp180_task_manual`. Either wire it in and test it, or delete it.

---

## Summary

| Section | High | Medium | Low | Total |
|---------|------|--------|-----|-------|
| BUGS | 4 | 5 | 5 | 14 |
| IMPROVEMENTS | 4 | 7 | 7 | 18 |

Of the 14 bugs, **6 are on the live boot path** (B1, B6, B7, B8, B9, B11) and
8 are latent in unwired code — 3 of which ([B2](#b2--bmp180_task_manual-passes-the-device-path-as-the-bus-path-latent),
[B3](#b3--null-pointer-dereference-in-the-registration-failure-handler-latent),
[B4](#b4--use-after-free-device-freed-while-its-dev-node-is-still-published-latent))
are concentrated in `bmp180_task_manual`.

[B1](#b1--conversion-wait-can-expire-before-the-conversion-finishes-live) is the
one to fix first: it is live, it silently corrupts the primary output, and it
undermines the OSS sweep that the release is built around.
