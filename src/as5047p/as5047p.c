#include "encoder_priv.h"

#if CONFIG_ENCODER_AS5047P

#include <stdlib.h>

#include "as5047p.h"
#include "as5047p_math.h"
#include "as5047p_regs.h"
#include "encoder_log.h"

#define DEFAULT_SPI_HZ		4000000
#define SPI_TIMEOUT_MS		10

struct as5047p {
	struct encoder_bus bus;
	uint16_t angle_reg;	/* ANGLECOM or ANGLEUNC */
	int32_t zero;		/* counts subtracted from every reported angle */
	struct encoder_turns turns;
	bool invert;
};

static const struct encoder_ops as5047p_ops;

/* Our state, or NULL if this encoder_dev was not probed by this driver. */
static struct as5047p *to_as5047p(struct encoder_dev *encoder)
{
	if (!encoder || encoder->ops != &as5047p_ops)
		return NULL;
	return encoder->priv;
}

/* ---------------------------------------------------------------- SPI ---- */

/* One 16-bit frame out, MSB first, and what came back during it. */
static int as5047p_transfer(struct as5047p *chip, uint16_t frame,
			    uint16_t *reply)
{
	uint8_t tx[2];
	uint8_t rx[2];
	int ret;

	frame = as5047p_with_parity(frame);
	tx[0] = (uint8_t)(frame >> 8);
	tx[1] = (uint8_t)frame;

	ret = encoder_bus_exchange(&chip->bus, tx, rx, sizeof(tx));
	if (ret)
		return ret;

	if (reply)
		*reply = (uint16_t)(((uint16_t)rx[0] << 8) | rx[1]);
	return 0;
}

/* Address out, NOP behind it to clock the answer back. The reply, undecoded. */
static int as5047p_read_frame(struct as5047p *chip, uint16_t reg,
			      uint16_t *reply)
{
	int ret;

	ret = as5047p_transfer(chip, (uint16_t)(reg | AS5047P_CMD_READ), NULL);
	if (ret)
		return ret;

	return as5047p_transfer(chip, (uint16_t)(AS5047P_NOP | AS5047P_CMD_READ),
				reply);
}

static int as5047p_decode(uint16_t reply, uint16_t *value)
{
	if (reply != as5047p_with_parity(reply))
		return -EBADMSG;

	/*
	 * EF means the chip refused the command -- bad parity or bad address
	 * on the wire. Details are in ERRFL, which reading also clears.
	 */
	if (reply & AS5047P_FRAME_ERROR)
		return -EIO;

	*value = reply & AS5047P_FRAME_DATA;
	return 0;
}

static int as5047p_reg_read(struct as5047p *chip, uint16_t reg,
			    uint16_t *value)
{
	uint16_t reply;
	int ret;

	ret = as5047p_read_frame(chip, reg, &reply);
	if (ret)
		return ret;

	ret = as5047p_decode(reply, value);
	/*
	 * EF stays set in every reply until ERRFL is read. Without this, one
	 * glitched frame would fail every later read until a re-probe. This
	 * read still fails; the next one gets a clean frame. Bad parity is not
	 * EF and gets no extra read.
	 */
	if (ret == -EIO)
		as5047p_read_frame(chip, AS5047P_ERRFL, &reply);
	return ret;
}

/* Read the angle register, applying the software direction. */
static int as5047p_read_angle_register(struct as5047p *chip, uint16_t *counts)
{
	uint16_t value;
	int ret;

	ret = as5047p_reg_read(chip, chip->angle_reg, &value);
	if (ret)
		return ret;

	if (chip->invert)
		value = (uint16_t)encoder_invert(value, AS5047P_COUNTS);

	*counts = value;
	return 0;
}

/* -------------------------------------------------------------- ops ------ */

