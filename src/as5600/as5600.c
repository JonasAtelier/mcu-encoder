#include "encoder_priv.h"

#if CONFIG_ENCODER_AS5600

#include <stdlib.h>

#include "as5600.h"
#include "as5600_regs.h"
#include "encoder_log.h"

#define I2C_TIMEOUT_MS		100

struct as5600 {
	struct encoder_bus bus;
	int32_t zero;		/* counts subtracted from every reported angle */
	struct encoder_turns turns;
	bool invert;
};

static const struct encoder_ops as5600_ops;

/* Our state, or NULL if this encoder_dev was not probed by this driver. */
static struct as5600 *to_as5600(struct encoder_dev *encoder)
{
	if (!encoder || encoder->ops != &as5600_ops)
		return NULL;
	return encoder->priv;
}

/* ---------------------------------------------------------------- I2C ---- */

static int as5600_reg_read(struct as5600 *chip, uint8_t reg, uint8_t *buf,
			   size_t len)
{
	return encoder_reg_read(&chip->bus, reg, buf, len);
}

static int as5600_reg_write(struct as5600 *chip, uint8_t reg,
			    const uint8_t *values, size_t len)
{
	return chip->bus.ops->transfer(&chip->bus, reg, values, len, NULL, 0);
}

/* Read one of the 12-bit angle registers, applying the software direction. */
static int as5600_read_12bit(struct as5600 *chip, uint8_t reg,
			     uint16_t *counts)
{
	uint8_t buf[2];
	uint16_t value;
	int ret;

	ret = as5600_reg_read(chip, reg, buf, sizeof(buf));
	if (ret)
		return ret;

	value = (uint16_t)(((uint16_t)(buf[0] & 0x0F) << 8) | buf[1]);
	if (chip->invert)
		value = (uint16_t)encoder_invert(value, AS5600_COUNTS);

	*counts = value;
	return 0;
}

/*
 * Read-modify-write, because the datasheet warns that blank CONF fields may
 * hold factory settings we must not clear (Figure 21, note 1).
 */
static int as5600_apply_config(struct as5600 *chip,
			       const struct as5600_config *cfg)
{
	uint8_t conf[2];
	int ret;

	ret = as5600_reg_read(chip, AS5600_CONF_H, conf, sizeof(conf));
	if (ret)
		return ret;

	conf[0] = (uint8_t)((conf[0] & ~AS5600_CONF_SF_MASK) |
			    ((uint8_t)cfg->filter & AS5600_CONF_SF_MASK));
	conf[1] = (uint8_t)((conf[1] & ~AS5600_CONF_HYST_MASK) |
			    (((uint8_t)cfg->hysteresis << AS5600_CONF_HYST_SHIFT) &
			     AS5600_CONF_HYST_MASK));

	return as5600_reg_write(chip, AS5600_CONF_H, conf, sizeof(conf));
}

/* -------------------------------------------------------------- ops ------ */

/* encoder_ops.read: one ANGLE read, the turn count kept up to date. */
static int as5600_read(struct encoder_dev *encoder, int64_t *counts)
{
	struct as5600 *chip = encoder->priv;
	uint16_t angle;
	int ret;

	ret = as5600_read_12bit(chip, AS5600_ANGLE_H, &angle);
	if (ret)
		return ret;

	encoder_track_turns(&chip->turns, angle, AS5600_COUNTS);
	*counts = (int64_t)chip->turns.revolutions * AS5600_COUNTS + angle -
		  chip->zero;
	return 0;
}

static int as5600_set_zero(struct encoder_dev *encoder)
{
	struct as5600 *chip = encoder->priv;
	uint16_t angle;
	int ret;

	ret = as5600_read_12bit(chip, AS5600_ANGLE_H, &angle);
	if (ret)
		return ret;

	encoder_track_turns(&chip->turns, angle, AS5600_COUNTS);
	chip->zero = angle;
	chip->turns.revolutions = 0;
	return 0;
}

static const struct encoder_ops as5600_ops = {
	.read = as5600_read,
	.set_zero = as5600_set_zero,
};

/* ----------------------------------------------------------- extras ------ */

int as5600_read_counts(struct encoder_dev *encoder, uint16_t *counts)
{
	struct as5600 *chip = to_as5600(encoder);

	if (!chip || !counts)
		return -EINVAL;

	return as5600_read_12bit(chip, AS5600_RAW_ANGLE_H, counts);
}

