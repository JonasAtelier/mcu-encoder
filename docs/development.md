# Architecture, driver development and tests

[Back to README](../README.md)

Paths mentioned in this guide are relative to the repository root.

## Repository layout

```
include/                    public API, the only directory apps include
  encoder.h                 struct encoder_dev, encoder_read(), encoder_set_zero(), encoder_time_us()
  encoder_log.h             encoder_err/warn/info/dbg, encoder_log_sample()
  as5047p.h as5600.h amt102.h
                            one header per chip: probe, remove, extras
src/                        private to the library
  encoder.c                 the core: timestamp, ops.read, counts into radians
  encoder_log.c             encoder_log_sample()
  encoder_priv.h            bus ops, attach functions, errno maps, turn count; shared by all chips
  encoder_port_esp.c        the bus layer on ESP-IDF; empty elsewhere
  encoder_port_stm.c        the bus layer on the STM32 HAL; empty under ESP-IDF
  as5047p/                  as5047p.c  as5047p_regs.h  as5047p_math.h
  as5600/                   as5600.c   as5600_regs.h
  amt102/                   amt102.c   amt102_math.h
test/                       host tests, `make -C test test`
  test_<chip>_math.c        maths tests: no bus and no platform API
  core_cases.h              core cases, shared by esp/ and stm/
  as5047p_cases.h           an AS5047P at frame level and its cases, shared
  as5600_cases.h            an AS5600 register file and its cases, shared
  rad.h                     the radians encoder_read() should report
  esp/                      suites on idf_stubs/, through encoder_port_esp.c:
                            esp-encoder's own test_<chip>.c, and
                            test_<chip>_shared.c running the shared cases
  stm/                      suites on hal_stubs/, through encoder_port_stm.c
  idf_stubs/  hal_stubs/    the slice of ESP-IDF and of the HAL the code calls
examples/
  esp/<chip>/               ESP-IDF projects: amt102, as5047p, as5600
  stm/<chip>/               STM32 snippets: amt102, as5047p, as5600
docs/                       the five guides; datasheet PDFs here are gitignored
assets/                     README and chip-reference figures
CMakeLists.txt              ESP-IDF component; elsewhere a static library for add_subdirectory(); stm-forge globs src/**/*.c instead
                            the static library's options are Kconfig's bool entries, found by the type line directly under each `config`
Kconfig                     ENCODER and ENCODER_<CHIP>, for ESP-IDF and both forges' menus
idf_component.yml           ESP-IDF component manifest
```

Naming inside `src/<chip>/`, the same in every category library:

| File | Holds |
|---|---|
| `<chip>.c` | the driver: probe, remove, the ops, the chip's extras; may split into `<chip>_<part>.c` |
| `<chip>_regs.h` | register map: addresses, bit fields, values, each with its datasheet citation |
| `<chip>_proto.h` | serial protocol: command bytes, frames, checksums, for parts without registers |
| `<chip>_math.h` | pure conversions, compiled and asserted on the host by `test/<chip>_math` tests |
| `<chip>_priv.h` | driver state, only if needed |

A chip has `_regs.h` or `_proto.h`, never both; `_math.h` and `_priv.h` only
when it needs them. Everything in `src/` is private: apps see `include/` only.
The AS5047P has registers behind its frames, so `as5047p_regs.h` holds the
frame bits as well; the AMT102 has neither registers nor a protocol, only
its counter arithmetic in `amt102_math.h`.

## Platform ports

It replaces esp-encoder (e788064), and adds the STM32 HAL. The bus chips' drivers in
`src/` are the same C on both platforms; only the bus and the clock under
them differ, in `src/encoder_port_esp.c` (ESP-IDF `i2c_master`,
`spi_master`, UART driver, `esp_timer`) and `src/encoder_port_stm.c` (HAL).
Each port is empty on the other platform, chosen by `ESP_PLATFORM`, which
ESP-IDF defines for every component. Probes and configs take the platform's
own handles: on ESP-IDF esp-encoder's, unchanged — an SPI host and CS GPIO,
an `i2c_master_bus_handle_t`, GPIO numbers — and on the HAL an
`SPI_HandleTypeDef *` and CS port/pin, an `I2C_HandleTypeDef *`, a
`TIM_HandleTypeDef *`, with clocks and modes the handle's.

