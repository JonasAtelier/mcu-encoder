# API usage

[Back to README](../README.md)

Paths mentioned in this guide are relative to the repository root.

## Device lifecycle

Initialise the bus, probe once, then read. The
[README quick start](../README.md#quick-start) shows a complete STM32
helper with cleanup. The [ESP-IDF examples](../examples/esp/) show the
corresponding bus setup.

```c
struct encoder_dev encoder = { 0 };
struct encoder_sample sample;

if (as5047p_probe(&encoder, &cfg))	/* probe logs why it failed */
	return;

encoder_set_zero(&encoder);		/* optional: here is 0 rad */

if (!encoder_read(&encoder, &sample))
	encoder_log_sample(&encoder, &sample);
```

One `encoder_log_sample()` line looks like this (values illustrative):

```
encoder: as5047p: t 1234000 us  angle  1.5708 rad  total       7.8540 rad
```

On ESP-IDF the same line carries the log's own prefix, as
`I (1234) encoder: as5047p: ...`.

The other chips are the same after the probe:
`as5600_probe(&encoder, bus, &cfg)` or `amt102_probe(&encoder, &cfg)`. Tear
down with the matching `*_remove(&encoder)`; the bus is left alone.

**Bus and timer handles on ESP-IDF.** The AS5047P's config names an SPI
host the app has `spi_bus_initialize()`d and a CS GPIO; the driver adds its
own device in mode 1 at `spi_hz` (0 for 4 MHz). The AS5600 takes an
`i2c_master_bus_handle_t` the app has created, and the driver adds its own
device at `scl_speed_hz` (0 for 400 kHz). The AMT102 takes A, B and X GPIOs,
and the driver sets up a PCNT unit and, with X, a GPIO interrupt.
`*_remove()` takes each off again.

**Bus and timer handles on the HAL.** The AS5047P's config holds an
`SPI_HandleTypeDef *` the app has `HAL_SPI_Init()`ed in mode 1 (CPOL low,
CPHA second edge) with 8-bit frames, and a chip select as a GPIO port and
pin: software, driven by the driver around each 16-bit frame, so the pin is
an ordinary output, idle high. The probe checks the handle's mode and frame
size and refuses a wrong one with `-EINVAL`. The AS5600 takes an
`I2C_HandleTypeDef *` the app has `HAL_I2C_Init()`ed; the address is the
chip's fixed 0x36. The AMT102's config holds a `TIM_HandleTypeDef *` the
app has set up in encoder mode — see
[installation](installation.md#stm32-hal). Bus and timer speed are the
handle's business.

**A `struct encoder_dev` must start zeroed** — static storage or `= { 0 }`.
Probe on one that is already bound (non-NULL `ops` or `priv`) returns
`-EBUSY` and touches nothing, rather than leak the first binding.
`*_remove()` and a failed probe zero it again, so it can be probed afresh.

**The sample.** `angle_rad` is a `float` in `[0, 2π)` from the zero point.
`total_rad` counts across turns and goes negative the other way; it is a
`double`, so it keeps one-count resolution however long you run (a `float`
loses it after about 1000 turns of a 14-bit encoder). The angle comes from
the integer count, not from `total_rad`, so it keeps full resolution within
the turn however far the total has run. There is no velocity: take
two samples and divide, `(total - last_total) / (t_us - last_t_us)`, with
whatever smoothing your loop wants.

**Absolute or incremental.** `encoder->absolute` is true for the AS5047P and
AS5600: they know where the shaft is the instant they power up, and their
zero starts at the chip's own zero. It is false for the AMT102: it only
counts movement, so zero is wherever the shaft sat at power-up. Home it (see
[the chip reference](chips.md#amt102-v)) before trusting an angle.

**Zero and turn history live in RAM.** `encoder_set_zero()` makes the current
position 0 rad and restarts the turn count; a reboot loses both.

**`cfg.invert`** reverses the direction in software on every chip. On the
two absolute chips it maps a count to `(N − count) mod N` — angle to −angle
— so the chip's own 0 stays 0, and it applies to `*_read_counts()` too. On
the AMT102 it negates the count.

### Sample fast enough

On the absolute chips the turn count is tracked in software from
single-turn readings: a jump of more than half a turn between two reads
counts as a wrap. **Move strictly less than half a turn between successful
reads**, or a revolution is missed:

| time between reads | speed must stay below |
|---|---:|
| 1 ms | 30,000 rpm |
| 10 ms | 3,000 rpm |
| 100 ms | 300 rpm |

The AMT102 on PCNT has no such limit; the driver lets PCNT fold its 16-bit
counter into a 32-bit total itself. On an STM32 timer the counter is
widened in software too, by the same rule at a larger scale: read before
the counter has moved half its range — 32767 counts, 4 turns at 2048 PPR,
on a 16-bit timer; practically never on a 32-bit one.

**Timestamps.** `sample.t_us` comes from `encoder_time_us()`, taken just
before the bus transfer. On ESP-IDF that is `esp_timer_get_time()`. On the
HAL the library's default is weak and only `HAL_GetTick() * 1000`:
whole milliseconds, wrapping after 49.7 days. **Define your own
`int64_t encoder_time_us(void)` for real microseconds** and it replaces the
default at link time — the `DWT->CYCCNT` cycle counter (enable
`CoreDebug->DEMCR` TRCENA and `DWT->CTRL` CYCCNTENA, then divide by the core
clock in MHz and extend the 32-bit count to 64; an F7 may also need
`DWT->LAR` unlocked first), or a free-running timer at 1 MHz.

**No waits.** No driver here sleeps: reads are one bus transaction or a
counter read, and the probes only check the chip and set it up.

**Logging.** On ESP-IDF everything goes under the log tag `encoder`,
prefixed with the chip name, so one `esp_log_level_set("encoder",
ESP_LOG_WARN)` quiets every driver, and `encoder_dbg()` is `ESP_LOGD`. On
the HAL every line goes through `printf()` as `encoder: <chip>: ...`. Your
own code can use the same `encoder_err()`, `encoder_warn()`,
`encoder_info()` and `encoder_dbg()` from `encoder_log.h`. On the HAL
`encoder_dbg()` is compiled out unless `CONFIG_ENCODER_DEBUG` is 1
(stm-forge: `ENCODER_DEBUG = y`).

**Chip extras** all take the same `struct encoder_dev *` and return
`-EINVAL` if another driver probed it: `as5047p_read_counts()`,
`as5047p_read_status()`, `as5600_read_counts()`, `as5600_read_status()`,
`amt102_index_hit()`, and on the HAL `amt102_index_irq()`, which does
nothing for a device it does not own.

### Changes from esp-encoder

The ESP-IDF API and behaviour are esp-encoder's (e788064), unchanged but
for the added `encoder_time_us()`. One thing moved under them: each AS5047P
frame goes out through the bus layer's `exchange()`, which calls
`spi_device_polling_transmit()` where esp-encoder called
`spi_device_transmit()`. The transaction is the same -- 16 bits in the
transaction's own `tx_data`/`rx_data` -- so the frames on the wire are too;
polling only skips the driver's queue.

## Errors

| errno | from | means |
|---|---|---|
| `-EINVAL` | any | NULL argument; `counts_per_turn` not set; an AS5600 filter or hysteresis outside its enum; AMT102 A, B and X not all different; on the HAL an AS5047P handle not in mode 1 with 8-bit frames, or an AMT102 timer whose `Init.Period` is not `0xFFFF` or `0xFFFFFFFF`; a pin, SPI host or glitch filter the IDF driver refuses; an `encoder_dev` another driver probed |
| `-EBUSY` | probe | that `encoder_dev` is already bound. Nothing was touched |
| `-ENXIO` | probe | AS5600: nothing ACKs at 0x36 (checked with `i2c_master_probe()` or `HAL_I2C_IsDeviceReady()` before anything is allocated). AS5047P: the raw DIAAGC reply was 0x0000, 0xFFFF or 0x3FFF -- MISO stuck, nothing answering |
| `-EBADMSG` | any | an AS5047P reply with bad parity |
| `-EIO` | any | AS5047P: the reply's error flag (EF) is set; the driver then reads `ERRFL` to clear it, so the next read is clean |
| `-ENOSPC` | probe | ESP-IDF: no free PCNT unit or channel for the AMT102, or no free CS slot on the AS5047P's SPI host |
| `-EOPNOTSUPP` | extra | `amt102_index_hit()` without an index: no `z_gpio`, or on the HAL `cfg.index` false; an IDF call that reports `ESP_ERR_NOT_SUPPORTED` |
| `-EAGAIN` | any | HAL only: the HAL answered `HAL_BUSY` — the bus, handle or timer is in use, or was never initialised |
| `-ETIMEDOUT` | any | a bus transfer timed out (`ESP_ERR_TIMEOUT`, `HAL_TIMEOUT`) |
| `-EIO` | any | any other bus or driver failure, a NACK in the middle of a transfer included |
| `-ENOMEM` | probe | out of heap |

On ESP-IDF failures map from `esp_err_t`: `ESP_ERR_TIMEOUT` is
`-ETIMEDOUT`, `ESP_ERR_INVALID_ARG` `-EINVAL`, `ESP_ERR_NOT_FOUND` `-ENXIO`,
`ESP_ERR_INVALID_SIZE` `-EMSGSIZE`, `ESP_ERR_NO_MEM` `-ENOMEM`,
`ESP_ERR_NOT_SUPPORTED` `-EOPNOTSUPP` and the rest -- a NACKed transfer's
`ESP_ERR_INVALID_STATE` included -- `-EIO`. On the HAL, `HAL_TIMEOUT` is `-ETIMEDOUT`, `HAL_BUSY` `-EAGAIN` and
`HAL_ERROR` `-EIO`. `strerror(-ret)` names any of them. **Do not use a
sample when `encoder_read()` returns an error**: it writes `*sample` only on
success, so the old values are still there. A failed read does not update
the turn count either; it only widens the gap to the next good one.