/* encoder_ops.read: one angle read, the turn count kept up to date. */
static int as5047p_read(struct encoder_dev *encoder, int64_t *counts)
{
	struct as5047p *chip = encoder->priv;
	uint16_t angle;
	int ret;

	ret = as5047p_read_angle_register(chip, &angle);
	if (ret)
		return ret;

	encoder_track_turns(&chip->turns, angle, AS5047P_COUNTS);
	*counts = (int64_t)chip->turns.revolutions * AS5047P_COUNTS + angle -
		  chip->zero;
	return 0;
}

static int as5047p_set_zero(struct encoder_dev *encoder)
{
	struct as5047p *chip = encoder->priv;
	uint16_t angle;
	int ret;

	ret = as5047p_read_angle_register(chip, &angle);
	if (ret)
		return ret;

	encoder_track_turns(&chip->turns, angle, AS5047P_COUNTS);
	chip->zero = angle;
	chip->turns.revolutions = 0;
	return 0;
}

static const struct encoder_ops as5047p_ops = {
	.read = as5047p_read,
	.set_zero = as5047p_set_zero,
};

/* ----------------------------------------------------------- extras ------ */

int as5047p_read_counts(struct encoder_dev *encoder, uint16_t *counts)
{
	struct as5047p *chip = to_as5047p(encoder);

	if (!chip || !counts)
		return -EINVAL;

	return as5047p_read_angle_register(chip, counts);
}

int as5047p_read_status(struct encoder_dev *encoder,
			struct as5047p_status *status)
{
	struct as5047p *chip = to_as5047p(encoder);
	uint16_t diaagc;
	uint16_t magnitude;
	int ret;

	if (!chip || !status)
		return -EINVAL;

	ret = as5047p_reg_read(chip, AS5047P_DIAAGC, &diaagc);
	if (ret)
		return ret;

	ret = as5047p_reg_read(chip, AS5047P_MAG, &magnitude);
	if (ret)
		return ret;

	status->magnet_too_weak = (diaagc & AS5047P_DIAAGC_MAGL) != 0;
	status->magnet_too_strong = (diaagc & AS5047P_DIAAGC_MAGH) != 0;
	status->cordic_overflow = (diaagc & AS5047P_DIAAGC_COF) != 0;
	status->offset_ready = (diaagc & AS5047P_DIAAGC_LF) != 0;
	status->agc = (uint8_t)(diaagc & AS5047P_DIAAGC_AGC);
	status->magnitude = magnitude;
	return 0;
}

/* ------------------------------------------------------- probe/remove ---- */

#ifdef ESP_PLATFORM
/* The SPI device on the app's host, in mode 1 at cfg's clock. */
static int as5047p_attach(struct as5047p *chip,
			  const struct as5047p_config *cfg)
{
	uint32_t hz = cfg->spi_hz > 0 ? (uint32_t)cfg->spi_hz : DEFAULT_SPI_HZ;
	int ret;

	ret = encoder_spi_attach(&chip->bus, cfg->host, cfg->cs_gpio, hz,
				 AS5047P_SPI_MODE);
	/* NOT_FOUND here is "no free CS slot on this host", not "no chip". */
	return ret == -ENXIO ? -ENOSPC : ret;
}

/* Where the chip is, for the log. */
#define AS5047P_WHERE_FMT	"CS %d"
#define AS5047P_WHERE(cfg)	(int)(cfg)->cs_gpio
#else
/*
 * The app's handle must already be in mode 1 with 8-bit frames: the HAL
 * keeps those in the handle, and nothing here changes them.
 */
static int as5047p_check_handle(const struct as5047p_config *cfg)
{
	const SPI_HandleTypeDef *spi = cfg->spi;

	if (!spi || !cfg->cs_port)
		return -EINVAL;
	if (spi->Init.CLKPolarity != SPI_POLARITY_LOW ||
	    spi->Init.CLKPhase != SPI_PHASE_2EDGE ||
	    spi->Init.DataSize != SPI_DATASIZE_8BIT)
		return -EINVAL;
	return 0;
}

