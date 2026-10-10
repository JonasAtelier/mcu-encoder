# Installation

[Back to README](../README.md)

Paths mentioned in this guide are relative to the repository root.

The repository is `mcu-encoder`. The existing forge package key, C API and
configuration options retain the `encoder` name. The clone command below
names the ESP-IDF component directory `encoder`; esp-encoder's README had
it checked out as `esp-encoder`, so a component that `REQUIRES esp-encoder`
changes that one word, or clones into `components/esp-encoder` as before.

## With forge

stm-forge must be at `ee921c3` or later ("build: libraries may keep sources
in src/ subfolders"); older ones only compile `src/*.c`, so the chip drivers
in `src/<chip>/` are never built and the link fails with undefined
references to the chip probes. esp-forge is unaffected: the component's
CMakeLists lists its sources.

With [esp-forge](https://github.com/JonasAtelier/esp-forge) or
[stm-forge](https://github.com/JonasAtelier/stm-forge). The catalog line in
`profiles/pkgversion` is the same for both:

```make
encoder_GIT=https://github.com/JonasAtelier/mcu-encoder@stable
```

and a target turns the library and its chips on in `configs/<target>_defconfig`:

```make
PKG_encoder         = y
ENCODER_AS5047P     = y
# ENCODER_AS5600 is not set
# ENCODER_AMT102 is not set
```

or with `make menu`. esp-forge turns each line into `CONFIG_ENCODER_<CHIP>=y`
in `sdkconfig.defaults`; stm-forge passes it as `-DCONFIG_ENCODER_<CHIP>=1`.
`ENCODER_DEBUG = y` turns on `encoder_dbg()` lines on the HAL; on ESP-IDF
the log level decides. The key and the options are esp-encoder's, so an
esp-forge target moves over by changing only the catalog line.

## ESP-IDF

Clone into your project's `components/` directory. A local
component is named after its directory, and that name is what you
`REQUIRES`:

```sh
git clone https://github.com/JonasAtelier/mcu-encoder components/encoder
```

```cmake
idf_component_register(SRCS "main.c" REQUIRES encoder driver)
```

Enable the chips under `idf.py menuconfig` → **Component config → Encoder
subsystem**, or put `CONFIG_ENCODER_<CHIP>=y` in your `sdkconfig.defaults`
as each example in `examples/esp/` does. With the subsystem on and no chip
picked, the build warns. The AMT102 option only appears on chips with a
PCNT peripheral. ESP-IDF 5.2 or newer is required for the `i2c_master` and
`pulse_cnt` APIs.

## STM32 HAL

Copy `include/` and `src/` into an STM32 project, compile every `.c` under
`src/`, chip folders included, add `include/` and `src/` to the include path
(`src/` only needs to reach this library's own sources), and define what the
forge would: `CMSIS_device_header` as your family header (`"stm32f4xx.h"` or
`"stm32f7xx.h"`), `USE_HAL_DRIVER`, and `CONFIG_ENCODER_<CHIP>=1` for each
chip you want.

**No chip driver is built by default.** Like a kernel subsystem, the core is
always built and each driver is opt-in: a chip whose option is absent or 0
compiles to nothing. With the library on and no chip picked, the ESP-IDF
CMakeLists warns and `src/encoder_port_stm.c` emits a `#warning`.

**The HAL modules are yours to enable.** The drivers call the HAL's I2C, SPI
and TIM, so your app's `stm32f4xx_hal_conf.h` (or `stm32f7xx_hal_conf.h`)
must define the module each chip uses, and its `.c` must be in the build:

| Option | Needs in hal_conf | HAL source |
|---|---|---|
| `ENCODER_AS5600` | `HAL_I2C_MODULE_ENABLED` | `stm32f{4,7}xx_hal_i2c.c` |
| `ENCODER_AS5047P` | `HAL_SPI_MODULE_ENABLED` | `stm32f{4,7}xx_hal_spi.c` |
| `ENCODER_AMT102` | `HAL_TIM_MODULE_ENABLED` | `stm32f{4,7}xx_hal_tim.c` |

A chip whose module is off stops the build with an `#error` in
`src/encoder_priv.h` that says which define is missing. The bus layer's
UART code sits under `HAL_UART_MODULE_ENABLED` and builds to nothing
without it.

**The AMT102's timer is the app's.** Set it up in encoder mode on both
inputs (`TIM_ENCODERMODE_TI12`), with `Init.Period` at the counter's full
range — `0xFFFF` on a 16-bit timer, `0xFFFFFFFF` on a 32-bit one — and the
input filter you want, as CubeMX generates it (`MX_TIMx_Init()`). The
driver refuses any other period: the counter would wrap early and lose
counts. It starts the timer with `HAL_TIM_Encoder_Start()` and stops it on
remove; it never changes the timer's configuration.

**printf() is the log.** Lines go out through `printf()`, so retarget
newlib's `_write()` to a UART. stm-forge links newlib-nano
(`--specs=nano.specs`), whose `printf()` drops `%f` unless the app links with
`-u _printf_float`: add `target_link_options(firmware PRIVATE -u _printf_float)`
to your app's `CMakeLists.txt`, or `encoder_log_sample()` prints blanks
where the numbers go.
