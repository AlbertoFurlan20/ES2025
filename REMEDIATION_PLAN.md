# Remediation Plan — v1.1.0 → v1.2.0

Schedule for working off [KNOWN_ISSUES.md](KNOWN_ISSUES.md). Four rounds, each
with an explicit verification gate that must pass before the next round starts.

**Phase 0 shipped as v1.1.0** — instrumentation only, measurement path untouched.
The rounds below are the first to change measured behaviour, so they land as
`1.2.0`.

**Decisions taken:**

- **B1 is fixed by padding the conversion constants**, not by SCO polling. SCO
  polling (I5) stays a separate change in R3, so the timing fix and the
  restructure can be bisected independently.
- **Hardware is available**, so every round ends on a real flash-and-read gate,
  not just a compile.

**Release target:** `1.2.0` — new behaviour (bus recovery, atomic measurement),
no breaking API change. [I9](KNOWN_ISSUES.md#i9--read_measurement-is-declared-_iow-but-writes-back-to-the-caller)
changes ioctl command numbers and
[I17b](KNOWN_ISSUES.md#i17b--temperature_cdeg-is-a-misnomer) renames a field in
the ioctl payload struct; both are breaking, both are deferred out of this plan
entirely, and both belong to a `2.0.0` batched together.

---

## ✅ Phase 0 — Instrumented baseline — COMPLETE (v1.1.0, 2026-08-04)

Superseded the original "capture the console text" step. Rather than eyeballing
four summary rows, v1.0.0 was instrumented with the structured telemetry stream
and a real dataset captured, so every later gate is a numerical comparison
instead of a judgement call.

**Plan:** [`docs/superpowers/plans/2026-08-03-phase0-telemetry-baseline.md`](docs/superpowers/plans/2026-08-03-phase0-telemetry-baseline.md)
**Design:** [`B_docs/TELEMETRY_DESIGN.md`](B_docs/TELEMETRY_DESIGN.md)
**Result:** [`E_analysis/BASELINE.md`](E_analysis/BASELINE.md)

The measurement path was untouched — only reporting was replaced.

**Outcome:** two 150 s captures, 7573 and 7576 samples, **zero drops, zero
errors, zero malformed lines**. Emission never throttled acquisition, which is
what makes the timing figures trustworthy.

**[B1](KNOWN_ISSUES.md#b1--conversion-wait-can-expire-before-the-conversion-finishes-live)
confirmed.** Noise fails to fall monotonically with oversampling in both runs, at
*different* transitions — run 1 across 0→1, run 2 across 1→2 — which is what a
stochastic stale-sample effect predicts and a fixed cause would not produce.
Jitter below 0.5 µs in every mode rules out the scheduler.

Phase 0 also absorbed [I11](KNOWN_ISSUES.md#i11--resolved-in-v110--sweep-task-ran-on-rtems_minimum_stack_size)
(explicit task stacks) and part of
[I18](KNOWN_ISSUES.md#i18--dead-code-carried-in-the-build), and surfaced two new
issues, I17b and I17c.

> All R2/R3 gates below are now evaluated with `metrics.per_oss_summary()`
> against the recorded baseline, not by reading a printed table.

### Reference numbers

```
 oss    rms_pa  p2p_pa  datasheet_rms_pa  read_freq_hz  median_interval_us  jitter_std_us
   0  5.63      33      6.0               90.9          10999               0.468
   1  4.68      27      5.0               71.4          13999               0.332
   2  4.93      32      4.0               50.0          19999               0.433
   3  3.96      23      3.0               31.3          31998               0.000
```

---

## R1 — Build safety net ✅ COMPLETE (v1.2.0, 2026-08-05)

Nothing here changes runtime behaviour. It exists so the rest of the plan is
verifiable at all.

| Item | Change | Status |
|------|--------|--------|
| [I3](KNOWN_ISSUES.md#i3--three-divergent-toolchain-path-mechanisms) | Collapse the toolchain-path mechanisms onto `local.cmake`. | ✅ All of `CMakeLists.txt`, `Makefile`, `compile.sh` and `flash.sh` now read `RTEMS_LOCAL_PATH` from `local.cmake`, environment-overridable. `.env/setup.env` no longer consulted. |
| [I17c](KNOWN_ISSUES.md#i17c--target-build-does-not-pin-the-c-standard) | Pin `-std=c++17` in `compile.sh` and `C_src/Makefile`. | ✅ |
| [I4](KNOWN_ISSUES.md#i4--no-warning-flags-anywhere-in-the-build) | Add `-Wall -Wextra` to all build paths. Not `-Werror` yet. | ✅ |
| [I18](KNOWN_ISSUES.md#i18--dead-code-carried-in-the-build) | Delete `bmp180_task_manual`. **Highest-value single edit in the plan.** | ✅ Went further: `bmp180_task` and `alive_task` deleted too. Closes B2, B3, B4, B5, B10, B14 **and B6**. |
| [B7](KNOWN_ISSUES.md#b7--registration-failure-leaves-a-silently-dead-board-live) | Added to the round: deleting `alive_task` would have made registration failure fully silent. | ✅ `E <t_us> 19` (`ENODEV`) now pushed before the tasks start. |
| [I7](KNOWN_ISSUES.md#i7--c-exceptions-used-to-signal-ordinary-error-codes) | Side effect, as predicted in the note below. | ✅ `-fno-exceptions -fno-rtti` on all paths; 520 bytes of `.text` at `-O0`. |

**Gate:** clean build from a fresh checkout with only `local.cmake` supplied.
Triage the new warning list — it should surface
[B11](KNOWN_ISSUES.md#b11--setuptask-takes-rtems_id-by-value-live) and
[I2](KNOWN_ISSUES.md#i2--seven-functions-declared-static-in-a-shared-header)
on its own.

**Gate result.** Build clean via both `make` and `compile.sh`, byte-identical
output (2 774 692 bytes). Warning list: one `-Wunused-parameter` in `Entrypoint`
(fixed on the spot) and 7 unique instances of I2, which R3 addresses. **B11 did
not surface** — passing `rtems_id` by value is legal and no warning class covers
it, so that prediction was wrong and B11 still needs its own fix. Host suites
green: `test_telem_ring`/`test_telem_fmt` ALL PASS, 24 Python tests pass.

**Outstanding: the post-R1 capture.** Flash and capture, then confirm
`metrics.per_block_summary()` still matches the Phase 0 reference numbers above —
R1 changes no runtime behaviour, so any movement beyond run-to-run noise means
something was broken, not fixed. Not taken yet because the board was not
connected. **Do this before starting R2**, so R2's comparison has an
immediately-preceding reference rather than one from two releases back.

The one thing to expect: the stream header now reads `fw=1.2.0`, and a board
whose sensor fails to register emits `E <t_us> 19` where v1.1.0 emitted nothing.

**Note, now settled:** deleting `bmp180_task_manual` removed the only consumer of
`bmp180_load_calibration` as a public entry point and the only `<stdexcept>`
user, so [I7](KNOWN_ISSUES.md#i7--c-exceptions-used-to-signal-ordinary-error-codes)
did disappear as a side effect. `-fno-exceptions -fno-rtti` are set on every
build path; the opportunity was taken, not missed.

---

## R2 — Live correctness

The bugs that are on the boot path today. Small, localised, individually
revertable.

### B1 — conversion timing

`C_src/inc/bmp_regs.h:40-44`

```c
#define BMP180_CONV_TIME_TEMP_MS        6u  // was 5
#define BMP180_CONV_TIME_PRESS_OSS0_MS  6u  // was 5
#define BMP180_CONV_TIME_PRESS_OSS1_MS  9u  // was 8
#define BMP180_CONV_TIME_PRESS_OSS2_MS 15u  // was 14
#define BMP180_CONV_TIME_PRESS_OSS3_MS 27u  // was 26
```

Each value is now `datasheet_max + 1` tick, so the worst case (`n-1` ticks)
still clears the datasheet maximum:

| Mode | Datasheet max | Ticks | Worst case | Margin |
|------|---------------|-------|------------|--------|
| Temperature | 4.5 ms | 6 | 5.0 ms | +0.5 |
| OSS0 | 4.5 ms | 6 | 5.0 ms | +0.5 |
| OSS1 | 7.5 ms | 9 | 8.0 ms | +0.5 |
| OSS2 | 13.5 ms | 15 | 14.0 ms | +0.5 |
| OSS3 | 25.5 ms | 27 | 26.0 ms | +0.5 |

Add a comment at the constants recording *why* they exceed the datasheet — the
next reader will otherwise "correct" them back.

### Remaining R2 items

| Item | Change |
|------|--------|
| [B8](KNOWN_ISSUES.md#b8--oss-is-not-validated-in-bmp180_register--out-of-bounds-read-live) | Range-check `oss` in `bmp180_register`, reusing the check already in the `SET_OSS` handler. Reject with `RTEMS_INVALID_NUMBER`. |
| [B9](KNOWN_ISSUES.md#b9--division-by-zero-in-the-compensation-math-live) | Guard `X1 + MD == 0` before the divide in `bmp180_compensate`. |
| [B11](KNOWN_ISSUES.md#b11--setuptask-takes-rtems_id-by-value-live) | `rtems_id*` out-parameter on `setupTask`; drop the dead `constexpr` ids. |

**Gate — this is the one that matters.** The six acceptance criteria are stated
in full in [`E_analysis/BASELINE.md`](E_analysis/BASELINE.md#r2-acceptance-criteria);
in short:

1. `rms_pa` falls **monotonically** with `oss`. Primary gate.
2. `p2p_pa` falls monotonically with `oss`.
3. `rms_pa` at or below `datasheet_rms_pa` in every mode.
4. `mean_pa` within ~100 Pa of baseline, allowing for weather movement.
5. `median_interval_us` rises 1–2 ms per mode — predicted 12.0 / 15.0 / 21.0 /
   33.0 ms. **Expected, not a regression:** it is the added margin.
6. Drops, errors and malformed lines stay at zero.

**Take at least two captures.** B1 is stochastic — it landed on a different mode
transition in each of the two baseline runs — so a single monotonic run is
suggestive but not conclusive. Require monotonicity in both.

Selftest must still print `PASS` (B9's guard must not perturb the datasheet
vector).

**B6 and B7 moved to R1** and are already closed. B6 went with `bmp180_task`;
B7 is now an `E <t_us> 19` (`ENODEV`) record emitted before the tasks start.
Neither needs anything from this round.

---

## R3 — Structural

Larger changes, landed one at a time now that R2 has established a trustworthy
sweep as the regression test.

| Item | Change | Notes |
|------|--------|-------|
| [I2](KNOWN_ISSUES.md#i2--seven-functions-declared-static-in-a-shared-header) | Move the seven `static` helpers into an anonymous namespace in `bmp180.cpp`; strip from `bmp.h`. | Pure refactor. Sweep output must be byte-identical to R2. |
| [I1](KNOWN_ISSUES.md#i1--a-measurement-is-not-atomic-on-the-bus) | Wrap the whole `bmp180_do_measurement` sequence in `i2c_bus_obtain`/`i2c_bus_release`, and guard `self->oss`. | The mutex is recursive, so the nested per-transfer locks are safe. Verify by running two reader tasks concurrently — without this, they interleave conversions. |
| [I5](KNOWN_ISSUES.md#i5--poll-the-sco-bit-instead-of-sleeping-a-fixed-time) | Poll `ctrl_meas` bit 5, using the R2 padded delay as the timeout. | Supersedes the R2 padding. `ms/smp` should now *drop* toward typical-case timings while `p2p_Pa` stays flat — that is the success signal. |
| [B4](KNOWN_ISSUES.md#b4--use-after-free-device-freed-while-its-dev-node-is-still-published-latent) / [I15](KNOWN_ISSUES.md#i15--no-teardown-path) | Add a real teardown that goes through `unlink(dev_path)`, and exercise it once so the destroy handlers stop being dead code. | Only meaningful if teardown is actually called; otherwise fold into R4 as a comment. |
| [I8](KNOWN_ISSUES.md#i8--i2c_poll_budget-is-an-iteration-count-not-a-timeout) | Re-express the poll budget in microseconds against `rtems_clock_get_uptime_nanoseconds()`. | Prerequisite for I14 being reviewable. |

**Gate:** sweep matches R2 within noise; the two-task concurrency test produces
coherent readings from both tasks.

---

## R4 — Cleanup

Everything remaining, batched into one commit per theme. No functional risk.

- [I6](KNOWN_ISSUES.md#i6--bitsstl_pairh-is-a-libstdc-internal-header) `<bits/stl_pair.h>` → `<utility>`
- [I10](KNOWN_ISSUES.md#i10--documentation-has-drifted-from-the-code) doc drift: missing `SET_OSS`, the 1 Hz/5 Hz mismatch, the wrong calibration comment, the `[Claude GENERATED]` marker
- [B5](KNOWN_ISSUES.md#b5--debug-output-is-gated-on-ifndef-debug-inverted-latent) inverted `#ifndef DEBUG`, [B10](KNOWN_ISSUES.md#b10--int-return-compared-against-rtems_status_code-latent) status-type confusion, [B13](KNOWN_ISSUES.md#b13--strict-aliasing-violation-in-the-calibration-sanity-check-live) aliasing, [B14](KNOWN_ISSUES.md#b14--missing-newline-in-the-calibration-print-latent) newline
- [I12](KNOWN_ISSUES.md#i12--error-is-too-generic-a-macro-name) `ERROR` macro, [I13](KNOWN_ISSUES.md#i13--task-policy-constant-lives-in-the-ioctl-header) constant placement, [I16](KNOWN_ISSUES.md#i16--redundant-memset-after-calloc) redundant memset, [I17](KNOWN_ISSUES.md#i17--header-guard-style-is-inconsistent) guard style
- [I14](KNOWN_ISSUES.md#i14--no-i2c-bus-recovery-sequence) bus recovery — the one real feature here; needs the SDA-stuck fault injection from `TESTING.md` §5 to verify
- Turn on `-Werror` once the warning list is empty

**Gate:** full `TESTING.md` ladder, including fault injection. Then tag `v1.1.0`
and move the fixed entries out of `KNOWN_ISSUES.md` into the CHANGELOG.

---

## Deferred

| Item | Why |
|------|-----|
| [I9](KNOWN_ISSUES.md#i9--read_measurement-is-declared-_iow-but-writes-back-to-the-caller) | `_IOW` → `_IOR` changes the encoded command number. Breaking ABI, so it waits for a major bump and should be batched with any other ioctl renumbering. |
|  [B2](KNOWN_ISSUES.md#b2--bmp180_task_manual-passes-the-device-path-as-the-bus-path-latent), [B3](KNOWN_ISSUES.md#b3--null-pointer-dereference-in-the-registration-failure-handler-latent), [B4](KNOWN_ISSUES.md#b4--use-after-free-device-freed-while-its-dev-node-is-still-published-latent), [B5](KNOWN_ISSUES.md#b5--debug-output-is-gated-on-ifndef-debug-inverted-latent), [B10](KNOWN_ISSUES.md#b10--int-return-compared-against-rtems_status_code-latent), [B14](KNOWN_ISSUES.md#b14--missing-newline-in-the-calibration-print-latent) | **Closed in v1.2.0** by deleting `bmp180_task_manual` — six bugs for one edit, none of them debugged. If that task is ever revived from git history, all six must be fixed first. |
| [B12](KNOWN_ISSUES.md#b12--zero-length-i2c-message-stalls-for-the-full-poll-budget-latent) | Only reachable if the I2C1 bus gains a second device driver. Revisit then. |
