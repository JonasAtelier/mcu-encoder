# mcu-encoder

Measure shaft angle and track turns with an AS5600 (I2C), AS5047P (SPI) or AMT102-V (quadrature).

**Platforms** · ESP-IDF 5.2+ and STM32 HAL (F4/F7). Hardware testing is pending. The host tests use simulated devices.

## Contents

- [Quick start](#quick-start)
- [Drivers and examples](#drivers-and-examples)
- [Using the library in your project](#using-the-library-in-your-project)
- [License](#license)

## Quick start

Wire one AS5600 on the I2C bus at its fixed address, 0x36, and centre a diametrically magnetised magnet over the sensor.

In a terminal with ESP-IDF activated, get the example and select the target for an ESP32. Change the target if your board uses another ESP32 chip.

```sh
git clone https://github.com/JonasAtelier/mcu-encoder.git encoder
cd encoder/examples/esp/as5600
idf.py set-target esp32
```

Check the [wiring guide](docs/hardware.md) and edit the settings in [main.c](examples/esp/as5600/main/main.c) to match your board. Connect the board by USB, then build, upload and open the serial monitor.

```sh
idf.py build flash monitor
```

The example checks the magnet, zeros the shaft and streams angle and total travel. Read an absolute encoder before it moves half a turn to keep the software turn count accurate. Zero and turn history live in RAM.

<details>
<summary>STM32 HAL example for an initialised bus</summary>

Initialise the bus in your application, then probe the sensor and read it.
This STM32 helper assumes the passed SPI handle is already initialised in
mode 1 with 8-bit frames, and the chip select pin set up as an output:

```c
#include "as5047p.h"
#include "encoder.h"
#include "encoder_log.h"

int read_once(SPI_HandleTypeDef *spi, GPIO_TypeDef *cs_port, uint16_t cs_pin)
{
    const struct as5047p_config cfg = {
        .spi = spi, .cs_port = cs_port, .cs_pin = cs_pin,
    };
    struct encoder_dev encoder = { 0 }; /* Must start zeroed. */
    struct encoder_sample sample;
    int ret = as5047p_probe(&encoder, &cfg);
    if (ret)
        return ret;

    ret = encoder_read(&encoder, &sample);
    if (!ret)
        encoder_log_sample(&encoder, &sample);

    as5047p_remove(&encoder);
    return ret;
}
```

</details>

## Drivers and examples

The magnetic encoders return an absolute shaft angle. The AMT102-V counts motion from a starting position and has an index pulse once per turn. All three support `encoder_read()` and `encoder_set_zero()`. [Shared API](include/encoder.h)

| Chip | Driver header | ESP-IDF project | STM32 HAL example |
|---|---|---|---|
| [AS5600](docs/chips.md#as5600) | [as5600.h](include/as5600.h) | [I2C angle and turns](examples/esp/as5600/) | [I2C angle and turns](examples/stm/as5600/main.c) |
| [AS5047P](docs/chips.md#as5047p) | [as5047p.h](include/as5047p.h) | [SPI angle and turns](examples/esp/as5047p/) | [SPI angle and turns](examples/stm/as5047p/main.c) |
| [AMT102-V](docs/chips.md#amt102-v) | [amt102.h](include/amt102.h) | [PCNT quadrature counter](examples/esp/amt102/) | [Timer in encoder mode](examples/stm/amt102/main.c) |

## Using the library in your project

- Add the component and enable the driver for your hardware. [Installation and build options](docs/installation.md)
- Open the ESP-IDF project for your hardware, or copy the STM32 example into a project with board setup already in place. [All ESP-IDF examples](examples/esp/) · [All STM32 examples](examples/stm/)
- Look up calls and chip-specific settings. [API usage](docs/usage.md) · [Hardware reference](docs/chips.md)

To run the simulated tests on your computer, run `make -C test test` from the library root. [Test requirements and coverage](docs/development.md)

## License

© 2026 JonasAtelier, [GPL-3.0-only](LICENSE)

<p align="center">
  <img src="assets/angle-concept.svg" width="760" alt="Magnet orientation maps to a 14-bit count: 0° = 0, 90° = 4096, 180° = 8192, 270° = 12288. After 16383 the count wraps to zero.">
</p>

[Browse the workshop catalog · ESP32 and STM32 drivers](https://github.com/JonasAtelier/jonas-atelier-libraries#esp32-and-stm32-drivers)
