/*
 * as5600 -- ams AS5600 12-bit magnetic rotary encoder, as an encoder chip
 * driver.
 *
 * Datasheet in docs/. A diametrically magnetised magnet spins above the chip
 * and it reports the absolute angle over I2C -- 4096 counts per revolution,
 * no index pulse, no homing, correct the instant it powers up.
 *
 * Angles come out of encoder_read(). Everything below that is AS5600
 * specific takes the struct encoder_dev that as5600_probe() filled, and
 * returns -EINVAL if handed a device some other driver probed.
 */
#ifndef AS5600_H
#define AS5600_H

#include <stdbool.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "driver/i2c_master.h"
#else
#include CMSIS_device_header
#endif

#include "encoder.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AS5600_ADDR	0x36	/* fixed 7-bit address; the chip has no strap */
#define AS5600_COUNTS	4096	/* one full turn */

/*
 * Slow filter: how hard the chip smooths the angle. Fewer samples react
 * faster and read noisier. 16x is the power-on default.
 */
enum as5600_filter {
	AS5600_FILTER_16X	= 0,
	AS5600_FILTER_8X	= 1,
	AS5600_FILTER_4X	= 2,
	AS5600_FILTER_2X	= 3,
};

/*
 * Output hysteresis: how far the angle must move before the output follows.
 * A count or two kills the last-bit flicker on a stationary shaft.
 */
enum as5600_hysteresis {
	AS5600_HYST_OFF		= 0,
	AS5600_HYST_1LSB	= 1,
	AS5600_HYST_2LSB	= 2,
	AS5600_HYST_3LSB	= 3,
};

struct as5600_config {
#ifdef ESP_PLATFORM
	uint32_t scl_speed_hz;	/* 0 means 400000; the AS5600 does up to 1 MHz */
#endif
	bool invert;		/* count the other way round, in software */
	enum as5600_filter filter;		/* default AS5600_FILTER_16X */
	enum as5600_hysteresis hysteresis;	/* default AS5600_HYST_OFF */
};

struct as5600_status {
	bool magnet_detected;	/* nothing else here is meaningful without it */
	bool magnet_too_weak;	/* too far away, or too small */
	bool magnet_too_strong;	/* too close */
	uint8_t agc;		/* gain in use: aim for mid-range by adjusting
				 * the air gap. 0-255 at 5V, 0-128 at 3.3V */
	uint16_t magnitude;	/* raw field strength seen by the chip */
};

/*
 * Attach to an already-initialised I2C bus, check the chip answers, then
 * apply the filter and hysteresis. bus is, on ESP-IDF, an i2c_master bus;
 * the driver adds its device at cfg->scl_speed_hz. On the STM32 HAL it is an
 * I2C handle the app has HAL_I2C_Init()ed; the bus speed is the handle's.
 * cfg may be NULL for defaults. The zero point starts at the chip's own
 * zero: call encoder_set_zero() to move it. Fills encoder->name, ops, priv,
 * counts_per_turn and absolute.
 *
 * -EBUSY if encoder is already bound (see struct encoder_dev); nothing is
 * touched. -EINVAL for a NULL encoder or bus, or a filter or hysteresis
 * outside its enum. -ENXIO if nothing ACKs at 0x36 (asked with
 * i2c_master_probe() or HAL_I2C_IsDeviceReady(), before any allocation).
 * Other bus failures come back as the errno of the failed call; a NACK in
 * the middle of a transfer, then or later, is -EIO. There is no ID
 * register, so a different chip at 0x36 is not detected.
 */
#ifdef ESP_PLATFORM
int as5600_probe(struct encoder_dev *encoder, i2c_master_bus_handle_t bus,
		 const struct as5600_config *cfg);
#else
int as5600_probe(struct encoder_dev *encoder, I2C_HandleTypeDef *bus,
		 const struct as5600_config *cfg);
#endif

/* Releases the I2C device. Safe on an unprobed encoder_dev. */
void as5600_remove(struct encoder_dev *encoder);

/*
 * Unmodified 0-4095 reading, straight from the chip's RAW ANGLE register,
 * direction applied ((4096 - count) mod 4096 with invert): no zero offset,
 * no wrap tracking.
 */
int as5600_read_counts(struct encoder_dev *encoder, uint16_t *counts);

/*
 * Is the magnet placed well? Check this once at startup, and when readings
 * look wrong.
 */
int as5600_read_status(struct encoder_dev *encoder,
		       struct as5600_status *status);

#ifdef __cplusplus
}
#endif

#endif /* AS5600_H */
