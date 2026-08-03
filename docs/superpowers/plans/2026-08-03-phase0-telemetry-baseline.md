# Phase 0 — Telemetry Instrumentation of v1.0.0 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Instrument the unmodified v1.0.0 driver so it emits a structured, parseable measurement stream, and build the host tooling to turn that stream into metrics — producing a baseline dataset against which all later remediation is measured.

**Architecture:** A single-producer/single-consumer ring buffer decouples acquisition from emission. The acquisition task performs the I²C measurement, timestamps it, and pushes a fixed-size binary record — O(1), never blocking. A lower-priority emitter task pops records, formats them to text, and writes them to the console. Ring overflow increments a drop counter rather than blocking the producer, and the counter is emitted as a `D` record so loss is visible in the data. Host-side Python parses the stream into DataFrames and derives metrics.

**Tech Stack:** C++17 (arm-rtems7-g++), RTEMS 7, STM32F4 BSP, Python 3.11+ with pandas/matplotlib/pytest.

## Global Constraints

- **The measurement path is not modified.** `bmp180_do_measurement`, `bmp180_read_ut`, `bmp180_read_up`, `bmp180_compensate`, all of `i2c1.cpp`, and every constant in `bmp_regs.h` stay byte-for-byte as they are in v1.0.0. This phase measures v1.0.0; it does not improve it.
- **Explicitly out of scope:** temperature decoupling, the B1 conversion-timing fix, the `-Wall -Wextra` build change, and every other item in `KNOWN_ISSUES.md`. Those are R1–R4 and come after the baseline is captured.
- **Wire format is frozen at `v1`** as specified in `B_docs/TELEMETRY_DESIGN.md`. Tags never change meaning; new fields append only to end-of-line; parsers ignore unknown trailing fields and unknown tags.
- **Session header for this phase is exactly** `#BMP180 v1 fw=1.0.0 temp_ms=0`. `temp_ms=0` means "temperature converted on every sample", which is what v1.0.0 does.
- **Timestamps** are `rtems_clock_get_uptime_nanoseconds() / 1000`, i.e. monotonic microseconds since boot, recorded at measurement *completion*.
- **No floating point in firmware.** The BSP links without float `printf` support; all firmware formatting is integer-only.
- **Toolchain lives at** `/Volumes/POLI/tools/RTEMS_toolchain/rtems`. The `C_src/Makefile` currently hardcodes a different developer's path and will not build here — Task 6 addresses this minimally, without the full I3 cleanup.

---

## File Structure

**Firmware — new:**

| File | Responsibility |
|------|----------------|
| `C_src/inc/telem_ring.h` | SPSC ring buffer and the record struct. Zero RTEMS dependency, so it is host-testable. |
| `C_src/inc/telem_fmt.h` | Integer-to-decimal formatters. Zero dependency, host-testable. |
| `C_src/inc/telemetry.h` | RTEMS-facing API: header emission, push helpers, emitter task entry point. |
| `C_src/src/telemetry.cpp` | Emitter task implementation and line assembly. |

**Firmware — modified:**

| File | Change |
|------|--------|
| `C_src/src/sensor.cpp` | Replace `bmp180_oss_sweep_task`'s on-device statistics with raw record emission. |
| `C_src/src/init.cpp` | Start the emitter task; raise task count; give both tasks explicit stacks. |

**Host tests — new:**

| File | Responsibility |
|------|----------------|
| `C_src/tests/test_telem_ring.cpp` | Ring buffer semantics, compiled natively. |
| `C_src/tests/test_telem_fmt.cpp` | Formatter edge cases, compiled natively. |
| `C_src/tests/Makefile` | Host-compiler build for the two test binaries. |

**Analysis — new:**

| File | Responsibility |
|------|----------------|
| `E_analysis/bmp180_analysis/__init__.py` | Package marker and version. |
| `E_analysis/bmp180_analysis/parse.py` | Text stream → `Session` objects. Parsing only. |
| `E_analysis/bmp180_analysis/metrics.py` | `Session` → numbers. No I/O, no plotting. |
| `E_analysis/bmp180_analysis/plot.py` | Numbers → charts. No parsing, no computation. |
| `E_analysis/tests/test_parse.py` | Parser tests against synthetic streams. |
| `E_analysis/tests/test_metrics.py` | Metric tests against synthetic sessions. |
| `E_analysis/capture.sh` | Serial capture helper. |
| `E_analysis/requirements.txt` | Pinned host dependencies. |
| `E_analysis/README.md` | How to capture and analyse. |

The ring buffer and formatters are deliberately split from `telemetry.cpp` so they carry no RTEMS dependency and can be unit-tested on the host. SPSC ring bugs are subtle and do not reproduce reliably on hardware; they must be tested where tests are cheap.

---

### Task 1: SPSC ring buffer

**Files:**
- Create: `C_src/inc/telem_ring.h`
- Create: `C_src/tests/test_telem_ring.cpp`
- Create: `C_src/tests/Makefile`

**Interfaces:**
- Consumes: nothing.
- Produces: `telem_kind` enum (`TELEM_SAMPLE`, `TELEM_ERROR`, `TELEM_DROP`); `struct telem_rec_t { uint64_t t_us; int32_t t_cdeg; int32_t p_pa; int32_t aux; uint8_t oss; uint8_t kind; }`; `template <uint32_t N> class TelemRing` with `bool push(const telem_rec_t&)`, `bool pop(telem_rec_t&)`, `uint32_t take_dropped()`, `static constexpr uint32_t capacity() noexcept`.

- [ ] **Step 1: Write the failing test**

Create `C_src/tests/test_telem_ring.cpp`:

```cpp
// Host-compiled unit tests for the SPSC telemetry ring.
// Build and run: make -C C_src/tests run

#include <cassert>
#include <cstdio>

#include "telem_ring.h"

static telem_rec_t sample(const uint64_t t, const int32_t p)
{
    telem_rec_t r{};
    r.t_us = t;
    r.t_cdeg = 244;
    r.p_pa = p;
    r.oss = 2;
    r.kind = TELEM_SAMPLE;
    return r;
}

static void test_empty_pop_fails()
{
    TelemRing<8> ring;
    telem_rec_t out{};
    assert(!ring.pop(out));
}

static void test_push_then_pop_roundtrip()
{
    TelemRing<8> ring;
    assert(ring.push(sample(1234, 96820)));

    telem_rec_t out{};
    assert(ring.pop(out));
    assert(out.t_us == 1234);
    assert(out.p_pa == 96820);
    assert(out.t_cdeg == 244);
    assert(out.oss == 2);
    assert(out.kind == TELEM_SAMPLE);
    assert(!ring.pop(out)); // drained
}

static void test_fifo_order()
{
    TelemRing<8> ring;
    for (uint64_t i = 0; i < 5; ++i)
    {
        assert(ring.push(sample(i, static_cast<int32_t>(1000 + i))));
    }
    for (uint64_t i = 0; i < 5; ++i)
    {
        telem_rec_t out{};
        assert(ring.pop(out));
        assert(out.t_us == i);
        assert(out.p_pa == static_cast<int32_t>(1000 + i));
    }
}

static void test_capacity_is_n_minus_one()
{
    TelemRing<8> ring;
    assert(ring.capacity() == 7);
}

static void test_overflow_drops_and_counts()
{
    TelemRing<8> ring; // capacity 7
    for (uint64_t i = 0; i < 7; ++i)
    {
        assert(ring.push(sample(i, 0)));
    }
    assert(!ring.push(sample(99, 0)));  // full
    assert(!ring.push(sample(100, 0))); // still full
    assert(ring.take_dropped() == 2);
    assert(ring.take_dropped() == 0);   // reset by the take
}

static void test_oldest_survives_overflow()
{
    // Overflow must discard the NEW record, never overwrite an old one.
    TelemRing<8> ring;
    for (uint64_t i = 0; i < 7; ++i)
    {
        assert(ring.push(sample(i, 0)));
    }
    assert(!ring.push(sample(999, 0)));

    telem_rec_t out{};
    assert(ring.pop(out));
    assert(out.t_us == 0); // oldest intact
}

static void test_wraparound()
{
    // Cycle several times through the buffer to exercise index masking.
    TelemRing<4> ring; // capacity 3
    for (uint64_t i = 0; i < 30; ++i)
    {
        assert(ring.push(sample(i, static_cast<int32_t>(i))));
        telem_rec_t out{};
        assert(ring.pop(out));
        assert(out.t_us == i);
    }
    assert(ring.take_dropped() == 0);
}

int main()
{
    test_empty_pop_fails();
    test_push_then_pop_roundtrip();
    test_fifo_order();
    test_capacity_is_n_minus_one();
    test_overflow_drops_and_counts();
    test_oldest_survives_overflow();
    test_wraparound();
    printf("test_telem_ring: ALL PASS\n");
    return 0;
}
```