static int as5047p_attach(struct as5047p *chip,
			  const struct as5047p_config *cfg)
{
	return encoder_spi_attach(&chip->bus, cfg->spi, cfg->cs_port,
				  cfg->cs_pin, SPI_TIMEOUT_MS);
}

#define AS5047P_WHERE_FMT	"CS pin 0x%04x"
#define AS5047P_WHERE(cfg)	(unsigned int)(cfg)->cs_pin
#endif

int as5047p_probe(struct encoder_dev *encoder,
		  const struct as5047p_config *cfg)
{
	struct as5047p *chip;
	uint16_t reply;
	uint16_t diaagc;
	int ret;

	if (!encoder || !cfg)
		return -EINVAL;
	/* Bound already: a second probe would leak the first binding. */
	if (encoder->ops || encoder->priv)
		return -EBUSY;

	encoder->name = "as5047p";

#ifndef ESP_PLATFORM
	ret = as5047p_check_handle(cfg);
	if (ret) {
		encoder_err(encoder, "SPI handle must be mode 1, 8-bit frames");
		return ret;
	}
#endif

	chip = calloc(1, sizeof(*chip));
	if (!chip)
		return -ENOMEM;

	chip->invert = cfg->invert;
	chip->angle_reg = cfg->raw_angle ? AS5047P_ANGLEUNC : AS5047P_ANGLECOM;

	ret = as5047p_attach(chip, cfg);
	if (ret) {
		encoder_err(encoder, "cannot add " AS5047P_WHERE_FMT
			    " to the bus (%d)", AS5047P_WHERE(cfg), ret);
		free(chip);
		return ret;
	}

	/*
	 * The first frame after power-up often reports a framing error against
	 * a chip that is perfectly fine. Read ERRFL once to clear it, and
	 * ignore what it says.
	 */
	as5047p_read_frame(chip, AS5047P_ERRFL, &reply);

	/*
	 * There is no ID register, so this is the check: a chip that answers
	 * returns something that is neither all-zeros nor all-ones. Look at the
	 * raw frame before decoding it: MISO stuck high reads 0xFFFF, which has
	 * good parity and EF set, and would otherwise pass for an error frame.
	 */
	ret = as5047p_read_frame(chip, AS5047P_DIAAGC, &reply);
	if (ret) {
		encoder_err(encoder, "no answer on " AS5047P_WHERE_FMT " (%d)",
			    AS5047P_WHERE(cfg), ret);
		goto fail;
	}
	if (reply == 0x0000 || reply == 0xFFFF || reply == AS5047P_FRAME_DATA) {
		encoder_err(encoder,
			    "MISO stuck at 0x%04x on " AS5047P_WHERE_FMT,
			    reply, AS5047P_WHERE(cfg));
		ret = -ENXIO;
		goto fail;
	}
	ret = as5047p_decode(reply, &diaagc);
	if (ret) {
		encoder_err(encoder, "DIAAGC reply 0x%04x is %s", reply,
			    ret == -EBADMSG ? "bad parity" : "an error frame");
		goto fail;
	}

	encoder->ops = &as5047p_ops;
	encoder->priv = chip;
	encoder->counts_per_turn = AS5047P_COUNTS;
	encoder->absolute = true;

	encoder_info(encoder, "on " AS5047P_WHERE_FMT ", %s angle",
		     AS5047P_WHERE(cfg),
		     cfg->raw_angle ? "uncompensated" : "DAEC");
	return 0;

fail:
	encoder_bus_detach(&chip->bus);
	free(chip);
	return ret;
}

void as5047p_remove(struct encoder_dev *encoder)
{
	struct as5047p *chip = to_as5047p(encoder);

	if (!chip)
		return;

	encoder_bus_detach(&chip->bus);
	free(chip);
	encoder->ops = NULL;
	encoder->priv = NULL;
	encoder->counts_per_turn = 0;
	encoder->absolute = false;
}

#endif /* CONFIG_ENCODER_AS5047P */
