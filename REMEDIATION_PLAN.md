# Remediation Plan — v1.0.0 → v1.1.0

Schedule for working off [KNOWN_ISSUES.md](KNOWN_ISSUES.md). Four rounds, each
with an explicit verification gate that must pass before the next round starts.

**Decisions taken:**

- **B1 is fixed by padding the conversion constants**, not by SCO polling. SCO
  polling (I5) stays a separate change in R3, so the timing fix and the
  restructure can be bisected independently.
- **Hardware is available**, so every round ends on a real flash-and-read gate,
  not just a compile.

**Release target:** `1.1.0` — new behaviour (bus recovery, atomic measurement),
no breaking API change. [I9](KNOWN_ISSUES.md#i9--read_measurement-is-declared-_iow-but-writes-back-to-the-caller)
changes ioctl command numbers and is therefore deferred out of this plan
entirely; it belongs to a `2.0.0`.

---

## Phase 0 — Instrumented baseline (before any edit)

Superseded the original "capture the console text" step. Rather than eyeballing
four summary rows, v1.0.0 is instrumented with the structured telemetry stream
and a real dataset is captured, so every later gate is a numerical comparison
instead of a judgement call.

**Plan:** [`docs/superpowers/plans/2026-08-03-phase0-telemetry-baseline.md`](docs/superpowers/plans/2026-08-03-phase0-telemetry-baseline.md)
**Design:** [`B_docs/TELEMETRY_DESIGN.md`](B_docs/TELEMETRY_DESIGN.md)

The measurement path is untouched in this phase — only reporting is replaced.
The deliverable is `E_analysis/BASELINE.md` plus the raw captures.

**Expected in the baseline:** `p2p_Pa` that does *not* fall cleanly with rising
OSS. That is the [B1](KNOWN_ISSUES.md#b1--conversion-wait-can-expire-before-the-conversion-finishes-live)
signature, and it is what R2 has to remove.

> Because Phase 0 replaces the sweep's on-device statistics with raw sample
> emission, the R2/R3 gates below are now evaluated with
> `metrics.per_oss_summary()` rather than by reading the old printed table.

---

## R1 — Build safety net

Nothing here changes runtime behaviour. It exists so the rest of the plan is
verifiable at all.

| Item | Change |
|------|--------|
| [I3](KNOWN_ISSUES.md#i3--three-divergent-toolchain-path-mechanisms) | Collapse the three toolchain-path mechanisms onto `local.cmake`. The hardcoded `/Users/albertofurlan/...` in `C_src/Makefile` cannot build on this machine — the toolchain is at `/Volumes/POLI/tools/`. |
| [I4](KNOWN_ISSUES.md#i4--no-warning-flags-anywhere-in-the-build) | Add `-Wall -Wextra` to all build paths. Not `-Werror` yet — the existing warnings must be read before they become fatal. |
| [I18](KNOWN_ISSUES.md#i18--dead-code-carried-in-the-build) | Delete `bmp180_task_manual`. It holds three of the latent High bugs (B2, B3, B4) and is not wired in. Deleting it closes them without debugging them. |

**Gate:** clean build from a fresh checkout with only `local.cmake` supplied.
Triage the new warning list — it should surface
[B11](KNOWN_ISSUES.md#b11--setuptask-takes-rtems_id-by-value-live) and
[I2](KNOWN_ISSUES.md#i2--seven-functions-declared-static-in-a-shared-header)
on its own. Flash and confirm the sweep still matches baseline.

**Note:** deleting `bmp180_task_manual` also removes the only consumer of
`bmp180_load_calibration` as a public entry point and the only `<stdexcept>`
user, which makes [I7](KNOWN_ISSUES.md#i7--c-exceptions-used-to-signal-ordinary-error-codes)
mostly disappear as a side effect. Confirm no `-fno-exceptions` opportunity is
missed before closing the round.

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
| [B6](KNOWN_ISSUES.md#b6--negative-temperatures-print-malformed-live) | Format temperature via `abs()` on the fractional part with an explicit sign on the whole part. Both print sites. |
| [B7](KNOWN_ISSUES.md#b7--registration-failure-leaves-a-silently-dead-board-live) | Either re-enable the heartbeat task or drop the comment that promises it. Registration failure must not be silent. |
| [B11](KNOWN_ISSUES.md#b11--setuptask-takes-rtems_id-by-value-live) | `rtems_id*` out-parameter on `setupTask`; drop the dead `constexpr` ids. |

**Gate — this is the one that matters.** Flash, capture the sweep, compare to
baseline:

- `p2p_Pa` and `std_Pa` must now **fall monotonically** as OSS rises. That is the
  direct evidence B1 was real and is fixed.
- `mean_Pa` must stay ~constant across all four modes and match baseline.
- `ms/smp` will rise by ~1–2 ms per mode. **This is expected**, not a
  regression — it is the added margin. Record it so it is not mistaken for one
  later.
- Selftest must still print `PASS` (B9's guard must not perturb the datasheet
  vector).

Additionally: cool the sensor below 0 °C (canned air, inverted) and confirm the
temperature line reads e.g. `-2.4`, not `-2.-4`.

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
| [I11](KNOWN_ISSUES.md#i11--sweep-task-runs-on-rtems_minimum_stack_size) | Give `setupTask` an explicit stack size. | Cheap; do alongside the B11 signature change if R2 slips. |

**Gate:** sweep matches R2 within noise; the two-task concurrency test produces
coherent readings from both tasks.

---

## R4 — Cleanup

Everything remaining, batched into one commit per theme. No functional risk.

- [I6](KNOWN_ISSUES.md#i6--bitsstl_pairh-is-a-libstdc-internal-header) `<bits/stl_pair.h>` → `<utility>`
- [I10](KNOWN_ISSUES.md#i10--documentation-has-drifted-from-the-code) doc drift: missing `SET_OSS`, the 1 Hz/5 Hz mismatch, the wrong calibration comment, the `[Claude GENERATED]` marker
- [B5](KNOWN_ISSUES.md#b5--debug-output-is-gated-on-ifndef-debug-inverted) inverted `#ifndef DEBUG`, [B10](KNOWN_ISSUES.md#b10--int-return-compared-against-rtems_status_code-latent) status-type confusion, [B13](KNOWN_ISSUES.md#b13--strict-aliasing-violation-in-the-calibration-sanity-check-live) aliasing, [B14](KNOWN_ISSUES.md#b14--missing-newline-in-the-calibration-print-latent) newline
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
| [B2](KNOWN_ISSUES.md#b2--bmp180_task_manual-passes-the-device-path-as-the-bus-path-latent), [B3](KNOWN_ISSUES.md#b3--null-pointer-dereference-in-the-registration-failure-handler-latent) | Closed by deleting `bmp180_task_manual` in R1. If that task is ever revived, both must be fixed first. |
| [B12](KNOWN_ISSUES.md#b12--zero-length-i2c-message-stalls-for-the-full-poll-budget-latent) | Only reachable if the I2C1 bus gains a second device driver. Revisit then. |
