# Pin configuration — STM32F407 Discovery

Two independent links. They share nothing but ground, and neither can stall the
other.

| Link | Peripheral | STM32 pins | Other end |
|------|-----------|------------|-----------|
| Sensor data | **I2C1**, 100 kHz | PB6 = SCL, PB7 = SDA | BMP180 at 7-bit address `0x77` |
| Console to the Mac | **USART2**, 115200 8N1 | PA2 = TX, PA3 = RX | CP2102 USB-to-TTL |

**I2C is the sensor bus only.** Nothing in this project sends telemetry over
I2C. The stream reaching the Mac goes out USART2, which the RTEMS BSP owns —
`src/telemetry/wire.cpp` writes to `STDOUT_FILENO` and never names a UART.

The separation is testable: pull SDA during a run and `S` records stop while the
console keeps printing the `E` record that reports the fault (`C_src/TESTING.md`
§5). If the two shared a peripheral, that test could not work.

---

## STM32F407 to BMP180 (pressure sensor)

Driven by `C_src/src/i2c/i2c.cpp`, a polled STM32F4 master — the BSP ships no
I2C driver. Pins are AF4 (`STM32F4_GPIO_AF_I2C1`), open-drain with pull-up.

### BMP180 pins
- GND
- VIN (3.3 V)
- SDA (serial data)
- SCL (serial clock)

### Connections
| BMP180 | STM32 | Note |
|--------|-------|------|
| GND | GND | |
| VIN | VDD (3.3 V) | **not 5 V** |
| SDA | PB7 | I2C1_SDA |
| SCL | PB6 | I2C1_SCL |

---

## STM32F407 to CP2102 (USB to TTL)

The CP2102 carries the console, which is where the telemetry stream leaves the
board. **TX and RX cross**: the dongle transmits into the STM32's receive pin
and vice versa.

### CP2102 pins
- GND
- 5V
- 3.3V
- TXD
- RXD

### Connections
| CP2102 | STM32 | Note |
|--------|-------|------|
| GND | GND | common ground is required, not optional |
| TXD | PA3 | USART2_RX — dongle transmits, STM32 receives |
| RXD | PA2 | USART2_TX — STM32 transmits, dongle receives |
| 5V | 5V | power the dongle from the board, or leave it USB-powered |

Only the `PA2` line actually carries data in this project: the firmware never
reads from the console.

### Which USART, and on which pins

The BSP defines two pin mappings for USART2 — `PA2`/`PA3` and `PD5`/`PD6`
(`bsp/io.h`, `STM32F4_PIN_USART2_*`). This project uses **PA2/PA3**.

Other ports, listed because the earlier revision of this file confused them:

| Port | TX | RX | State here |
|------|----|----|------------|
| USART1 | PA9 | PA10 | **not enabled** — these are *not* USART2 pins |
| USART2 | **PA2** | **PA3** | ✅ the console |
| USART3 | PD8 (or PC10) | PD9 (or PC11) | **not enabled** — was the BSP default before `config.ini` changed it |

Check what your own BSP was built with. The path comes from `.env/setup.env`, so
this works on any machine:

```bash
source ../.env/setup.env
grep -E "USART|UART|BAUD|CONSOLE" \
  "$RTEMS_LOCAL_PATH/RTEMS_toolchain/rtems/7/arm-rtems7/stm32f4/lib/include/bspopts.h"
```

Current output for this project's toolchain:

```
/* #undef STM32F4_ENABLE_UART_4 */
/* #undef STM32F4_ENABLE_UART_5 */
/* #undef STM32F4_ENABLE_USART_1 */
#define STM32F4_ENABLE_USART_2 1
/* #undef STM32F4_ENABLE_USART_3 */
/* #undef STM32F4_ENABLE_USART_6 */
#define STM32F4_USART_BAUD 115200
#define BSP_CONSOLE_USE_INTERRUPTS 1
```

USART3 is the stock BSP default. It was switched to USART2 when the BSP was
built, via `config.ini` in the RTEMS build tree:

```ini
[arm/stm32f4]
BUILD_TESTS = True
# STM32F4_USART_BAUD = 115200   <- default, left alone
STM32F4_ENABLE_USART_2 = True
STM32F4_ENABLE_USART_3 = False
```

Changing this means rebuilding the BSP, not just the firmware. See
[`SETUP.md`](SETUP.md).

---

## On the Mac

```bash
screen /dev/cu.usbserial-0001 115200    # cu.*, never tty.*
```

`cu.*` and not `tty.*`: the `tty.*` node blocks on open waiting for carrier
detect, which a three-wire adapter never asserts. For anything quantitative use
`E_analysis/capture.sh`, which holds the port open and resets the board over the
ST-Link so the capture starts at the session header.