Create `C_src/tests/Makefile`:

```make
# Host-compiled unit tests for the dependency-free telemetry helpers.
# These do NOT use the RTEMS toolchain - they run on the development machine.

CXX      ?= c++
CXXFLAGS  = -std=c++17 -Wall -Wextra -O1 -g -I../inc

BINS = test_telem_ring test_telem_fmt

.PHONY: all run clean

all: $(BINS)

test_telem_ring: test_telem_ring.cpp ../inc/telem_ring.h
	$(CXX) $(CXXFLAGS) $< -o $@

test_telem_fmt: test_telem_fmt.cpp ../inc/telem_fmt.h
	$(CXX) $(CXXFLAGS) $< -o $@

run: $(BINS)
	@for b in $(BINS); do ./$$b || exit 1; done

clean:
	rm -f $(BINS)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make -C C_src/tests test_telem_ring`
Expected: FAIL with `make: *** No rule to make target '../inc/telem_ring.h', needed by 'test_telem_ring'.  Stop.`

The header is listed as a prerequisite of the binary, so make aborts before it
ever invokes the compiler. To see the underlying compiler error instead, run
`c++ -std=c++17 -I../inc C_src/tests/test_telem_ring.cpp` directly, which gives
`fatal error: 'telem_ring.h' file not found`. Either way the step is red for the
right reason.

- [ ] **Step 3: Write minimal implementation**

Create `C_src/inc/telem_ring.h`:

```cpp
//
// Single-producer / single-consumer ring buffer for telemetry records.
//
// The producer is the acquisition task; the consumer is the emitter task.
// Only the producer advances the tail and only the consumer advances the head,
// so no mutex is required - the acquire/release pairs below are sufficient.
//
// Deliberately free of RTEMS dependencies so it can be unit-tested on the host.
//

#ifndef ES2025_TELEM_RING_H
#define ES2025_TELEM_RING_H

#include <atomic>
#include <cstdint>

/**
 * @brief Record kind. Maps to the single-character wire tag.
 */
enum telem_kind : uint8_t
{
    TELEM_SAMPLE = 0, /**< 'S' */
    TELEM_ERROR  = 1, /**< 'E' */
    TELEM_DROP   = 2  /**< 'D' */
};

/**
 * @brief One telemetry record, fixed size so the ring needs no allocation.
 *
 * @param t_us   monotonic microseconds since boot
 * @param t_cdeg SAMPLE: temperature in 0.1 degC; otherwise unused
 * @param p_pa   SAMPLE: pressure in Pa; otherwise unused
 * @param aux    ERROR: errno; DROP: number of records lost; SAMPLE: unused
 * @param oss    SAMPLE: oversampling setting 0..3; otherwise 0
 * @param kind   @see telem_kind
 */
struct telem_rec_t
{
    uint64_t t_us;
    int32_t  t_cdeg;
    int32_t  p_pa;
    int32_t  aux;
    uint8_t  oss;
    uint8_t  kind;
};

/**
 * @brief Lock-free SPSC ring holding @p N slots (one is reserved to
 *        distinguish full from empty, so usable capacity is @p N - 1).
 *
 * @tparam N slot count, must be a power of two
 */
template <uint32_t N>
class TelemRing
{
    static_assert(N >= 2, "ring needs at least two slots");
    static_assert((N & (N - 1)) == 0, "N must be a power of two");

public:
    /**
     * @brief Producer side. Never blocks.
     *
     * @return true if stored; false if the ring was full, in which case the
     *         drop counter is incremented and the record is discarded. The
     *         NEW record is dropped - already-queued records are never
     *         overwritten, so the surviving data stays contiguous in time.
     */
    bool push(const telem_rec_t& rec) noexcept
    {
        const uint32_t tail = tail_.load(std::memory_order_relaxed);
        const uint32_t next = (tail + 1) & (N - 1);

        if (next == head_.load(std::memory_order_acquire))
        {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        buf_[tail] = rec;
        tail_.store(next, std::memory_order_release);
        return true;
    }

    /**
     * @brief Consumer side. Never blocks.
     *
     * @return true if a record was written to @p out, false if the ring is empty.
     */
    bool pop(telem_rec_t& out) noexcept
    {
        const uint32_t head = head_.load(std::memory_order_relaxed);

        if (head == tail_.load(std::memory_order_acquire))
        {
            return false;
        }

        out = buf_[head];
        head_.store((head + 1) & (N - 1), std::memory_order_release);
        return true;
    }

    /**
     * @brief Read and clear the drop counter atomically.
     */
    uint32_t take_dropped() noexcept
    {
        return dropped_.exchange(0, std::memory_order_relaxed);
    }

    /** @brief Usable capacity (one slot is reserved). */
    static constexpr uint32_t capacity() noexcept { return N - 1; }

private:
    telem_rec_t           buf_[N]{};
    std::atomic<uint32_t> head_{0};
    std::atomic<uint32_t> tail_{0};
    std::atomic<uint32_t> dropped_{0};
};

#endif //ES2025_TELEM_RING_H
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make -C C_src/tests run`
Expected: `test_telem_ring: ALL PASS`

- [ ] **Step 5: Commit**

```bash
git add C_src/inc/telem_ring.h C_src/tests/test_telem_ring.cpp C_src/tests/Makefile
git commit -m "feat(telemetry): add host-tested SPSC ring buffer"
```

---

### Task 2: Integer formatters

Firmware must not depend on `printf("%llu")` — newlib-nano configurations silently mis-format 64-bit values, and the emitter is cheaper assembling one buffer and issuing a single write.

**Files:**
- Create: `C_src/inc/telem_fmt.h`
- Create: `C_src/tests/test_telem_fmt.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `size_t fmt_u64(char* dst, size_t off, uint64_t v)`, `size_t fmt_i32(char* dst, size_t off, int32_t v)`, `size_t fmt_ch(char* dst, size_t off, char c)`, `size_t fmt_str(char* dst, size_t off, const char* s)`. Each appends at `off` and returns the new offset. Callers guarantee capacity.

- [ ] **Step 1: Write the failing test**

Create `C_src/tests/test_telem_fmt.cpp`:

```cpp
// Host-compiled unit tests for the integer formatters.

#include <cassert>
#include <cstdio>
#include <cstring>

#include "telem_fmt.h"

static void check_u64(const uint64_t v, const char* expect)
{
    char buf[32] = {};
    const size_t n = fmt_u64(buf, 0, v);
    buf[n] = '\0';
    assert(strcmp(buf, expect) == 0);
}

static void check_i32(const int32_t v, const char* expect)
{
    char buf[32] = {};
    const size_t n = fmt_i32(buf, 0, v);
    buf[n] = '\0';
    assert(strcmp(buf, expect) == 0);
}

static void test_u64_boundaries()
{
    check_u64(0, "0");
    check_u64(7, "7");
    check_u64(10, "10");
    check_u64(4294967295ULL, "4294967295");            // UINT32_MAX
    check_u64(4294967296ULL, "4294967296");            // first value past 32 bits
    check_u64(18446744073709551615ULL, "18446744073709551615"); // UINT64_MAX
}

static void test_i32_boundaries()
{
    check_i32(0, "0");
    check_i32(244, "244");
    check_i32(-1, "-1");
    check_i32(-244, "-244");
    check_i32(2147483647, "2147483647");   // INT32_MAX
    check_i32(-2147483648, "-2147483648"); // INT32_MIN - negation must not overflow
}

static void test_append_offsets()
{
    char buf[64] = {};
    size_t n = 0;
    n = fmt_ch(buf, n, 'S');
    n = fmt_ch(buf, n, ' ');
    n = fmt_u64(buf, n, 1234567);
    n = fmt_ch(buf, n, ' ');
    n = fmt_i32(buf, n, -244);
    buf[n] = '\0';
    assert(strcmp(buf, "S 1234567 -244") == 0);
    assert(n == 14);
}

static void test_str_append()
{
    char buf[64] = {};
    size_t n = fmt_str(buf, 0, "#BMP180 v1");
    buf[n] = '\0';
    assert(strcmp(buf, "#BMP180 v1") == 0);
    assert(n == 10);
}