The AMT102 is a pulse device, not a bus chip, and sits outside the bus
layer: `src/amt102/amt102.c` holds a PCNT half and a timer half behind
`#ifdef ESP_PLATFORM`, around one set of ops and extras. On the HAL the
library never configures the timer — the same rule as for buses — it only
starts, reads and stops what the app set up.

`encoder_priv.h` also holds the two errno maps, `encoder_errno()` for
`esp_err_t` and `encoder_hal_errno()` for `HAL_StatusTypeDef`, because the
AMT102 needs them outside the ports; and `encoder_track_turns()`, the
software turn count both absolute chips keep. On the HAL it stops the build
with an `#error` when a chip's module is off: I2C for the AS5600, SPI for
the AS5047P, TIM for the AMT102.

## Bus abstraction

Under the bus chips sits a regmap-style ops table, `struct
encoder_bus_ops` in `src/encoder_priv.h`: each chip holds one `struct
encoder_bus` and talks to it only through its ops. The kind — I2C, SPI or
UART — is picked at run time by which attach the probe calls. The AS5600 is
on I2C, the AS5047P on SPI; UART is there because every category library
offers all three kinds, and `test/{esp,stm}/test_port_*` checks it.

`transfer(cmd, tx, rx)` is the register access, as in the other category
libraries. The AS5047P adds a need they do not have: its answer comes back
*during* the next 16-bit frame, and every frame needs its own chip select,
so `transfer()`, which keeps only what comes after cmd and tx, would lose
the answer's first byte. So SPI has a second op, `exchange(tx, rx, len)`:
one frame under one chip select, full duplex, `rx[i]` clocked in while
`tx[i]` goes out. I2C and UART leave it NULL and `encoder_bus_exchange()`
says `-EOPNOTSUPP`. On ESP-IDF a frame of up to four bytes travels in the
transaction's own `tx_data`/`rx_data`, as esp-encoder sent the AS5047P's;
a longer one goes through buffers. It is a candidate to backport to the
other mcu-* libraries' bus layers, for the next SPI part that streams its
answer the same way; none of them has one yet.

There is no `receive()`: every chip here answers a register read or a
frame, none streams, and none wants its command and its answer as two I2C
transactions.

`encoder_spi_attach()` takes an SPI host the app has `spi_bus_initialize()`d,
a CS GPIO, a clock and a mode on ESP-IDF, or an `SPI_HandleTypeDef *` and a
CS port and pin on the HAL. `encoder_uart_attach()` takes a `uart_port_t`
whose driver the app has installed, or a `UART_HandleTypeDef *` it has
initialised. On the HAL the I2C, SPI and UART code each sit under their
module switch.

## Adding a chip

A driver is a probe, a remove, and two ops:

1. `include/foo.h`: `int foo_probe(struct encoder_dev *encoder, <bus>, const struct foo_config *cfg);`
   and `void foo_remove(struct encoder_dev *encoder);`, with the platform's
   handle types behind `#ifdef ESP_PLATFORM`, as `as5600.h` does.
2. `src/foo/foo.c`, in a folder of its own with its private headers
   (`foo_regs.h`, ...); common files and the platform ports stay at the top
   of `src/`. Include siblings and `encoder_priv.h` by plain name, never
   with `../`: siblings resolve from the chip's own folder, and
   `encoder_priv.h` through `src/`, which the ESP-IDF CMakeLists has as
   `PRIV_INCLUDE_DIRS` and stm-forge's `libs.cmake` puts on this library's
   include path while it compiles every `.c` under `src/`. Wrap the whole
   driver in `#if CONFIG_ENCODER_FOO` (never `#ifdef`); the HAL module check
   goes in `src/encoder_priv.h`. A bus chip keeps a `struct encoder_bus` in
   its state and talks to the chip only through `encoder_reg_read()`,
   `bus.ops->transfer()` or `encoder_bus_exchange()`. Give it a
   `static const struct encoder_ops foo_ops = { .read = foo_read, .set_zero = foo_set_zero };`.
   `foo_read()` returns signed counts since the zero point, across turns;
   `encoder_read()` does the timestamp and the radians. An absolute chip can
   use `encoder_track_turns()` for the turn count and `encoder_invert()`
   for the software direction.
