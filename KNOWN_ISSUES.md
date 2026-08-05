# Known Issues

**Audited against: v1.2.0 (2026-08-05).** The measurement path is still unchanged
since v1.0.0, so every defect in it stands as originally described. R1 removed
dead code and tightened the build; it changed no runtime behaviour beyond the
registration-failure record described under B7.

Audit of `C_src` against the BMP180 datasheet (BST-BMP180-DS000-09 Rev 2.5),
ST RM0090, and the RTEMS I2C framework sources
(`cpukit/dev/i2c/i2c-bus.c`, `cpukit/dev/i2c/i2c-dev.c`).

## Changes in v1.2.0 (R1)

**14 issues resolved: B2, B3, B4, B5, B6, B7, B10, B14, I2, I3, I4, I7, I17c, I18.**
Each is annotated in place below rather than deleted, so the reasoning stays
readable and every existing cross-link keeps working.

| Change | Closes |
|--------|--------|
| Deleted `bmp180_task_manual` | B2, B3, B4, B5, B10, B14, and I7 as a side effect |
| Deleted `bmp180_task` | B6 |
| Deleted `alive_task` and `alive.cpp` | the phantom heartbeat behind B7 |
| `E`-record on registration failure | B7 |
| All build paths read `local.cmake` | I3 |
| `-Wall -Wextra` everywhere | I4 |
| `-std=c++17` pinned | I17c |
| `-fno-exceptions -fno-rtti` | I7 |

**Eight of fourteen bugs are now closed**, and the `[latent]` category is empty —
there is no longer any unreachable code in the tree. The six remaining bugs are
all `[live]`: B1, B8, B9, B11, B12, B13.

