/*
 * as5047p -- ams AS5047P 14-bit magnetic rotary encoder, as an encoder chip
 * driver.
 *
 * A diametrically magnetised magnet spins above the chip and it reports the
 * absolute angle over SPI -- 16384 counts per revolution, about 0.022 degrees
 * each. No index pulse, no homing move: the angle is right the instant it
 * powers up, even if the shaft turned while the board was off.
 *
 * Built for motors. It is specified to 28000 rpm, and its DAEC feature
 * predicts out the delay between sampling and answering, so the angle you
 * read at speed is the angle *now* rather than the angle a few microseconds
 * ago.
 *
 * SPI only. The chip also drives ABI/UVW/PWM outputs; those are not touched
 * here.
 *
 * Angles come out of encoder_read(). Everything below that is AS5047P
 * specific takes the struct encoder_dev that as5047p_probe() filled, and
 * returns -EINVAL if handed a device some other driver probed.
 */
#ifndef AS5047P_H
#define AS5047P_H

#include <stdbool.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "driver/gpio.h"
#include "driver/spi_master.h"
#else
#include CMSIS_device_header
#endif

#include "encoder.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AS5047P_COUNTS	16384	/* one full turn */

struct as5047p_config {
#ifdef ESP_PLATFORM
	spi_host_device_t host;	/* an SPI bus you have already initialised */
	gpio_num_t cs_gpio;
	int spi_hz;		/* 0 selects 4 MHz. The chip tops out at 10 MHz. */
#else
	/*
	 * An SPI handle the app has HAL_SPI_Init()ed in mode 1 (CPOL low,
	 * CPHA second edge) with 8-bit frames, at 10 MHz or less; the clock
	 * is the handle's. Chip select is software: an output the app has set
	 * up, idle high.
	 */
	SPI_HandleTypeDef *spi;
	GPIO_TypeDef *cs_port;
	uint16_t cs_pin;
#endif
	bool invert;		/* count the other way round, in software */

	/*
	 * Skip the chip's dynamic angle error compensation and read the plain
	 * measured angle. DAEC is on by default and is what you want on a
	 * spinning shaft; set this if the shaft is mostly still, or if you
	 * would rather not have the chip extrapolating on your behalf.
	 */
	bool raw_angle;
};

struct as5047p_status {
	bool magnet_too_weak;	/* MAGL: too far away, or too small */
	bool magnet_too_strong;	/* MAGH: too close */
	bool cordic_overflow;	/* COF: the angle output is meaningless */
	bool offset_ready;	/* LF: the chip has finished settling */
	uint8_t agc;		/* gain in use, 0-255: aim for mid-range by
				 * adjusting the air gap */
	uint16_t magnitude;	/* raw field strength seen by the chip */
};

/*
 * Attach to an already-initialised SPI bus and check the chip answers. The
 * zero point starts at the chip's own zero, not wherever the shaft is: call
 * encoder_set_zero() for that. Fills encoder->name, ops, priv,
 * counts_per_turn and absolute.
 *
 * -EBUSY if encoder is already bound (see struct encoder_dev); nothing is
 * touched. On the STM32 HAL, -EINVAL for a NULL handle or chip select port,
 * or a handle not in mode 1 with 8-bit frames, before the bus is touched.
 * On ESP-IDF, -ENOSPC if the SPI host has no free CS slot left. -ENXIO if MISO
 * reads stuck all-zeros or all-ones, which is what no chip looks like -- the
 * part has no ID register. -EBADMSG for a reply with bad parity, -EIO if the
 * chip flagged an error (EF). Other bus failures come back as the errno of
 * the failed call.
 *
 * After probe, a read that gets EF also reads ERRFL, which clears it: that
 * read returns -EIO and the next one is clean.
 */
int as5047p_probe(struct encoder_dev *encoder,
		  const struct as5047p_config *cfg);

/* Releases the SPI device. Safe on an unprobed encoder_dev. */
void as5047p_remove(struct encoder_dev *encoder);

/*
 * Unmodified 0-16383 reading straight from the chip, direction applied
 * ((16384 - count) mod 16384 with invert): no zero offset, no wrap tracking.
 */
int as5047p_read_counts(struct encoder_dev *encoder, uint16_t *counts);

/*
 * Is the magnet placed well? Check this once at startup, and when readings
 * look wrong.
 */
int as5047p_read_status(struct encoder_dev *encoder,
			struct as5047p_status *status);

#ifdef __cplusplus
}
#endif

#endif /* AS5047P_H */