3. In probe: return `-EBUSY` if `encoder->ops` or `encoder->priv` is already
   set, before touching anything. Set `encoder->name` so failures log with
   it, refuse a bad config, check presence with `encoder_i2c_probe()` on I2C
   before allocating, allocate your state, attach its bus, and only on
   success set `ops`, `priv`, `counts_per_turn` and `absolute`; on failure
   `encoder_bus_detach()` and free. Remove detaches and clears them. Use
   `encoder_err()` for anything the user needs to see.
4. Guard your chip-specific calls with `encoder->ops == &foo_ops`.
5. Add `CONFIG_ENCODER_FOO` to the no-chip checks in `CMakeLists.txt` (its
   ESP-IDF part; the `add_subdirectory()` part reads `Kconfig`) and
   `src/encoder_port_stm.c` and a HAL module `#error` to
   `src/encoder_priv.h`, a `src/foo/foo.c` line to `CMakeLists.txt`, a
   `config ENCODER_FOO` bool to `Kconfig`, `# ENCODER_FOO is not set` to
   both forges' defconfigs, and `test/esp/test_foo.c` and
   `test/stm/test_foo.c` (and `test/test_foo_math.c` for its maths header)
   targets to `test/Makefile`.

## Tests

```sh
make -C test test
```

Host-only; no toolchain, HAL or ESP-IDF needed. The layout is
[Repository layout](#repository-layout)'s: `test/esp/` through
`src/encoder_port_esp.c` and `test/idf_stubs/`, `test/stm/` through
`src/encoder_port_stm.c` and `test/hal_stubs/`. Nothing is checked against
real silicon.

- **esp-encoder's own tests** (e788064) run as `test/esp/test_<chip>.c`,
  against the stubs they were written for (`test/idf_stubs/` is theirs, with
  what the bus port also needs added). One line differs from upstream: the
  AS5047P fake is `spi_device_polling_transmit()` rather than
  `spi_device_transmit()`, because the bus port polls. No assertion changed.
- **This library's own cases** for the core and the bus chips are written
  once, in `test/*_cases.h`, against a fake that knows nothing of the
  driver; `test/esp/test_*_shared.c` and `test/stm/test_*.c` feed that fake
  from their own platform's calls and add the checks only that platform
  has. So both ports run the same cases.
- The AMT102 is not a bus chip, so its two suites test its two counters:
  esp-encoder's on PCNT, `stm/test_amt102` on a timer.
- The maths tests in `test/` touch no bus or platform API and run once.

| Binary | Checks |
|---|---|
| `esp/test_core` | esp-encoder's: stamps, errno passthrough, set_zero, counts to radians, turn tracking, invert, the errno map, the log prefix |
| `esp/test_as5047p` | esp-encoder's: probe (mode 1, 4 MHz, MISO stuck low and high, bad parity, EF, no free CS slot, a bus failure), DAEC register, wraps and zero, invert, a failed read changing nothing, EF cleared by an ERRFL read, status, NULL and foreign devices, a second probe refused |
| `esp/test_as5600` | esp-encoder's: probe, absolute before zero, wraps forward and backward with zero, invert, a failed read changing nothing, status, NULL and foreign devices, a second probe refused |
| `esp/test_amt102` | esp-encoder's: probe and its refusals, counts to radians, invert, the index under its spinlock, a failed read changing nothing, NULL and foreign devices, a second probe refused |
| `test_as5047p_math` | the frame parity against hand-counted frames, and over every 15-bit value: data kept, parity even, the old bit 15 ignored, a frame with its parity its own fixed point |
| `test_amt102_math` | the timer widening at 16 and 32 bits: forward and back across the wrap, the largest moves that still come out right, upper bits ignored at 16, and a 100000-step walk wrapping both ways that sums to the truth |
| `esp/test_core_shared`, `stm/test_core` | `encoder_read()` stamps before the read, converts counts to an angle in `[0, 2π)` and a signed `double` total, keeps full angle resolution a million turns out, passes errors through and leaves the sample alone; `encoder_set_zero()` and a missing op or zero `counts_per_turn`; `encoder_track_turns()` across zero both ways and at exactly half a turn; log prefix and `encoder_log_sample()` format, including 64-bit timestamps without `%lld`; on the HAL, the default clock is `HAL_GetTick()` in µs without 32-bit wrap |
| `esp/test_as5047p_shared`, `stm/test_as5047p` | a fake AS5047P at frame level -- parity, pipelined answers, EF -- through `exchange()`: probe's four frames clearing ERRFL, MISO stuck low or high or DIAAGC all data bits as `-ENXIO`, bad parity `-EBADMSG`, a refused command `-EIO`, a bus timeout; angles and turns through zero in steps under half a turn, zero, raw counts, `ANGLEUNC` with `raw_angle`, inversion keeping 0 at 0; a refused read followed by an ERRFL read and a clean next read, bad parity with no ERRFL read; DIAAGC and MAG fields; `-EBUSY`, another driver's device refused, a failed probe leaks nothing. ESP-IDF: the device added in mode 1 at 4 MHz or `spi_hz`, removed again, a refused add, each frame in the transaction's own `tx_data`/`rx_data`. HAL: a chip select around every frame and high again after a failure, `-EAGAIN` and `-EIO`, a handle not in mode 1 or not 8-bit refused before anything |
| `esp/test_as5600_shared`, `stm/test_as5600` | a fake AS5600 register file: presence checked before anything is allocated, `-ENXIO` when absent, a filter or hysteresis outside its enum refused before the bus; CONF read-modify-write setting only SF and HYST and keeping every other bit, NULL config writing the defaults; angles and turns, zero, `RAW ANGLE` for raw counts, the upper nibble ignored, inversion keeping 0 at 0; STATUS, AGC and MAGNITUDE; bus failures mapped and nothing leaked; `-EBUSY`, another driver's device refused. ESP-IDF: `i2c_master_probe()` before the device is added, at 400 kHz or `scl_speed_hz`, removed again. HAL: the address shifted, `-EAGAIN` and `-EIO` |
| `stm/test_amt102` | a fake timer the test turns: started and stopped, the counter never written; across 0xFFFF both ways and twenty turns of a 16-bit counter in reads of up to 32767 counts; across 0xFFFFFFFF, and ten turns in one read of a 32-bit counter; zero and inversion; the index through `amt102_index_irq()`, read and cleared with interrupts masked and PRIMASK put back, ignored for a device not its own; an `Init.Period` other than 0xFFFF or 0xFFFFFFFF refused before anything is started; `HAL_ERROR`/`BUSY`/`TIMEOUT` from the start mapped and nothing leaked |
| `esp/test_port_esp`, `stm/test_port_stm` | the SPI and UART ops of each port. SPI `transfer()`: attach, one full-duplex transaction with rx taken after cmd and tx, a write, bounds, failures mapped with rx untouched, on the HAL chip select high again after a failure. SPI `exchange()`: one frame, rx clocked during tx -- on ESP-IDF in `tx_data`/`rx_data` up to four bytes and through buffers past that -- empty and oversize frames refused, failures mapped with rx untouched, on the HAL under one chip select and high again after a failure; no `exchange()` on UART or I2C. UART: stale input dropped, command and arguments in one send, short answers as `-ETIMEDOUT`, driver and HAL failures mapped |

All build under `-std=gnu11 -Wall -Wextra -Werror -pedantic`, with
`-Wl,--wrap` (GNU ld) to count allocations and capture `printf()`;
esp-encoder's tests wrap `calloc()` alone, as they always did.
