# Decoupling temperature acquisition from pressure acquisition

**Feeds:** Design and implementation
**Status:** Decided, not yet implemented
**Sources:** BST-BMP180-DS000-09 Rev 2.5 (April 2013) §3.3, §3.5, Table 3;
`C_src/src/bmp180.cpp:216-244`

## The decision

The driver stops converting temperature once per pressure sample. Temperature is
converted on its own slow cadence — nominally once per second — and the
resulting `B5` term is cached and reused for every pressure sample taken in the
interim.

## Why the system framing makes this the right trade

RTEMS was originally the *Real-Time Executive for Missile Systems*, written for
guidance applications before being generalised and renamed. That heritage is a
useful lens for this project, because it fixes what the two measured quantities
are actually *for* in a guidance-class system:

- **Pressure is the signal.** It is the input to the barometric altitude
  computation (§3.6), and therefore to anything downstream that needs to know
  where the vehicle is. It is the quantity whose *rate of change* carries
  information — vertical velocity is the derivative of the altitude the pressure
  produces.
- **Temperature is a correction on that signal.** It is not, in this
  application, an output anyone is steering by. Its role in the measurement
  chain is to produce `B5`, which cancels the temperature dependence of the
  piezo-resistive pressure element.

Once framed that way, the sampling requirement for each falls out of what each
one is tracking, not out of symmetry in the code. A correction term should be
sampled fast enough to track the error it corrects — no faster.

## What the original implementation cost

`bmp180_do_measurement` (`C_src/src/bmp180.cpp:216-244`) performs a full
temperature conversion followed by a full pressure conversion on every call:

```
write ctrl_meas = 0x2E   ->  temperature conversion  (4.5 ms)  ->  read 0xF6
write ctrl_meas = 0x34+  ->  pressure conversion     (4.5-25.5 ms) -> read 0xF6
```

Both conversions are blocking waits on the sensor's internal ADC. In standard
mode (OSS=1) this is 4.5 ms of temperature for every 7.5 ms of pressure —
**37.5 % of every acquisition cycle spent re-measuring a quantity that is
physically incapable of changing meaningfully over that window.**

The waste is not CPU time. The task yields during conversion via
`rtems_task_wake_after`, so the processor is idle. What is consumed is *sample
rate*, which in a guidance context is the scarce resource: it sets the bandwidth
of the altitude estimate and the latency of any derived vertical rate.

## The physical argument

The temperature the compensation needs is the **die** temperature, not ambient
air temperature. The `B5` term corrects for thermal drift in the pressure
element itself, so the relevant quantity is the temperature of the silicon a few
hundred micrometres away from that element.

That quantity is low-pass filtered by the thermal mass of the die and its
package. Its time constant is on the order of seconds. It is not merely that
temperature "changes slowly" in the environment — it is that the sensor is
*structurally incapable* of presenting a fast temperature transient to the
compensation path.

Pressure has no equivalent limiter. It is a bulk property of the gas, couples to
the sensor through the vent essentially instantaneously, and in the target
application changes fast by design: §3.6 gives 1 hPa per 8.43 m at sea level, so
a 1 m altitude change appears as ≈ 11.9 Pa immediately.

Sampling both at the same rate therefore oversamples one signal by roughly three
orders of magnitude while undersampling the other.

## Vendor confirmation

This is not an inference the project made against the datasheet; it is the
manufacturer's stated recommendation. BST-BMP180-DS000-09 §3.3:

> "The sampling rate can be increased up to 128 samples per second (standard
> mode) for dynamic measurement. In this case, it is sufficient to measure the
> temperature only once per second and to use this value for all pressure
> measurements during the same period."

Two things are worth drawing out of that sentence:

1. **"for dynamic measurement"** — Bosch names the regime this project is in.
   The decoupled mode is the one intended for a moving platform; the coupled
   1:1 mode of §3.5's flow diagram is the static, low-rate case.
2. **"128 samples per second (standard mode)"** — a concrete target. Standard
   mode is OSS=1, whose pressure conversion is 7.5 ms (Table 3), giving a
   theoretical ceiling of 133 Hz. Bosch's 128 Hz figure sits just under it,
   which confirms that the quoted rate assumes exactly one conversion per
   sample — i.e. that temperature has been taken out of the inner loop.

## Quantitative effect

Conversion time only, excluding I²C bus traffic (≈ 0.5–1 ms per cycle at
100 kHz):

| Mode | OSS | Coupled | Decoupled | Speed-up |
|------|-----|---------|-----------|----------|
| ultra low power | 0 | 9.0 ms / 111 Hz | 4.5 ms / 222 Hz | 2.00× |
| standard | 1 | 12.0 ms / 83 Hz | 7.5 ms / 133 Hz | 1.60× |
| high resolution | 2 | 18.0 ms / 55 Hz | 13.5 ms / 74 Hz | 1.33× |
| ultra high resolution | 3 | 30.0 ms / 33 Hz | 25.5 ms / 39 Hz | 1.18× |

The gain is largest at low OSS. This is convenient rather than coincidental: low
OSS is the mode a dynamic application selects when it wants bandwidth, and it is
precisely where the fixed 4.5 ms temperature cost dominates the cycle. At OSS=3,
where the application has already chosen resolution over rate, the decoupling
buys comparatively little — and correspondingly matters less.

## Cost, and how it is bounded

The reused `B5` is stale by up to one temperature interval. Two effects could in
principle make that staleness matter, and both are addressed rather than
assumed:

1. **Environmental temperature drift.** Bounded by the thermal time constant
   argument above and by Bosch's explicit sanction of a 1 s interval.
2. **Self-heating.** Running the sensor at a high duty cycle dissipates more
   power than the 3–12 µA quiescent figures in Table 3, which are quoted at
   1 sample/s. The die may therefore warm *because* of the sampling rate,
   meaning the reported temperature is not exactly ambient and drifts with load.

The second effect is the more interesting one and is not something the datasheet
bounds for this duty cycle. The telemetry format therefore records temperature
on every sample line alongside pressure, so the drift is directly observable in
the captured stream rather than argued about: if self-heating is significant, it
appears as a monotonic temperature ramp correlated with acquisition rate.

The temperature interval is exposed as a runtime parameter rather than a
compile-time constant, so the interval can be swept and its effect on pressure
noise and bias measured empirically on the target hardware.

## Verification criterion

Table 3 gives typical RMS pressure noise per mode: 0.06, 0.05, 0.04 and
0.03 hPa for OSS 0–3 respectively (equivalently 6, 5, 4 and 3 Pa; or 0.5, 0.4,
0.3 and 0.25 m of altitude). Decoupling is judged correct if measured RMS noise
remains consistent with these figures after the change — that is, if the extra
throughput is obtained without degrading the per-sample quality that the
oversampling setting is supposed to deliver.