int main()
{
    test_u64_boundaries();
    test_i32_boundaries();
    test_append_offsets();
    test_str_append();
    printf("test_telem_fmt: ALL PASS\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make -C C_src/tests test_telem_fmt`
Expected: FAIL with `make: *** No rule to make target '../inc/telem_fmt.h', needed by 'test_telem_fmt'.  Stop.`
(same prerequisite behaviour as Task 1 Step 2)

- [ ] **Step 3: Write minimal implementation**

Create `C_src/inc/telem_fmt.h`:

```cpp
//
// Integer-to-decimal formatters for telemetry lines.
//
// The BSP links without floating-point printf support, and newlib-nano
// configurations do not reliably handle "%llu". Formatting by hand removes both
// risks and lets the emitter assemble a whole line before issuing one write.
//
// Every function appends at @p off and returns the new offset. The caller owns
// bounds checking; telemetry lines have a known maximum length.
//

#ifndef ES2025_TELEM_FMT_H
#define ES2025_TELEM_FMT_H

#include <cstddef>
#include <cstdint>

/** @brief Append one character. */
inline size_t fmt_ch(char* dst, const size_t off, const char c)
{
    dst[off] = c;
    return off + 1;
}

/** @brief Append a NUL-terminated string (the NUL itself is not written). */
inline size_t fmt_str(char* dst, size_t off, const char* s)
{
    while (*s != '\0')
    {
        dst[off++] = *s++;
    }
    return off;
}

/** @brief Append an unsigned 64-bit value in decimal. */
inline size_t fmt_u64(char* dst, size_t off, uint64_t v)
{
    char tmp[20]; // UINT64_MAX is 20 digits
    int n = 0;

    do
    {
        tmp[n++] = static_cast<char>('0' + static_cast<int>(v % 10u));
        v /= 10u;
    }
    while (v != 0);

    while (n > 0)
    {
        dst[off++] = tmp[--n];
    }
    return off;
}

/**
 * @brief Append a signed 32-bit value in decimal.
 *
 * @note The magnitude is taken via int64_t so that INT32_MIN does not overflow
 *       during negation.
 */
inline size_t fmt_i32(char* dst, size_t off, const int32_t v)
{
    uint32_t mag;

    if (v < 0)
    {
        dst[off++] = '-';
        mag = static_cast<uint32_t>(-static_cast<int64_t>(v));
    }
    else
    {
        mag = static_cast<uint32_t>(v);
    }

    return fmt_u64(dst, off, mag);
}

#endif //ES2025_TELEM_FMT_H
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make -C C_src/tests run`
Expected: both binaries print `ALL PASS`

- [ ] **Step 5: Commit**

```bash
git add C_src/inc/telem_fmt.h C_src/tests/test_telem_fmt.cpp
git commit -m "feat(telemetry): add integer formatters with host tests"
```

---

### Task 3: Telemetry module and emitter task

**Files:**
- Create: `C_src/inc/telemetry.h`
- Create: `C_src/src/telemetry.cpp`

**Interfaces:**
- Consumes: `TelemRing`, `telem_rec_t`, `telem_kind` from Task 1; `fmt_*` from Task 2.
- Produces:
  - `void telem_emit_header(const char* fw, uint32_t temp_ms)` — writes the session header synchronously, before the emitter task starts.
  - `uint64_t telem_now_us()` — monotonic microseconds.
  - `bool telem_push_sample(uint64_t t_us, int32_t t_cdeg, int32_t p_pa, uint8_t oss)`
  - `bool telem_push_error(uint64_t t_us, int32_t err)`
  - `rtems_task telem_emitter_task(rtems_task_argument)` — drains the ring forever.

- [ ] **Step 1: Write the header**

Create `C_src/inc/telemetry.h`:

```cpp
//
// Telemetry front-end: acquisition-side push helpers plus the emitter task that
// drains them to the console.
//
// The split exists so that the UART can never stall acquisition. Pushes are
// O(1) and non-blocking; all formatting and all blocking I/O happen in the
// lower-priority emitter task. On overflow the producer drops and counts rather
// than waiting, and the count is emitted as a 'D' record so loss is visible in
// the captured data instead of silently biasing it.
//
// Wire format is specified in B_docs/TELEMETRY_DESIGN.md and frozen at v1.
//

#ifndef ES2025_TELEMETRY_H
#define ES2025_TELEMETRY_H

#include <cstdint>

#include <rtems.h>

/**
 * @brief Emit the session header. Call once, before starting the emitter task.
 *
 * @param fw       firmware version string, e.g. "1.0.0"
 * @param temp_ms  temperature re-conversion interval in ms; 0 means every sample
 */
void telem_emit_header(const char* fw, uint32_t temp_ms);

/**
 * @brief Monotonic microseconds since boot.
 *
 * @details Derived from rtems_clock_get_uptime_nanoseconds(), which reads the
 *          hardware timecounter and therefore resolves finer than the 1 ms tick.
 */
uint64_t telem_now_us();

/**
 * @brief Queue a measurement. Non-blocking.
 *
 * @return false if the ring was full and the record was dropped.
 */
bool telem_push_sample(uint64_t t_us, int32_t t_cdeg, int32_t p_pa, uint8_t oss);

/**
 * @brief Queue an error. Non-blocking.
 *
 * @return false if the ring was full and the record was dropped.
 */
bool telem_push_error(uint64_t t_us, int32_t err);

/**
 * @brief Emitter task entry point. Drains the ring to the console forever,
 *        sleeping briefly whenever it finds the ring empty.
 */
rtems_task telem_emitter_task(rtems_task_argument ignored);

#endif //ES2025_TELEMETRY_H
```

- [ ] **Step 2: Write the implementation**

Create `C_src/src/telemetry.cpp`:

```cpp
//
// See telemetry.h for the rationale behind the producer/consumer split.
//

#include <unistd.h>

#include <rtems.h>

#include "telemetry.h"
#include "telem_fmt.h"
#include "telem_ring.h"

namespace
{
    // 128 slots x 24 bytes = ~3 KB. At the v1.0.0 ceiling of ~111 Hz (OSS0) the
    // emitter drains roughly 500 lines/s, so the ring holds over a second of
    // slack - far more than any plausible console stall.
    constexpr uint32_t TELEM_RING_SLOTS = 128;

    // "S <20> <11> <11> <1>\n" plus slack.
    constexpr size_t TELEM_LINE_MAX = 64;

    TelemRing<TELEM_RING_SLOTS> g_ring;

    /** @brief Write a fully assembled line to stdout. */
    void emit_line(const char* buf, const size_t len)
    {
        (void)write(STDOUT_FILENO, buf, len);
    }

    /** @brief Format and emit one record. */
    void emit_record(const telem_rec_t& rec)
    {
        char buf[TELEM_LINE_MAX];
        size_t n = 0;

        switch (rec.kind)
        {
        case TELEM_SAMPLE:
            n = fmt_ch(buf, n, 'S');
            n = fmt_ch(buf, n, ' ');
            n = fmt_u64(buf, n, rec.t_us);
            n = fmt_ch(buf, n, ' ');
            n = fmt_i32(buf, n, rec.t_cdeg);
            n = fmt_ch(buf, n, ' ');
            n = fmt_i32(buf, n, rec.p_pa);
            n = fmt_ch(buf, n, ' ');
            n = fmt_i32(buf, n, static_cast<int32_t>(rec.oss));
            break;

        case TELEM_ERROR:
            n = fmt_ch(buf, n, 'E');
            n = fmt_ch(buf, n, ' ');
            n = fmt_u64(buf, n, rec.t_us);
            n = fmt_ch(buf, n, ' ');
            n = fmt_i32(buf, n, rec.aux);
            break;

        case TELEM_DROP:
            n = fmt_ch(buf, n, 'D');
            n = fmt_ch(buf, n, ' ');
            n = fmt_u64(buf, n, rec.t_us);
            n = fmt_ch(buf, n, ' ');
            n = fmt_i32(buf, n, rec.aux);
            break;

        default:
            return; // unknown kind: emit nothing rather than a malformed line
        }

        n = fmt_ch(buf, n, '\n');
        emit_line(buf, n);
    }
}

uint64_t telem_now_us()
{
    return rtems_clock_get_uptime_nanoseconds() / 1000u;
}

void telem_emit_header(const char* fw, const uint32_t temp_ms)
{
    char buf[TELEM_LINE_MAX];
    size_t n = 0;

    n = fmt_str(buf, n, "#BMP180 v1 fw=");
    n = fmt_str(buf, n, fw);
    n = fmt_str(buf, n, " temp_ms=");
    n = fmt_u64(buf, n, temp_ms);
    n = fmt_ch(buf, n, '\n');

    emit_line(buf, n);
}

bool telem_push_sample(const uint64_t t_us, const int32_t t_cdeg,
                       const int32_t p_pa, const uint8_t oss)
{
    telem_rec_t rec{};
    rec.t_us = t_us;
    rec.t_cdeg = t_cdeg;
    rec.p_pa = p_pa;
    rec.oss = oss;
    rec.kind = TELEM_SAMPLE;

    return g_ring.push(rec);
}

bool telem_push_error(const uint64_t t_us, const int32_t err)
{
    telem_rec_t rec{};
    rec.t_us = t_us;
    rec.aux = err;
    rec.kind = TELEM_ERROR;

    return g_ring.push(rec);
}

rtems_task telem_emitter_task(rtems_task_argument ignored)
{
    (void)ignored;

    while (true)
    {
        telem_rec_t rec{};

        if (g_ring.pop(rec))
        {
            emit_record(rec);
            continue;
        }

        // Ring drained. Report any drops that accumulated while it was full,
        // then yield for a tick so the acquisition task runs.
        if (const uint32_t lost = g_ring.take_dropped(); lost != 0)
        {
            telem_rec_t drop{};
            drop.t_us = telem_now_us();
            drop.aux = static_cast<int32_t>(lost);
            drop.kind = TELEM_DROP;
            emit_record(drop);
        }

        rtems_task_wake_after(1);
    }
}
```

- [ ] **Step 3: Verify it compiles for the target**

Run: `cd C_src && ./compile.sh`
Expected: compiles without error. (`compile.sh` sources `../.env/setup.env`; if `TARGET_DIR` is wrong for this machine, fix that file to `/Volumes/POLI/tools` before running — see Task 6.)

- [ ] **Step 4: Confirm host tests still pass**

Run: `make -C C_src/tests run`
Expected: both binaries print `ALL PASS`

- [ ] **Step 5: Commit**

```bash
git add C_src/inc/telemetry.h C_src/src/telemetry.cpp
git commit -m "feat(telemetry): add emitter task and record formatting"
```

---

### Task 4: Emit raw samples from the acquisition task

Replaces the sweep's on-device statistics with raw record emission. The statistics move to Python, where they are easier to verify and can be recomputed without reflashing. `isqrt32` is deleted along with them.

**Files:**
- Modify: `C_src/src/sensor.cpp` (replace `isqrt32` and `bmp180_oss_sweep_task`, lines 222-371)

**Interfaces:**
- Consumes: `telem_now_us`, `telem_push_sample`, `telem_push_error`, `telem_emit_header` from Task 3.
- Produces: `rtems_task bmp180_telemetry_task(rtems_task_argument)`.

- [ ] **Step 1: Add the include**

In `C_src/src/sensor.cpp`, add to the include block at the top:

```cpp
#include "telemetry.h"
```

- [ ] **Step 2: Delete `isqrt32` and `bmp180_oss_sweep_task`**

Delete lines 222 through 371 of `C_src/src/sensor.cpp` — the `isqrt32` helper and the whole `bmp180_oss_sweep_task` function, including their doc comments. Both are superseded: the statistics they computed are now derived in Python from the raw stream.

Leave `bmp180_task` and `bmp180_task_manual` untouched. `bmp180_task_manual` is scheduled for deletion in R1, not here.

- [ ] **Step 3: Append the replacement task**

Add to the end of `C_src/src/sensor.cpp`:

```cpp
/**
 * @brief Baseline telemetry task: sweeps the four oversampling modes, then
 *        settles into a continuous run at high resolution.
 *
 * @details Emits one raw record per measurement. No statistics are computed on
 *          the device - mean, stddev, peak-to-peak and achieved rate are all
 *          derived host-side from the timestamps and values in the stream.
 *
 *          Sampling is back-to-back with no inter-sample sleep, so consecutive
 *          timestamps bound the true acquisition cycle time.
 *
 * @note The first inter-sample delta of each OSS block spans the mode change and
 *       the warm-up, so it is not a cycle time. metrics.py discards it.
 *
 * @param ignored input param to task is ignored
 */
rtems_task bmp180_telemetry_task(const rtems_task_argument ignored)
{
    (void)ignored;

    const int fd = open("/dev/bmp180-0", O_RDWR);
    if (fd < 0)
    {
        perror("open /dev/bmp180-0");
        rtems_task_delete(RTEMS_SELF);

        return;
    }

    constexpr int SWEEP_SAMPLES = 500; // per oversampling mode
    constexpr int WARMUP = 3;          // discarded settling samples

    for (int oss = 0; oss <= 3; ++oss)
    {
        bmp180_oss_t mode = static_cast<bmp180_oss_t>(oss);
        if (ioctl(fd, BMP180_IOCTL_SET_OSS, &mode) != 0)
        {
            telem_push_error(telem_now_us(), errno);
            continue;
        }

        bmp180_measurement_t m;
        for (int i = 0; i < WARMUP; ++i)
        {
            ioctl(fd, BMP180_IOCTL_READ_MEASUREMENT, &m);
        }

        for (int i = 0; i < SWEEP_SAMPLES; ++i)
        {
            if (ioctl(fd, BMP180_IOCTL_READ_MEASUREMENT, &m) != 0)
            {
                telem_push_error(telem_now_us(), errno);
                continue;
            }

            telem_push_sample(telem_now_us(), m.temperature_cdeg, m.pressure_pa,
                              static_cast<uint8_t>(oss));
        }
    }

    // Continuous run for drift, jitter and self-heating analysis.
    bmp180_oss_t hi = BMP180_OSS_HIGH_RESOLUTION;
    if (ioctl(fd, BMP180_IOCTL_SET_OSS, &hi) != 0)
    {
        telem_push_error(telem_now_us(), errno);
    }

    while (true)
    {
        bmp180_measurement_t m;

        if (ioctl(fd, BMP180_IOCTL_READ_MEASUREMENT, &m) != 0)
        {
            telem_push_error(telem_now_us(), errno);
            continue;
        }

        telem_push_sample(telem_now_us(), m.temperature_cdeg, m.pressure_pa,
                          static_cast<uint8_t>(BMP180_OSS_HIGH_RESOLUTION));
    }
}
```

- [ ] **Step 4: Add the `errno` include**

`telem_push_error` reports `errno`, so add to the include block of `C_src/src/sensor.cpp` if not already present:

```cpp
#include <cerrno>
```

- [ ] **Step 5: Verify it compiles**

Run: `cd C_src && ./compile.sh`
Expected: compiles without error.

- [ ] **Step 6: Commit**

```bash
git add C_src/src/sensor.cpp
git commit -m "feat(telemetry): emit raw samples instead of on-device statistics"
```

---

### Task 5: Wire the tasks into boot

**Files:**
- Modify: `C_src/src/init.cpp` (forward declarations at 9-12, `setupTask` at 14-44, `Entrypoint` at 46-86, configuration at 88-106)

**Interfaces:**
- Consumes: `bmp180_telemetry_task` from Task 4; `telem_emit_header`, `telem_emitter_task` from Task 3.
- Produces: nothing consumed by later tasks.

- [ ] **Step 1: Update forward declarations**

In `C_src/src/init.cpp`, replace the forward declaration block at lines 9-12 with:

```cpp
rtems_task alive_task(rtems_task_argument ignored);
rtems_task bmp180_task(rtems_task_argument ignored);
rtems_task bmp180_task_manual(rtems_task_argument ignored);
rtems_task bmp180_telemetry_task(rtems_task_argument ignored);
```

Add the telemetry include alongside the existing ones:

```cpp
#include "telemetry.h"
```

- [ ] **Step 2: Give `setupTask` an explicit stack size**

The default `RTEMS_MINIMUM_STACK_SIZE` is too tight for tasks that format and write. Replace the `setupTask` signature and its `rtems_task_create` call in `C_src/src/init.cpp`:

```cpp
template <typename TaskType>
void setupTask(rtems_id task_id, const char title[4], const int prio,
               const size_t stack_size, TaskType taskRrf)
{
    rtems_status_code task = rtems_task_create(
        rtems_build_name(title[0], title[1], title[2], title[3]),
        prio,
        stack_size,
        RTEMS_DEFAULT_MODES,
        RTEMS_DEFAULT_ATTRIBUTES,
        &task_id
    );
```

The rest of the function body is unchanged.

> The by-value `rtems_id` remains wrong (`KNOWN_ISSUES.md` B11) and is fixed in R2. It is left alone here so this phase changes nothing it does not have to.

- [ ] **Step 3: Start both tasks**

In `Entrypoint`, replace the task-startup block (currently lines 76-83, the two `constexpr rtems_id` declarations, the commented-out heartbeat and the `setupTask` call) with:

```cpp
    constexpr rtems_id sensor_task_id = 0;
    constexpr rtems_id emitter_task_id = 0;

    // Session header goes out synchronously, before any record can be emitted.
    telem_emit_header("1.0.0", 0);

    // Acquisition runs at higher priority (lower number) than emission, so the
    // console can never delay a measurement. The emitter gets the CPU during the
    // conversion sleeps, which is most of every acquisition cycle.
    setupTask(sensor_task_id, "TELE", 2, 4 * 1024, bmp180_telemetry_task);
    setupTask(emitter_task_id, "EMIT", 5, 4 * 1024, telem_emitter_task);
```

- [ ] **Step 4: Raise the task limit**

In the configuration block at the bottom of `C_src/src/init.cpp`, change:

```cpp
#define CONFIGURE_MAXIMUM_TASKS             4
```

to:

```cpp
#define CONFIGURE_MAXIMUM_TASKS             6
```

Init, acquisition and emitter is three; the headroom covers the heartbeat if it is re-enabled during fault testing.

- [ ] **Step 5: Verify it compiles**

Run: `cd C_src && ./compile.sh`
Expected: compiles without error.

- [ ] **Step 6: Commit**

```bash
git add C_src/src/init.cpp
git commit -m "feat(telemetry): start acquisition and emitter tasks at boot"
```

---

### Task 6: Fix the toolchain path and flash

The `Makefile` hardcodes another developer's path and cannot build on this machine. This is the minimum fix needed to flash — the full three-mechanism cleanup is I3 in R1.

**Files:**
- Modify: `.env/setup.env`
- Modify: `C_src/Makefile:1-2`

**Interfaces:**
- Consumes: everything from Tasks 1-5.
- Produces: a flashed board emitting telemetry.

- [ ] **Step 1: Point the environment at this machine's toolchain**

Set `.env/setup.env` to:

```bash
TARGET_DIR=/Volumes/POLI/tools
```

- [ ] **Step 2: Point the Makefile at the same place**

Replace lines 1-2 of `C_src/Makefile`:

```make
RTEMS_PREFIX = /Volumes/POLI/tools/RTEMS_toolchain/rtems/7
RTEMS_TOOL_PREFIX = /Volumes/POLI/tools/RTEMS_toolchain/rtems
```

- [ ] **Step 3: Build and flash**

Run: `cd C_src && ./flash.sh`
Expected: OpenOCD reports `** Verified OK **` and `** Resetting Target **`.

- [ ] **Step 4: Observe the stream**

Run: `screen /dev/cu.usbserial-0001 115200`, then press the black B2 reset button.

Expected output — the selftest and registration lines from v1.0.0, then the header, then records:

```
[SELFTEST] compensate: T=150 (exp 150)  P=69964 (exp 69964)  -> PASS
[[DEBUG]] BMP180 registered on /dev/bmp180-0
#BMP180 v1 fw=1.0.0 temp_ms=0
S 1043118 244 96820 0
S 1053402 244 96825 0
S 1063688 244 96818 0
```

Check three things before proceeding:

1. **Timestamps increase monotonically and the deltas are plausible** — roughly 10 000 µs at `oss=0`. If they are all zero or wildly wrong, `telem_now_us` or `fmt_u64` is broken.
2. **No `D` records during the sweep.** Any drop at these rates means the ring or the task priorities are wrong, not that the buffer is too small.
3. **`oss` climbs 0 → 1 → 2 → 3** across the sweep, then stays at 2 for the continuous run.

Exit `screen` with `Ctrl-A` then `\`.

- [ ] **Step 5: Commit**

```bash
git add .env/setup.env C_src/Makefile
git commit -m "fix(build): point toolchain paths at this machine"
```

---

### Task 7: Python parser

**Files:**
- Create: `E_analysis/requirements.txt`
- Create: `E_analysis/bmp180_analysis/__init__.py`
- Create: `E_analysis/bmp180_analysis/parse.py`
- Create: `E_analysis/tests/test_parse.py`

**Interfaces:**
- Consumes: the wire format from Task 3.
- Produces: `@dataclass Session` with fields `schema: str`, `fw: str`, `temp_ms: int`, `samples: pd.DataFrame` (columns `t_us`, `t_cdeg`, `p_pa`, `oss`), `errors: pd.DataFrame` (`t_us`, `errno`), `drops: pd.DataFrame` (`t_us`, `count`), `malformed: int`; and `parse_stream(lines: Iterable[str]) -> list[Session]`, `parse_file(path) -> list[Session]`.

- [ ] **Step 1: Write the failing test**

Create `E_analysis/tests/test_parse.py`:

```python
"""Parser tests. Synthetic streams only - no hardware required."""

from bmp180_analysis.parse import parse_stream

HEADER = "#BMP180 v1 fw=1.0.0 temp_ms=0"


def test_header_fields():
    (session,) = parse_stream([HEADER])
    assert session.schema == "v1"
    assert session.fw == "1.0.0"
    assert session.temp_ms == 0


def test_samples_parsed():
    (session,) = parse_stream([HEADER, "S 1000 244 96820 2", "S 2000 245 96825 2"])
    assert len(session.samples) == 2
    assert list(session.samples.t_us) == [1000, 2000]
    assert list(session.samples.t_cdeg) == [244, 245]
    assert list(session.samples.p_pa) == [96820, 96825]
    assert list(session.samples.oss) == [2, 2]


def test_negative_temperature():
    (session,) = parse_stream([HEADER, "S 1000 -244 96820 2"])
    assert session.samples.t_cdeg.iloc[0] == -244


def test_errors_and_drops():
    (session,) = parse_stream([HEADER, "E 1500 5", "D 1600 17"])
    assert list(session.errors.t_us) == [1500]
    assert list(session.errors.errno) == [5]
    assert list(session.drops.count) == [17]


def test_unknown_tag_ignored():
    """Stability contract: an unknown tag must not break the parse."""
    (session,) = parse_stream([HEADER, "X 1 2 3", "S 1000 244 96820 2"])
    assert len(session.samples) == 1
    assert session.malformed == 0


def test_trailing_fields_ignored():
    """Stability contract: appended fields must not break an old parser."""
    (session,) = parse_stream([HEADER, "S 1000 244 96820 2 27898 23843"])
    assert len(session.samples) == 1
    assert session.samples.p_pa.iloc[0] == 96820


def test_malformed_counted_not_fatal():
    (session,) = parse_stream([HEADER, "S 1000 notanumber 96820 2", "S 2000 244 96820 2"])
    assert len(session.samples) == 1
    assert session.malformed == 1


def test_boot_noise_before_header_skipped():
    lines = ["[SELFTEST] compensate: PASS", "[[DEBUG]] registered", HEADER, "S 1 2 3 0"]
    (session,) = parse_stream(lines)
    assert len(session.samples) == 1


def test_reset_starts_new_session():
    lines = [HEADER, "S 1000 244 96820 2", HEADER, "S 10 244 96830 2"]
    first, second = parse_stream(lines)
    assert len(first.samples) == 1
    assert len(second.samples) == 1
    assert second.samples.t_us.iloc[0] == 10


def test_empty_stream_yields_no_sessions():
    assert parse_stream([]) == []
```

Create `E_analysis/requirements.txt`:

```
pandas>=2.0
matplotlib>=3.7
pytest>=7.4
```

Create `E_analysis/bmp180_analysis/__init__.py`:

```python
"""Analysis tooling for the RTEMS BMP180 telemetry stream."""

__version__ = "1.0.0"
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd E_analysis && python3 -m pytest tests/test_parse.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'bmp180_analysis.parse'`

- [ ] **Step 3: Write minimal implementation**

Create `E_analysis/bmp180_analysis/parse.py`:

```python
"""Parse the BMP180 telemetry wire format into DataFrames.

Format is specified in B_docs/TELEMETRY_DESIGN.md and frozen at v1:

    #BMP180 v1 fw=1.0.0 temp_ms=0
    S <t_us> <t_cdeg> <p_pa> <oss>
    E <t_us> <errno>
    D <t_us> <count>

The stability contract this module must honour:
  1. A tag never changes meaning.
  2. New fields append only to the end of a line.
  3. Unknown tags and unknown trailing fields are ignored, not errors.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

import pandas as pd

SAMPLE_COLUMNS = ["t_us", "t_cdeg", "p_pa", "oss"]
ERROR_COLUMNS = ["t_us", "errno"]
DROP_COLUMNS = ["t_us", "count"]


@dataclass
class Session:
    """One boot-to-reset run of the device."""

    schema: str = ""
    fw: str = ""
    temp_ms: int = 0
    samples: pd.DataFrame = field(
        default_factory=lambda: pd.DataFrame(columns=SAMPLE_COLUMNS)
    )
    errors: pd.DataFrame = field(
        default_factory=lambda: pd.DataFrame(columns=ERROR_COLUMNS)
    )
    drops: pd.DataFrame = field(
        default_factory=lambda: pd.DataFrame(columns=DROP_COLUMNS)
    )
    malformed: int = 0


class _Accumulator:
    """Collects rows for one session before they are frozen into DataFrames."""

    def __init__(self, schema: str, fw: str, temp_ms: int) -> None:
        self.schema = schema
        self.fw = fw
        self.temp_ms = temp_ms
        self.samples: list[tuple[int, int, int, int]] = []
        self.errors: list[tuple[int, int]] = []
        self.drops: list[tuple[int, int]] = []
        self.malformed = 0

    def finish(self) -> Session:
        return Session(
            schema=self.schema,
            fw=self.fw,
            temp_ms=self.temp_ms,
            samples=pd.DataFrame(self.samples, columns=SAMPLE_COLUMNS),
            errors=pd.DataFrame(self.errors, columns=ERROR_COLUMNS),
            drops=pd.DataFrame(self.drops, columns=DROP_COLUMNS),
            malformed=self.malformed,
        )


def _parse_header(line: str) -> tuple[str, str, int] | None:
    """Return (schema, fw, temp_ms) or None if this is not a header line."""
    parts = line.split()
    if not parts or parts[0] != "#BMP180":
        return None

    schema = parts[1] if len(parts) > 1 else ""
    fw = ""
    temp_ms = 0
    for token in parts[2:]:
        key, _, value = token.partition("=")
        if key == "fw":
            fw = value
        elif key == "temp_ms":
            try:
                temp_ms = int(value)
            except ValueError:
                temp_ms = 0
    return schema, fw, temp_ms


def parse_stream(lines: Iterable[str]) -> list[Session]:
    """Parse a telemetry stream into one Session per device boot."""
    sessions: list[Session] = []
    current: _Accumulator | None = None

    for raw in lines:
        line = raw.strip()
        if not line:
            continue

        header = _parse_header(line)
        if header is not None:
            if current is not None:
                sessions.append(current.finish())
            current = _Accumulator(*header)
            continue

        # Anything before the first header is boot noise from the BSP.
        if current is None:
            continue

        parts = line.split()
        tag = parts[0]

        # Rule 3: unknown tags are ignored, not counted as malformed.
        if tag not in ("S", "E", "D"):
            continue

        try:
            # Rule 2: read only the fields this version knows; ignore the rest.
            if tag == "S":
                current.samples.append(
                    (int(parts[1]), int(parts[2]), int(parts[3]), int(parts[4]))
                )
            elif tag == "E":
                current.errors.append((int(parts[1]), int(parts[2])))
            else:
                current.drops.append((int(parts[1]), int(parts[2])))
        except (ValueError, IndexError):
            current.malformed += 1

    if current is not None:
        sessions.append(current.finish())

    return sessions


def parse_file(path: str | Path) -> list[Session]:
    """Parse a captured telemetry file."""
    with open(path, "r", errors="replace") as handle:
        return parse_stream(handle)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd E_analysis && python3 -m pytest tests/test_parse.py -v`
Expected: 10 passed

- [ ] **Step 5: Commit**

```bash
git add E_analysis/requirements.txt E_analysis/bmp180_analysis/ E_analysis/tests/test_parse.py
git commit -m "feat(analysis): add telemetry stream parser"
```

---

### Task 8: Metrics

**Files:**
- Create: `E_analysis/bmp180_analysis/metrics.py`
- Create: `E_analysis/tests/test_metrics.py`

**Interfaces:**
- Consumes: `Session` from Task 7.
- Produces: `intervals_us(session, oss) -> pd.Series`, `read_freq_hz(session, oss) -> float`, `jitter_us(session, oss) -> dict`, `pressure_noise(session, oss) -> dict`, `altitude_m(p_pa, p0_pa=101325.0) -> float`, `DATASHEET_RMS_PA: dict[int, float]`, `per_oss_summary(session) -> pd.DataFrame`, `temperature_drift(session) -> dict`.

- [ ] **Step 1: Write the failing test**

Create `E_analysis/tests/test_metrics.py`:

```python
"""Metric tests. Synthetic sessions with known statistics."""

import math

import pytest

from bmp180_analysis import metrics
from bmp180_analysis.parse import parse_stream

HEADER = "#BMP180 v1 fw=1.0.0 temp_ms=0"


def build(rows):
    return parse_stream([HEADER] + rows)[0]


def test_intervals_drop_first_of_block():
    """The first delta of an OSS block spans the mode change, not a cycle."""
    session = build(
        [
            "S 1000 244 96820 0",
            "S 11000 244 96820 0",
            "S 21000 244 96820 0",
        ]
    )
    intervals = metrics.intervals_us(session, oss=0)
    # 3 samples -> 2 deltas, both 10000 us; nothing is discarded within a block.
    assert list(intervals) == [10000, 10000]


def test_intervals_exclude_cross_mode_gap():
    session = build(
        [
            "S 1000 244 96820 0",
            "S 11000 244 96820 0",
            "S 500000 244 96820 1",  # huge gap: mode change + warmup
            "S 515000 244 96820 1",
        ]
    )
    assert list(metrics.intervals_us(session, oss=0)) == [10000]
    assert list(metrics.intervals_us(session, oss=1)) == [15000]


def test_read_freq_hz():
    session = build(["S 0 244 96820 0", "S 10000 244 96820 0", "S 20000 244 96820 0"])
    assert metrics.read_freq_hz(session, oss=0) == pytest.approx(100.0)


def test_read_freq_uses_median_not_mean():
    """One stalled sample must not drag the reported rate down."""
    session = build(
        [
            "S 0 244 96820 0",
            "S 10000 244 96820 0",
            "S 20000 244 96820 0",
            "S 500000 244 96820 0",  # single long stall
        ]
    )
    assert metrics.read_freq_hz(session, oss=0) == pytest.approx(100.0)


def test_pressure_noise_known_values():
    # Population stddev of [10, 12, 10, 12] is 1.0; peak-to-peak is 2.
    session = build(
        [
            "S 0 244 10 0",
            "S 10000 244 12 0",
            "S 20000 244 10 0",
            "S 30000 244 12 0",
        ]
    )
    noise = metrics.pressure_noise(session, oss=0)
    assert noise["rms_pa"] == pytest.approx(1.0)
    assert noise["p2p_pa"] == 2
    assert noise["mean_pa"] == pytest.approx(11.0)


def test_datasheet_reference_values():
    """BST-BMP180-DS000-09 Table 3, converted from hPa to Pa."""
    assert metrics.DATASHEET_RMS_PA == {0: 6.0, 1: 5.0, 2: 4.0, 3: 3.0}


def test_altitude_at_sea_level_is_zero():
    assert metrics.altitude_m(101325.0) == pytest.approx(0.0, abs=1e-6)


def test_altitude_matches_datasheet_gradient():
    """Datasheet 3.6: 1 hPa corresponds to ~8.43 m at sea level."""
    delta = metrics.altitude_m(101325.0 - 100.0) - metrics.altitude_m(101325.0)
    assert delta == pytest.approx(8.43, abs=0.05)


def test_temperature_drift_detects_ramp():
    rows = [f"S {i * 1_000_000} {200 + i} 96820 2" for i in range(11)]
    drift = metrics.temperature_drift(build(rows))
    # +1 unit of 0.1 degC per second -> 0.1 degC/s -> 6.0 degC/min
    assert drift["cdeg_per_min"] == pytest.approx(600.0, rel=1e-3)


def test_per_oss_summary_shape():
    rows = []
    for oss in range(4):
        for i in range(5):
            rows.append(f"S {oss * 1_000_000 + i * 10000} 244 96820 {oss}")
    summary = metrics.per_oss_summary(build(rows))
    assert list(summary.oss) == [0, 1, 2, 3]
    assert "rms_pa" in summary.columns
    assert "read_freq_hz" in summary.columns
    assert "datasheet_rms_pa" in summary.columns


def test_empty_oss_block_is_safe():
    session = build(["S 0 244 96820 0"])
    assert math.isnan(metrics.read_freq_hz(session, oss=3))
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd E_analysis && python3 -m pytest tests/test_metrics.py -v`
Expected: FAIL with `ImportError: cannot import name 'metrics'`

- [ ] **Step 3: Write minimal implementation**

Create `E_analysis/bmp180_analysis/metrics.py`:

```python
"""Derive metrics from a parsed telemetry Session.

Pure computation: no file I/O, no plotting. Everything here is testable against
synthetic sessions without hardware.
"""

from __future__ import annotations

import math

import numpy as np
import pandas as pd

from .parse import Session

# BST-BMP180-DS000-09 Table 3, typical RMS pressure noise per oversampling
# mode, converted from hPa to Pa. Measured noise should land near these.
DATASHEET_RMS_PA: dict[int, float] = {0: 6.0, 1: 5.0, 2: 4.0, 3: 3.0}

# Datasheet 3.6 international barometric formula constants.
_ALT_SCALE_M = 44330.0
_ALT_EXPONENT = 1.0 / 5.255
_SEA_LEVEL_PA = 101325.0


def _block(session: Session, oss: int) -> pd.DataFrame:
    """Samples for one oversampling mode, in acquisition order."""
    if session.samples.empty:
        return session.samples
    return session.samples[session.samples.oss == oss]


def intervals_us(session: Session, oss: int) -> pd.Series:
    """Inter-sample intervals within one OSS block.

    Deltas are taken only within the block, so the gap spanning the mode change
    and its warm-up samples never appears - that gap is not a cycle time.
    """
    block = _block(session, oss)
    if len(block) < 2:
        return pd.Series(dtype="int64")
    return block.t_us.diff().dropna().astype("int64")


def read_freq_hz(session: Session, oss: int) -> float:
    """Achieved sample rate, from the median interval.

    Median rather than mean: a single console stall or scheduling hiccup should
    not move the reported rate, and the median is what characterises the
    sustained cycle time.
    """
    intervals = intervals_us(session, oss)
    if intervals.empty:
        return math.nan
    median = float(intervals.median())
    return 1_000_000.0 / median if median > 0 else math.nan


def jitter_us(session: Session, oss: int) -> dict:
    """Spread of the inter-sample interval."""
    intervals = intervals_us(session, oss)
    if intervals.empty:
        return {"std_us": math.nan, "p95_us": math.nan, "max_us": math.nan}
    return {
        "std_us": float(intervals.std(ddof=0)),
        "p95_us": float(intervals.quantile(0.95)),
        "max_us": float(intervals.max()),
    }


def pressure_noise(session: Session, oss: int) -> dict:
    """Mean, RMS deviation and peak-to-peak pressure for one OSS block."""
    block = _block(session, oss)
    if block.empty:
        return {"mean_pa": math.nan, "rms_pa": math.nan, "p2p_pa": math.nan, "n": 0}
    return {
        "mean_pa": float(block.p_pa.mean()),
        "rms_pa": float(block.p_pa.std(ddof=0)),
        "p2p_pa": int(block.p_pa.max() - block.p_pa.min()),
        "n": int(len(block)),
    }


def altitude_m(p_pa: float, p0_pa: float = _SEA_LEVEL_PA) -> float:
    """Altitude above the reference pressure.

    Datasheet 3.6: altitude = 44330 * (1 - (p / p0) ^ (1 / 5.255))
    """
    return _ALT_SCALE_M * (1.0 - (p_pa / p0_pa) ** _ALT_EXPONENT)


def temperature_drift(session: Session) -> dict:
    """Least-squares slope of temperature against time.

    A positive slope during a sustained high-rate run is the signature of die
    self-heating rather than a change in ambient temperature.
    """
    samples = session.samples
    if len(samples) < 2:
        return {"cdeg_per_min": math.nan, "span_cdeg": math.nan}

    seconds = (samples.t_us - samples.t_us.iloc[0]) / 1_000_000.0
    slope = float(np.polyfit(seconds, samples.t_cdeg.astype(float), 1)[0])

    return {
        "cdeg_per_min": slope * 60.0,
        "span_cdeg": int(samples.t_cdeg.max() - samples.t_cdeg.min()),
    }


def per_oss_summary(session: Session) -> pd.DataFrame:
    """One row per oversampling mode present in the session."""
    rows = []
    for oss in sorted(session.samples.oss.unique()) if not session.samples.empty else []:
        noise = pressure_noise(session, int(oss))
        jitter = jitter_us(session, int(oss))
        rows.append(
            {
                "oss": int(oss),
                "n": noise["n"],
                "mean_pa": noise["mean_pa"],
                "rms_pa": noise["rms_pa"],
                "p2p_pa": noise["p2p_pa"],
                "datasheet_rms_pa": DATASHEET_RMS_PA.get(int(oss), math.nan),
                "read_freq_hz": read_freq_hz(session, int(oss)),
                "median_interval_us": float(intervals_us(session, int(oss)).median())
                if not intervals_us(session, int(oss)).empty
                else math.nan,
                "jitter_std_us": jitter["std_us"],
            }
        )
    return pd.DataFrame(rows)
```

Add `numpy>=1.24` to `E_analysis/requirements.txt`.

- [ ] **Step 4: Run test to verify it passes**

Run: `cd E_analysis && python3 -m pytest tests/ -v`
Expected: all tests pass (10 parse + 11 metrics)

- [ ] **Step 5: Commit**

```bash
git add E_analysis/bmp180_analysis/metrics.py E_analysis/tests/test_metrics.py E_analysis/requirements.txt
git commit -m "feat(analysis): add metrics derivation"
```

---

### Task 9: Plots and capture tooling

**Files:**
- Create: `E_analysis/bmp180_analysis/plot.py`
- Create: `E_analysis/capture.sh`
- Create: `E_analysis/README.md`

**Interfaces:**
- Consumes: `Session` from Task 7; all of `metrics` from Task 8.
- Produces: `plot_noise_vs_oss(session, ax=None)`, `plot_pressure_series(session, ax=None)`, `plot_interval_hist(session, oss, ax=None)`, `plot_temperature(session, ax=None)`, `report(session, out_dir)`.

- [ ] **Step 1: Write the plotting module**

Create `E_analysis/bmp180_analysis/plot.py`:

```python
"""Charts for a parsed telemetry Session.

Plotting only: every number shown here comes from metrics.py, so the figures can
be checked without rendering anything.
"""

from __future__ import annotations

from pathlib import Path

import matplotlib

matplotlib.use("Agg")  # file output; no display needed
import matplotlib.pyplot as plt

from . import metrics
from .parse import Session


def plot_noise_vs_oss(session: Session, ax=None):
    """Measured RMS pressure noise per mode against the datasheet reference."""
    if ax is None:
        _, ax = plt.subplots(figsize=(6, 4))

    summary = metrics.per_oss_summary(session)
    ax.bar(summary.oss - 0.2, summary.rms_pa, width=0.4, label="measured")
    ax.bar(summary.oss + 0.2, summary.datasheet_rms_pa, width=0.4,
           label="datasheet typ.")
    ax.set_xlabel("oversampling setting")
    ax.set_ylabel("RMS pressure noise [Pa]")
    ax.set_xticks(summary.oss)
    ax.set_title("Pressure noise vs oversampling")
    ax.legend()
    return ax


def plot_pressure_series(session: Session, ax=None):
    """Pressure against time, coloured by oversampling mode."""
    if ax is None:
        _, ax = plt.subplots(figsize=(10, 4))

    samples = session.samples
    seconds = (samples.t_us - samples.t_us.iloc[0]) / 1_000_000.0
    for oss in sorted(samples.oss.unique()):
        mask = samples.oss == oss
        ax.plot(seconds[mask], samples.p_pa[mask], ".", markersize=2,
                label=f"oss={oss}")

    ax.set_xlabel("time since first sample [s]")
    ax.set_ylabel("pressure [Pa]")
    ax.set_title("Pressure")
    ax.legend()
    return ax


def plot_interval_hist(session: Session, oss: int, ax=None):
    """Distribution of inter-sample interval for one mode."""
    if ax is None:
        _, ax = plt.subplots(figsize=(6, 4))

    intervals = metrics.intervals_us(session, oss) / 1000.0
    ax.hist(intervals, bins=50)
    ax.set_xlabel("inter-sample interval [ms]")
    ax.set_ylabel("count")
    ax.set_title(f"Acquisition interval, oss={oss}")
    return ax


def plot_temperature(session: Session, ax=None):
    """Temperature against time - the self-heating check."""
    if ax is None:
        _, ax = plt.subplots(figsize=(10, 3))

    samples = session.samples
    seconds = (samples.t_us - samples.t_us.iloc[0]) / 1_000_000.0
    ax.plot(seconds, samples.t_cdeg / 10.0, ".", markersize=2)
    ax.set_xlabel("time since first sample [s]")
    ax.set_ylabel("temperature [degC]")

    drift = metrics.temperature_drift(session)
    ax.set_title(f"Temperature (drift {drift['cdeg_per_min'] / 10.0:+.3f} degC/min)")
    return ax


def report(session: Session, out_dir: str | Path) -> list[Path]:
    """Write every chart to out_dir and return the paths written."""
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []

    for name, fn in (
        ("noise_vs_oss.png", plot_noise_vs_oss),
        ("pressure_series.png", plot_pressure_series),
        ("temperature.png", plot_temperature),
    ):
        fig, ax = plt.subplots(figsize=(10, 4))
        fn(session, ax=ax)
        fig.tight_layout()
        path = out / name
        fig.savefig(path, dpi=120)
        plt.close(fig)
        written.append(path)

    for oss in sorted(session.samples.oss.unique()):
        fig, ax = plt.subplots(figsize=(6, 4))
        plot_interval_hist(session, int(oss), ax=ax)
        fig.tight_layout()
        path = out / f"interval_oss{int(oss)}.png"
        fig.savefig(path, dpi=120)
        plt.close(fig)
        written.append(path)

    return written
```

- [ ] **Step 2: Write the capture script**

Create `E_analysis/capture.sh`:

```bash
#!/bin/bash
# Capture a telemetry run to a timestamped file.
#
# Usage: ./capture.sh [seconds] [device]
# Reset the board (black B2 button) right after this starts.

set -e

DURATION="${1:-120}"
DEVICE="${2:-/dev/cu.usbserial-0001}"
OUT_DIR="$(dirname "$0")/captures"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="$OUT_DIR/$STAMP.log"

mkdir -p "$OUT_DIR"

if [ ! -e "$DEVICE" ]; then
    echo "No such device: $DEVICE" >&2
    echo "Available:" >&2
    ls /dev/cu.* >&2
    exit 1
fi

# The device has no RTC, so record the host wall-clock start alongside the
# capture. Device timestamps are monotonic microseconds since ITS boot; absolute
# time is this value plus the device timestamp.
echo "# capture_start_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$OUT"

stty -f "$DEVICE" 115200 cs8 -cstopb -parenb raw

echo "Capturing $DURATION s from $DEVICE -> $OUT"
echo "Press the black B2 reset button now."

timeout "$DURATION" cat "$DEVICE" >> "$OUT" || true

echo "Done. $(grep -c '^S ' "$OUT" || true) samples captured."
echo "$OUT"
```

Make it executable: `chmod +x E_analysis/capture.sh`

- [ ] **Step 3: Write the README**

Create `E_analysis/README.md`:

```markdown
# BMP180 telemetry analysis

Host-side tooling for the measurement stream defined in
[`../B_docs/TELEMETRY_DESIGN.md`](../B_docs/TELEMETRY_DESIGN.md).

## Install

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

## Capture

```bash
./capture.sh 300                      # 5 minutes from the default device
./capture.sh 300 /dev/cu.usbserial-XXXX
```

Press the black B2 reset button right after the script starts, so the session
header is captured. Output lands in `captures/YYYYmmdd-HHMMSS.log`.

The device has no RTC. Timestamps in the stream are monotonic microseconds since
the device booted; the capture file records the host wall-clock start on its
first line so absolute time can be reconstructed.

## Analyse

```python
from bmp180_analysis.parse import parse_file
from bmp180_analysis import metrics, plot

sessions = parse_file("captures/20260803-190000.log")
session = sessions[-1]          # last boot in the capture

print(metrics.per_oss_summary(session))
print(metrics.temperature_drift(session))

plot.report(session, "figures/")
```

## Layout

| Module | Responsibility |
|--------|----------------|
| `parse.py` | Text stream to DataFrames. Parsing only. |
| `metrics.py` | DataFrames to numbers. No I/O, no plotting. |
| `plot.py` | Numbers to charts. No parsing, no computation. |

Split this way so metrics are testable without hardware or a display.

## Tests

```bash
python3 -m pytest tests/ -v
```

All tests run against synthetic streams; none need a board.
```

- [ ] **Step 4: Verify plotting works end to end**

Run:

```bash
cd E_analysis && python3 -c "
from bmp180_analysis.parse import parse_stream
from bmp180_analysis import plot
rows = ['#BMP180 v1 fw=1.0.0 temp_ms=0']
for oss in range(4):
    for i in range(50):
        rows.append(f'S {oss*1000000 + i*10000} {244+i//10} {96820 + (i%5)} {oss}')
s = parse_stream(rows)[0]
print(plot.report(s, '/tmp/figtest'))
"
```

Expected: prints seven `.png` paths and exits cleanly.

- [ ] **Step 5: Commit**

```bash
git add E_analysis/bmp180_analysis/plot.py E_analysis/capture.sh E_analysis/README.md
git commit -m "feat(analysis): add plotting, capture script and docs"
```

---

### Task 10: Capture the v1.0.0 baseline

The deliverable of this whole phase. Everything before it exists to make this dataset trustworthy.

**Files:**
- Create: `E_analysis/captures/` (data, committed)
- Create: `E_analysis/BASELINE.md`

**Interfaces:**
- Consumes: everything.
- Produces: the reference dataset for R1-R4.

- [ ] **Step 1: Capture the sweep run**

The sweep is 4 modes x 500 samples; at v1.0.0 cycle times that is roughly
`500 * (10 + 13 + 19 + 31) ms ≈ 37 s`. Capture generously past it:

```bash
cd E_analysis && ./capture.sh 120
```

Press B2 immediately. Confirm the file starts with `#BMP180 v1 fw=1.0.0 temp_ms=0`.

- [ ] **Step 2: Capture the long run**

```bash
cd E_analysis && ./capture.sh 900
```

Press B2 immediately. This run's value is in the continuous tail after the
sweep — drift, jitter stability and self-heating.

- [ ] **Step 3: Sanity-check both captures**

Run:

```bash
cd E_analysis && python3 -c "
import sys, glob
from bmp180_analysis.parse import parse_file
from bmp180_analysis import metrics
for path in sorted(glob.glob('captures/*.log')):
    for s in parse_file(path):
        print(path, 'fw=', s.fw, 'temp_ms=', s.temp_ms, 'malformed=', s.malformed)
        print(metrics.per_oss_summary(s).to_string(index=False))
        print('drops:', int(s.drops['count'].sum()) if len(s.drops) else 0,
              'errors:', len(s.errors))
        print(metrics.temperature_drift(s))
        print()
"
```

Check:

1. `malformed` is 0. Anything else means the emitter is producing bad lines or
   the capture dropped bytes.
2. `drops` is 0. A nonzero count means the ring or the priorities are wrong —
   fix that before trusting any timing number.
3. `read_freq_hz` falls as `oss` rises, and the median intervals are near
   10 / 13 / 19 / 31 ms.
4. `rms_pa` is in the same neighbourhood as `datasheet_rms_pa` (6 / 5 / 4 / 3).

> **Expected anomaly, do not "fix" it:** `p2p_pa` may not fall cleanly as `oss`
> rises. That is the signature of
> [B1](../../../KNOWN_ISSUES.md#b1--conversion-wait-can-expire-before-the-conversion-finishes-live),
> the conversion-wait off-by-one that v1.0.0 has. Capturing it is the point of
> this phase — R2 is judged by whether it removes it.

- [ ] **Step 4: Generate the figures**

```bash
cd E_analysis && python3 -c "
import glob
from bmp180_analysis.parse import parse_file
from bmp180_analysis import plot
path = sorted(glob.glob('captures/*.log'))[-1]
plot.report(parse_file(path)[-1], 'figures/baseline')
print('written to figures/baseline')
"
```

- [ ] **Step 5: Record the baseline**

Create `E_analysis/BASELINE.md` containing, for each capture: the filename, the
host wall-clock start, ambient conditions (room temperature, roughly stable or
not), the `per_oss_summary` table pasted verbatim, drop and error counts, and
the temperature drift figure. This is the document R2 is compared against, so
the conditions matter as much as the numbers.

- [ ] **Step 6: Commit**

```bash
git add E_analysis/captures/ E_analysis/BASELINE.md E_analysis/figures/
git commit -m "data: capture v1.0.0 telemetry baseline"
```

---

## Self-Review

**Spec coverage** — every section of `B_docs/TELEMETRY_DESIGN.md` maps to a task:

| Spec section | Task |
|--------------|------|
| §1 wire format, stability contract | 3 (emitter), 7 (parser honours it) |
| §1 timestamps via `uptime_nanoseconds` | 3 (`telem_now_us`) |
| §2.2 ring buffer, emitter, drop counting | 1, 3 |
| §2.3 task wiring, stacks, task count | 5 |
| §3 `parse.py` / `metrics.py` / `plot.py` split | 7, 8, 9 |
| §3 `read_freq`, jitter, RMS vs Table 3, altitude, drift, loss | 8 |
| §3 capture via serial | 9 (`capture.sh`) |

Deliberately **not** covered, per the Global Constraints: §2.1 temperature decoupling and the B1 dependency in §2.4. Those are the updates that follow this baseline.

**Type consistency** — `telem_rec_t` field names (`t_us`, `t_cdeg`, `p_pa`, `aux`, `oss`, `kind`) are identical in Tasks 1, 3 and 4. The DataFrame columns in Task 7 (`t_us`, `t_cdeg`, `p_pa`, `oss`) match what Task 8 indexes. `per_oss_summary` emits `rms_pa`, `read_freq_hz` and `datasheet_rms_pa`, which are exactly the columns Task 9's `plot_noise_vs_oss` reads and Task 8's test asserts on.

**Known gap:** the ring buffer's host tests are single-threaded, so they verify the index arithmetic, FIFO order and drop accounting but not the concurrent producer/consumer interleaving. That interleaving is exercised on hardware in Task 6 Step 4, where a nonzero `D` count is the failure signal. A true concurrency test would need a threaded harness; given a single-core target where the only interleaving is task preemption at known points, the index tests plus the hardware drop counter are proportionate.

---

## Execution Handoff

Plan complete and saved to `ES2025/docs/superpowers/plans/2026-08-03-phase0-telemetry-baseline.md`.