Warning triage from the new `-Wall -Wextra` build, as the R1 gate required: one
`-Wunused-parameter` in `Entrypoint` and seven instances of
[I2](#i2--seven-functions-declared-static-in-a-shared-header). Both were fixed in
this release, so the build is now warning-free. The gate also
predicted [B11](#b11--setuptask-takes-rtems_id-by-value-live) would surface — it
did not. Passing `rtems_id` by value is legal C++ and no warning class covers
it, so B11 still needs the fix it always did.

## Changes in v1.1.0

| Issue | Change |
|-------|--------|
| [B1](#b1--conversion-wait-can-expire-before-the-conversion-finishes-live) | **Confirmed on hardware.** No longer a prediction — measured in two independent captures. See [`E_analysis/BASELINE.md`](E_analysis/BASELINE.md). |
| [I11](#i11--resolved-in-v110--sweep-task-ran-on-rtems_minimum_stack_size) | **Resolved.** `setupTask` takes an explicit stack size; both tasks get 4 KB. |
| I17b, I17c | **New**, found during Phase 0 implementation. |

Two sections: **BUGS** (defects — the code does something other than what it
should) and **IMPROVEMENTS** (correct-but-weak — robustness, portability,
maintainability). Each is ordered by severity, highest first.

Severity meaning:

| Level | Meaning |
|-------|---------|
| **High** | Wrong output, memory corruption, or crash on a path that is reachable |
| **Medium** | Wrong behaviour under specific but realistic conditions |
| **Low** | Latent, cosmetic, or only reachable through code not currently wired in |

Legend: **[live]** = on the default boot path
(`Entrypoint` → `bmp180_telemetry_task` + `telem_emitter_task`).
**[latent]** = in code not currently reached, so it cannot bite until that code is
wired in. As of v1.2.0 no `[latent]` issue remains open; the tags are kept on
resolved entries as a record of what they were.

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

> **Confirmed on hardware, v1.1.0 baseline.** Two independent 150 s captures. Each
> oversampling step should cut RMS noise ~1.0 Pa; in each run **exactly one
> transition fails at ~5.4σ, and a different one each time** — run 1 at 0→1
> (+0.393 Pa observed, z = +5.65), run 2 at 1→2 (+0.146 Pa, z = +5.39). The
> unspoiled transitions match the datasheet closely (run 2: z = +0.22, +0.71), so
> the sensor can meet spec and something intermittently stops it. A systematic
> cause would spoil the same mode every run; only a stochastic one moves. Jitter
> below 0.5 µs and zero dropped records rule out the scheduler and telemetry
> back-pressure respectively. Derivation:
> [`A_report/fragments/02-conversion-timing-defect-evidence.md`](A_report/fragments/02-conversion-timing-defect-evidence.md).
> Numbers and R2 criteria: [`E_analysis/BASELINE.md`](E_analysis/BASELINE.md).

### B2 — `bmp180_task_manual` passes the device path as the bus path [latent]

> **RESOLVED in v1.2.0 (R1).** Closed by deleting `bmp180_task_manual` in R1. The function no longer exists.

`C_src/src/sensor.cpp:112-114`

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

> **RESOLVED in v1.2.0 (R1).** Closed by deleting `bmp180_task_manual` in R1. The function no longer exists.

`C_src/src/sensor.cpp:177-184`

```cpp
catch (const std::exception& e)
{
    printf("Exception on sensor registration, killing task...\n%s", e.what());
    dev_ptr->base.destroy(&dev_ptr->base);   // dev_ptr may still be nullptr
```

`dev_ptr` is initialised to `nullptr` at line 116 and only assigned after
`bmp180_register` returns. Any exception thrown before that assignment reaches
this handler with `dev_ptr == nullptr`, and the cleanup faults. The error path
is strictly worse than no error path at all.

### B4 — Use-after-free: device freed while its `/dev` node is still published [latent]

> **RESOLVED in v1.2.0 (R1).** Closed by deleting `bmp180_task_manual` in R1. The function no longer exists.

`C_src/src/sensor.cpp:160`, `C_src/src/sensor.cpp:215`

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

> **RESOLVED in v1.2.0 (R1).** Closed by deleting `bmp180_task_manual` in R1. The function no longer exists.

`C_src/src/sensor.cpp:132`, `C_src/src/sensor.cpp:207`

The blocks that dump calibration coefficients and exception text are wrapped in
`#ifndef DEBUG` — they compile in when `DEBUG` is **not** defined and vanish when
it is. `DEBUG` is not defined anywhere in `CMakeLists.txt`, `Makefile`, or
`compile.sh` (verified), so today these always compile. The intent is clearly
the opposite.

### B6 — Negative temperatures print malformed [latent since v1.1.0]

> **RESOLVED in v1.2.0 (R1).** Closed by deleting `bmp180_task`, the last remaining site, in R1. The telemetry emitter never splits a signed value, so the whole class of error is gone from the tree.

`C_src/src/sensor.cpp:71-74`

```cpp
printf("Temperature: %d.%d degC   Pressure: %ld Pa\n",
       static_cast<int>(m.temperature_cdeg / 10),
       static_cast<int>(m.temperature_cdeg % 10),
```

C integer division truncates toward zero and `%` keeps the sign of the dividend,
so `-24` (i.e. −2.4 °C) prints as `-2.-4`. The BMP180 is specified down to
−40 °C, so this is inside the operating range, not a corner case.

**Downgraded from `[live]` to `[latent]` in v1.1.0.** The second site, in
`bmp180_oss_sweep_task`, was deleted along with that task. The surviving site is
in `bmp180_task`, which is no longer on the boot path. The telemetry emitter is
unaffected: it writes `t_cdeg` as a single signed integer via `fmt_i32` and never
splits it, so the whole class of error cannot occur there — the split now happens
host-side in `plot.py`, in Python, where `/` and `%` floor consistently.

### B7 — Registration failure leaves a silently dead board [live]

> **RESOLVED in v1.2.0 (R1).** Registration failure now pushes `E <t_us> 19` (`ENODEV`) into the telemetry stream before the tasks start, and the comment promising a heartbeat is gone. Failure is non-fatal but no longer silent.

`C_src/src/init.cpp:63-74` vs `C_src/src/init.cpp:80`

The registration failure path is deliberately non-fatal, justified by the
comment:

```cpp
// Keep going: the heartbeat still proves the system is alive.
```

There is no heartbeat. In v1.0.0 the call was commented out seventeen lines
below; v1.1.0 removed even that, so the comment now refers to something with no
trace in the file at all. `alive_task` still exists in `alive.cpp` and is still
never started.

So when registration fails, the telemetry task's `open("/dev/bmp180-0")` fails,
the task deletes itself, the init task suspends, and the board goes quiet with no
indication of why. The comment documents a safety net that does not exist.

**Slightly worse in v1.1.0, and more consequential.** Because the console now
carries a machine-readable stream rather than prose, a silent death is harder to
notice by eye — the capture simply contains a header and no `S` records.
`E_analysis` reports that honestly (`samples=0`), so the failure is detectable,
but only after a capture rather than at a glance. Either start the heartbeat or
emit an `E` record and delete the comment.

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

> **RESOLVED in v1.2.0 (R1).** Closed by deleting `bmp180_task_manual` in R1. The function no longer exists.

`C_src/src/sensor.cpp:157`

```cpp
if (bmp180_load_calibration(dev) != RTEMS_SUCCESSFUL)
```

`bmp180_load_calibration` returns an `int` carrying `0` or an `errno` value, not
an `rtems_status_code`. This works only because `RTEMS_SUCCESSFUL == 0`. It is a
type confusion that will mislead the next reader and breaks the moment anyone
maps the return onto RTEMS status semantics.

### B11 — `setupTask` takes `rtems_id` by value [live]

`C_src/src/init.cpp:16-17`

```cpp
void setupTask(rtems_id task_id, const char title[4], const int prio, TaskType taskRrf)
```

`rtems_task_create(&task_id)` writes into the function's local copy. It happens
to work because `rtems_task_start` runs inside the same function, but the caller
never receives the created task's id — so no task can ever be deleted, suspended,
or signalled from `Entrypoint`. The `constexpr rtems_id sensor_task_id = 0;`
at the call site is dead.

### B12 — Zero-length I2C message stalls for the full poll budget [latent]

`C_src/src/i2c.cpp:112-124`, `C_src/src/i2c.cpp:146-168`

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

> **RESOLVED in v1.2.0 (R1).** Closed by deleting `bmp180_task_manual` in R1. The function no longer exists.

`C_src/src/sensor.cpp:135`

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

> **RESOLVED in v1.2.0 (R1).** Declarations moved from `bmp.h` into `bmp180.cpp`, where the definitions live. `bmp.h` falls from 158 to 70 lines and exposes only `bmp180_register`, `bmp180_selftest`, `bmp180_load_calibration` and `bmp180_do_measurement`. Build is warning-free.

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

> **RESOLVED in v1.2.0 (R1).** All three build paths — root `CMakeLists.txt`, `C_src/Makefile`, `C_src/compile.sh` (and `flash.sh`) — now read `RTEMS_LOCAL_PATH` from `local.cmake` alone, with an environment override. `.env/setup.env` is no longer consulted.

`C_src/Makefile:1-2`, `C_src/compile.sh:6`, `CMakeLists.txt:10-14`

- `Makefile` hardcodes `/Users/albertofurlan/Developer/PoliMi/...`
- `compile.sh` sources `../.env/setup.env`
- CMake requires `RTEMS_LOCAL_PATH` from `local.cmake`

Only the last two are per-developer. The `Makefile` path is baked in and will not
build on any other machine — including this one, where the toolchain actually
lives under `/Volumes/POLI/tools/`. Collapse onto one mechanism.

### I4 — No warning flags anywhere in the build

> **RESOLVED in v1.2.0 (R1).** `-Wall -Wextra` added to all three build paths. The list it produced was triaged and then emptied — see [I2](#i2--seven-functions-declared-static-in-a-shared-header). The tree compiles clean, so promoting to `-Werror` is now a one-line change.

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

> **RESOLVED in v1.2.0 (R1).** Closed by deleting `bmp180_task_manual`, the only `throw`/`catch`/`<stdexcept>` user in the tree. `-fno-exceptions -fno-rtti` are now set on all build paths; measured saving 520 bytes of `.text` at `-O0`.

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

`C_src/src/i2c.cpp:27`

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

### I11 — RESOLVED in v1.1.0 — sweep task ran on `RTEMS_MINIMUM_STACK_SIZE`

~~Every task created through `setupTask` got the minimum stack, while the init
task was given `4 * 1024` explicitly — suggesting the minimum had already been
found wanting once.~~

**Fixed.** `setupTask` now takes an explicit `stack_size` parameter
(`C_src/src/init.cpp:16-17`) and both the acquisition and emitter tasks are
created with 4 KB. No `RTEMS_MINIMUM_STACK_SIZE` remains in `init.cpp`.

Retained here rather than deleted so the remediation rounds have a record of what
Phase 0 already absorbed.

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

`C_src/src/i2c.cpp`

If a slave is reset mid-transfer it can hold SDA low indefinitely, and the
peripheral will report `BUSY` forever. Every transfer then returns `-EBUSY` until
a power cycle. The standard remedy is to switch SCL to GPIO output, clock out up
to nine pulses until SDA releases, then issue a STOP and re-init.

### I15 — No teardown path

`C_src/src/bmp180.cpp`, `C_src/src/i2c.cpp`

Nothing ever unlinks `/dev/bmp180-0` or `/dev/i2c-1`. Fine for a
boot-and-run-forever image, but it means the destroy handlers are dead code that
has never executed — see [B4](#b4--use-after-free-device-freed-while-its-dev-node-is-still-published-latent),
which is precisely the kind of defect that hides in an untested teardown path.

### I16 — Redundant `memset` after `calloc`

`C_src/src/bmp180.cpp:346`

`i2c_dev_alloc_and_init` allocates with `calloc` (verified in `i2c-dev.c`), so
`memset(&dev->calib, 0, sizeof(dev->calib))` clears already-zero memory.
Harmless, but it implies a guarantee the caller has not actually checked.

### I17b — `temperature_cdeg` is a misnomer

`C_src/inc/bmp_types.h:49`

```c
int32_t  temperature_cdeg;   /* Temperature [steps of 0.1°C] */
```

The name says centi-degrees; the comment and the value say deci-degrees. The
comment is correct — datasheet Table 1 gives 0.1 °C resolution — so the name is
wrong by a factor of ten.

Found during Phase 0: it produced a real unit bug in the host tooling, where a
drift metric came out 10× off. `E_analysis/metrics.py` now absorbs the
conversion so no consumer has to know, but the field itself should be renamed
`temperature_ddeg` (or the value scaled) so the trap stops existing. Renaming
touches the ioctl payload struct, so it is a breaking change — batch it with
[I9](#i9--read_measurement-is-declared-_iow-but-writes-back-to-the-caller).

### I17c — Target build does not pin the C++ standard

> **RESOLVED in v1.2.0 (R1).** `-std=c++17` pinned in `C_src/Makefile` and `C_src/compile.sh`. The root CMake build already set `CMAKE_CXX_STANDARD 17`.

`C_src/compile.sh:13-19`, `C_src/Makefile:8-16`

Neither passes `-std=`. The code uses C++17 features — `if` with initializer in
`telemetry.cpp` and structured bindings in `sensor.cpp` — and compiles only
because this `arm-rtems7-g++` happens to default to `gnu++17`.

`CMakeLists.txt:25` sets `CMAKE_CXX_STANDARD 17` and `C_src/tests/Makefile` sets
`-std=c++17`, so two of the four build paths pin it and two do not. An older
toolchain would break the unpinned pair with confusing syntax errors. Fix
alongside [I3](#i3--three-divergent-toolchain-path-mechanisms).

### I17 — Header guard style is inconsistent

`C_src/inc/constants.h:1` uses `#pragma once`; the other four headers use
`ES2025_*` include guards. Pick one.

### I18 — Dead code carried in the build

> **RESOLVED in v1.2.0 (R1).** All three dead functions deleted — `bmp180_task`, `bmp180_task_manual` and `alive_task` (with `alive.cpp`). `sensor.cpp` drops from 304 to 94 lines.

`alive_task`, `bmp180_task` and `bmp180_task_manual` are all compiled but
unreachable. All three latent High-severity bugs above
([B2](#b2--bmp180_task_manual-passes-the-device-path-as-the-bus-path-latent),
[B3](#b3--null-pointer-dereference-in-the-registration-failure-handler-latent),
[B4](#b4--use-after-free-device-freed-while-its-dev-node-is-still-published-latent))
live in `bmp180_task_manual`, and [B5](#b5--debug-output-is-gated-on-ifndef-debug-inverted-latent),
[B10](#b10--int-return-compared-against-rtems_status_code-latent) and
[B14](#b14--missing-newline-in-the-calibration-print-latent) do too. Deleting that
one function closes six of the fourteen bugs on this page without debugging any
of them — which is why R1 does exactly that.

**Partly reduced in v1.1.0:** `bmp180_oss_sweep_task`, `isqrt32` and the unused
`heartbeat_task_id` were deleted with the Phase 0 rewrite. The three functions
above remain.

---

## Summary

As of v1.2.0:

| Section | Open | Resolved |
|---------|------|----------|
| BUGS | 6 | 8 (B2, B3, B4, B5, B6, B7, B10, B14) |
| IMPROVEMENTS | 14 | 6 (I2, I3, I4, I7, I11, I17c) |

**All six remaining bugs are `[live]`** — B1, B8, B9, B11, B12, B13. The
`[latent]` category is empty: R1 deleted every unreachable function in the tree,
so there is no longer anywhere for a bug to hide from execution. B12 is the
exception that proves it: it is an unreachable *branch* inside live code, not
dead code, and needs a length guard added rather than anything removed.

R1 closed eight bugs while debugging none of them. Six went with
`bmp180_task_manual`, one with `bmp180_task`, and B7 was fixed by making
registration failure visible in the telemetry stream. This was the cheapest
round in the plan by a wide margin, and it is now spent — every bug left costs
real work.

[B1](#b1--conversion-wait-can-expire-before-the-conversion-finishes-live) remains
the one to fix first, and is no longer a matter of judgement: it is **confirmed
on hardware** across two independent captures, it silently corrupts the primary
output, and it is the reason the v1.1.0 baseline shows noise rising with
oversampling. [`E_analysis/BASELINE.md`](E_analysis/BASELINE.md) states the six
criteria R2 must satisfy to close it.