int as5600_read_status(struct encoder_dev *encoder,
		       struct as5600_status *status)
{
	struct as5600 *chip = to_as5600(encoder);
	uint8_t flags;
	uint8_t agc;
	uint8_t magnitude[2];
	int ret;

	if (!chip || !status)
		return -EINVAL;

	ret = as5600_reg_read(chip, AS5600_STATUS, &flags, 1);
	if (ret)
		return ret;

	ret = as5600_reg_read(chip, AS5600_AGC, &agc, 1);
	if (ret)
		return ret;

	ret = as5600_reg_read(chip, AS5600_MAGNITUDE_H, magnitude,
			      sizeof(magnitude));
	if (ret)
		return ret;

	status->magnet_detected = (flags & AS5600_STATUS_MD) != 0;
	status->magnet_too_weak = (flags & AS5600_STATUS_ML) != 0;
	status->magnet_too_strong = (flags & AS5600_STATUS_MH) != 0;
	status->agc = agc;
	status->magnitude = (uint16_t)(((uint16_t)(magnitude[0] & 0x0F) << 8) |
				       magnitude[1]);
	return 0;
}

/* ------------------------------------------------------- probe/remove ---- */

#ifdef ESP_PLATFORM
#define as5600_scl_hz(cfg)	((cfg)->scl_speed_hz)
#else
#define as5600_scl_hz(cfg)	0	/* the handle's */
#endif

int as5600_probe(struct encoder_dev *encoder, encoder_i2c_bus bus,
		 const struct as5600_config *cfg)
{
	static const struct as5600_config defaults = { 0 };
	struct as5600 *chip;
	uint16_t angle;
	int ret;

	if (!encoder || !bus)
		return -EINVAL;
	/* Bound already: a second probe would leak the first binding. */
	if (encoder->ops || encoder->priv)
		return -EBUSY;

	encoder->name = "as5600";

	if (!cfg)
		cfg = &defaults;
	if ((unsigned int)cfg->filter > AS5600_FILTER_2X ||
	    (unsigned int)cfg->hysteresis > AS5600_HYST_3LSB) {
		encoder_err(encoder, "filter %d or hysteresis %d out of range",
			    (int)cfg->filter, (int)cfg->hysteresis);
		return -EINVAL;
	}

	/*
	 * A NACKed transfer comes back as -EIO, which says nothing about why;
	 * the presence check is the one call that reports "nobody at this
	 * address" as such. Ask it before allocating anything.
	 */
	ret = encoder_i2c_probe(bus, AS5600_ADDR, I2C_TIMEOUT_MS);
	if (ret) {
		encoder_err(encoder, "nothing answers at 0x%02x (%d)",
			    AS5600_ADDR, ret);
		return ret;
	}

	chip = calloc(1, sizeof(*chip));
	if (!chip)
		return -ENOMEM;

	chip->invert = cfg->invert;

	ret = encoder_i2c_attach(&chip->bus, bus, AS5600_ADDR,
				 as5600_scl_hz(cfg), I2C_TIMEOUT_MS);
	if (ret) {
		encoder_err(encoder, "cannot add 0x%02x to the bus (%d)",
			    AS5600_ADDR, ret);
		free(chip);
		return ret;
	}

	/* It ACKed its address; check it answers a register read too. */
	ret = as5600_read_12bit(chip, AS5600_ANGLE_H, &angle);
	if (ret) {
		encoder_err(encoder, "cannot read ANGLE at 0x%02x (%d)",
			    AS5600_ADDR, ret);
		goto fail;
	}

	ret = as5600_apply_config(chip, cfg);
	if (ret) {
		encoder_err(encoder, "cannot set filter and hysteresis (%d)",
			    ret);
		goto fail;
	}

	encoder->ops = &as5600_ops;
	encoder->priv = chip;
	encoder->counts_per_turn = AS5600_COUNTS;
	encoder->absolute = true;

	encoder_info(encoder, "at 0x%02x", AS5600_ADDR);
	return 0;

fail:
	encoder_bus_detach(&chip->bus);
	free(chip);
	return ret;
}

void as5600_remove(struct encoder_dev *encoder)
{
	struct as5600 *chip = to_as5600(encoder);

	if (!chip)
		return;

	encoder_bus_detach(&chip->bus);
	free(chip);
	encoder->ops = NULL;
	encoder->priv = NULL;
	encoder->counts_per_turn = 0;
	encoder->absolute = false;
}

#endif /* CONFIG_ENCODER_AS5600 */
