/*
 * The AMT102-V counts in hardware, outside the bus layer: on the ESP32's
 * PCNT, which this driver sets up on the app's GPIOs, or on an STM32 timer
 * the app has already put in encoder mode, which this driver only starts,
 * reads and stops. The ops and the extras are the same on both.
 */
#include "encoder_priv.h"

#if CONFIG_ENCODER_AMT102

#include <stdlib.h>

#ifdef ESP_PLATFORM
#include "driver/pulse_cnt.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#else
#include "amt102_math.h"
#endif

#include "amt102.h"
#include "encoder_log.h"

#define DEFAULT_PPR		2048

#ifdef ESP_PLATFORM
/*
 * The hardware counter is 16-bit. Watch points at these limits let the PCNT
 * driver fold each overflow into a 32-bit accumulator for us, so the count
 * only wraps after ~2 billion, not every 8 turns.
 */
#define PCNT_HIGH		16384
#define PCNT_LOW		(-PCNT_HIGH)

#define DEFAULT_GLITCH_NS	1000
#endif

/* The count as the hardware gives it: PCNT accumulates 32 bits, a timer is
 * widened to 64 here. */
#ifdef ESP_PLATFORM
typedef int32_t amt102_count_t;
#else
typedef int64_t amt102_count_t;
#endif

struct amt102 {
#ifdef ESP_PLATFORM
	pcnt_unit_handle_t unit;
	pcnt_channel_handle_t channel_a;
	pcnt_channel_handle_t channel_b;
	gpio_num_t z_gpio;
	bool isr_added;		/* our handler is on z_gpio; remove only then */
	/*
	 * index_hit is set by the ISR, possibly on the other core, and
	 * read-and-cleared by a task. Without the lock an edge landing between
	 * the read and the clear is lost.
	 */
	portMUX_TYPE lock;
#else
	TIM_HandleTypeDef *tim;
	uint32_t last;		/* the counter at the previous reading */
	int64_t total;		/* counts since probe, widened to 64 bits */
	bool wide;		/* a 32-bit counter, not 16 */
	bool has_index;
#endif
	amt102_count_t zero;	/* counts subtracted from every reading */
	bool invert;
	volatile bool index_hit;
};

static const struct encoder_ops amt102_ops;

/* Our state, or NULL if this encoder_dev was not probed by this driver. */
static struct amt102 *to_amt102(struct encoder_dev *encoder)
{
	if (!encoder || encoder->ops != &amt102_ops)
		return NULL;
	return encoder->priv;
}

#ifdef ESP_PLATFORM
static void IRAM_ATTR amt102_index_isr(void *arg)
{
	struct amt102 *chip = arg;

	portENTER_CRITICAL_ISR(&chip->lock);
	chip->index_hit = true;
	portEXIT_CRITICAL_ISR(&chip->lock);
}

/* The accumulated count with the software direction applied. */
static int amt102_count(struct amt102 *chip, amt102_count_t *count)
{
	int value;
	int ret;

	ret = encoder_errno(pcnt_unit_get_count(chip->unit, &value));
	if (ret)
		return ret;

	*count = chip->invert ? -(int32_t)value : (int32_t)value;
	return 0;
}
#else
/*
 * The counter, widened: each reading adds the signed move since the last,
 * modulo the counter's range, to a 64-bit total. Direction applied.
 */
static int amt102_count(struct amt102 *chip, amt102_count_t *count)
{
	uint32_t now = (uint32_t)__HAL_TIM_GET_COUNTER(chip->tim);

	chip->total += amt102_counter_delta(now, chip->last, chip->wide);
	chip->last = now;
	*count = chip->invert ? -chip->total : chip->total;
	return 0;
}
#endif

/* -------------------------------------------------------------- ops ------ */

/* encoder_ops.read: counts since the zero point, from the hardware counter. */
static int amt102_read(struct encoder_dev *encoder, int64_t *counts)
{
	struct amt102 *chip = encoder->priv;
	amt102_count_t count;
	int ret;

	ret = amt102_count(chip, &count);
	if (ret)
		return ret;

	*counts = (int64_t)count - chip->zero;
	return 0;
}

static int amt102_set_zero(struct encoder_dev *encoder)
{
	struct amt102 *chip = encoder->priv;

	return amt102_count(chip, &chip->zero);
}

static const struct encoder_ops amt102_ops = {
	.read = amt102_read,
	.set_zero = amt102_set_zero,
};

/* ----------------------------------------------------------- extras ------ */

int amt102_index_hit(struct encoder_dev *encoder, bool *hit)
{
	struct amt102 *chip = to_amt102(encoder);

	if (!chip || !hit)
		return -EINVAL;

#ifdef ESP_PLATFORM
	if (chip->z_gpio == GPIO_NUM_NC)
		return -EOPNOTSUPP;

	portENTER_CRITICAL(&chip->lock);
	*hit = chip->index_hit;
	chip->index_hit = false;
	portEXIT_CRITICAL(&chip->lock);
#else
	if (!chip->has_index)
		return -EOPNOTSUPP;

	/*
	 * The flag is set from the app's EXTI interrupt: with interrupts on,
	 * an edge landing between the read and the clear would be lost. Mask
	 * them for the two instructions, and put PRIMASK back as it was.
	 */
	{
		uint32_t primask = __get_PRIMASK();

		__disable_irq();
		*hit = chip->index_hit;
		chip->index_hit = false;
		__set_PRIMASK(primask);
	}
#endif
	return 0;
}

#ifndef ESP_PLATFORM
void amt102_index_irq(struct encoder_dev *encoder)
{
	struct amt102 *chip = to_amt102(encoder);

	if (chip)
		chip->index_hit = true;
}
#endif

/* ------------------------------------------------------- probe/remove ---- */

#ifdef ESP_PLATFORM
static esp_err_t amt102_setup_index(struct amt102 *chip)
{
	gpio_config_t io = {
		.pin_bit_mask = 1ULL << chip->z_gpio,
		.mode = GPIO_MODE_INPUT,
		.intr_type = GPIO_INTR_POSEDGE,
	};
	esp_err_t err;

	err = gpio_config(&io);
	if (err != ESP_OK)
		return err;

	/* Shared service: someone else in the application may already own it. */
	err = gpio_install_isr_service(0);
	if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
		return err;

	err = gpio_isr_handler_add(chip->z_gpio, amt102_index_isr, chip);
	if (err == ESP_OK)
		chip->isr_added = true;
	return err;
}

static esp_err_t amt102_setup_pcnt(struct amt102 *chip,
				   const struct amt102_config *cfg)
{
	pcnt_unit_config_t unit_config = {
		.high_limit = PCNT_HIGH,
		.low_limit = PCNT_LOW,
		.flags.accum_count = true,
	};
	pcnt_glitch_filter_config_t filter = {
		.max_glitch_ns = cfg->glitch_ns ? cfg->glitch_ns :
						  DEFAULT_GLITCH_NS,
	};
	pcnt_chan_config_t channel_a_config = {
		.edge_gpio_num = cfg->a_gpio,
		.level_gpio_num = cfg->b_gpio,
	};
	pcnt_chan_config_t channel_b_config = {
		.edge_gpio_num = cfg->b_gpio,
		.level_gpio_num = cfg->a_gpio,
	};
	esp_err_t err;

	err = pcnt_new_unit(&unit_config, &chip->unit);
	if (err != ESP_OK)
		return err;

	err = pcnt_unit_set_glitch_filter(chip->unit, &filter);
	if (err != ESP_OK)
		return err;

	err = pcnt_new_channel(chip->unit, &channel_a_config, &chip->channel_a);
	if (err != ESP_OK)
		return err;

	err = pcnt_new_channel(chip->unit, &channel_b_config, &chip->channel_b);
	if (err != ESP_OK)
		return err;

	/*
	 * 4x decoding: each channel counts both of its own edges, and the
	 * other channel's level decides which way.
	 */
	err = pcnt_channel_set_edge_action(chip->channel_a,
					   PCNT_CHANNEL_EDGE_ACTION_DECREASE,
					   PCNT_CHANNEL_EDGE_ACTION_INCREASE);
	if (err != ESP_OK)
		return err;

	err = pcnt_channel_set_level_action(chip->channel_a,
					    PCNT_CHANNEL_LEVEL_ACTION_KEEP,
					    PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
	if (err != ESP_OK)
		return err;

	err = pcnt_channel_set_edge_action(chip->channel_b,
					   PCNT_CHANNEL_EDGE_ACTION_INCREASE,
					   PCNT_CHANNEL_EDGE_ACTION_DECREASE);
	if (err != ESP_OK)
		return err;

	err = pcnt_channel_set_level_action(chip->channel_b,
					    PCNT_CHANNEL_LEVEL_ACTION_KEEP,
					    PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
	if (err != ESP_OK)
		return err;

	/* accum_count only works at points the unit is told to watch. */
	err = pcnt_unit_add_watch_point(chip->unit, PCNT_HIGH);
	if (err != ESP_OK)
		return err;

	err = pcnt_unit_add_watch_point(chip->unit, PCNT_LOW);
	if (err != ESP_OK)
		return err;

	err = pcnt_unit_enable(chip->unit);
	if (err != ESP_OK)
		return err;

	err = pcnt_unit_clear_count(chip->unit);
	if (err != ESP_OK)
		return err;

	return pcnt_unit_start(chip->unit);
}

/* Undo whatever part of the setup happened, then free. */
static void amt102_release(struct amt102 *chip)
{
	/* Never someone else's handler on the same pin. */
	if (chip->isr_added)
		gpio_isr_handler_remove(chip->z_gpio);

	if (chip->unit) {
		pcnt_unit_stop(chip->unit);
		pcnt_unit_disable(chip->unit);
	}

	if (chip->channel_a)
		pcnt_del_channel(chip->channel_a);

	if (chip->channel_b)
		pcnt_del_channel(chip->channel_b);

	if (chip->unit)
		pcnt_del_unit(chip->unit);

	free(chip);
}

int amt102_probe(struct encoder_dev *encoder,
		 const struct amt102_config *cfg)
{
	struct amt102 *chip;
	int32_t counts_per_turn;
	esp_err_t err;
	int ret;

	if (!encoder || !cfg)
		return -EINVAL;
	/* Bound already: a second probe would leak the first binding. */
	if (encoder->ops || encoder->priv)
		return -EBUSY;

	encoder->name = "amt102";

	if (cfg->a_gpio == cfg->b_gpio ||
	    (cfg->z_gpio != GPIO_NUM_NC &&
	     (cfg->z_gpio == cfg->a_gpio || cfg->z_gpio == cfg->b_gpio))) {
		encoder_err(encoder, "A %d, B %d and X %d must differ",
			    (int)cfg->a_gpio, (int)cfg->b_gpio,
			    (int)cfg->z_gpio);
		return -EINVAL;
	}

	chip = calloc(1, sizeof(*chip));
	if (!chip)
		return -ENOMEM;
	/* All-zero is not an unlocked spinlock on the real part. */
	portMUX_INITIALIZE(&chip->lock);

	chip->invert = cfg->invert;
	chip->z_gpio = cfg->z_gpio;
	counts_per_turn = (int32_t)(cfg->ppr ? cfg->ppr : DEFAULT_PPR) * 4;

	err = amt102_setup_pcnt(chip, cfg);
	if (err == ESP_ERR_NOT_FOUND) {
		encoder_err(encoder, "no free PCNT unit or channel");
		ret = -ENOSPC;
		goto fail;
	}
	ret = encoder_errno(err);
	if (ret) {
		encoder_err(encoder, "cannot set up PCNT (%d)", ret);
		goto fail;
	}

	if (chip->z_gpio != GPIO_NUM_NC) {
		ret = encoder_errno(amt102_setup_index(chip));
		if (ret) {
			encoder_err(encoder, "cannot set up index on GPIO %d (%d)",
				    (int)chip->z_gpio, ret);
			goto fail;
		}
	}

	encoder->ops = &amt102_ops;
	encoder->priv = chip;
	encoder->counts_per_turn = counts_per_turn;
	encoder->absolute = false;

	encoder_info(encoder, "A %d B %d X %d, %ld counts per turn",
		     (int)cfg->a_gpio, (int)cfg->b_gpio, (int)cfg->z_gpio,
		     (long)counts_per_turn);
	return 0;

fail:
	amt102_release(chip);
	return ret;
}

void amt102_remove(struct encoder_dev *encoder)
{
	struct amt102 *chip = to_amt102(encoder);

	if (!chip)
		return;

	amt102_release(chip);
	encoder->ops = NULL;
	encoder->priv = NULL;
	encoder->counts_per_turn = 0;
	encoder->absolute = false;
}

#else /* STM32 HAL */

int amt102_probe(struct encoder_dev *encoder,
		 const struct amt102_config *cfg)
{
	struct amt102 *chip;
	int32_t counts_per_turn;
	uint32_t period;
	int ret;

	if (!encoder || !cfg)
		return -EINVAL;
	/* Bound already: a second probe would leak the first binding. */
	if (encoder->ops || encoder->priv)
		return -EBUSY;

	encoder->name = "amt102";

	/*
	 * The counter must run its full range, so that the widening in
	 * amt102_counter_delta() is exact; any other auto-reload would wrap
	 * early and lose counts.
	 */
	if (!cfg->tim)
		return -EINVAL;
	period = cfg->tim->Init.Period;
	if (period != 0xFFFFu && period != 0xFFFFFFFFu) {
		encoder_err(encoder, "timer Period 0x%lx: must be 0xFFFF or "
			    "0xFFFFFFFF", (unsigned long)period);
		return -EINVAL;
	}

	chip = calloc(1, sizeof(*chip));
	if (!chip)
		return -ENOMEM;

	chip->tim = cfg->tim;
	chip->wide = period == 0xFFFFFFFFu;
	chip->invert = cfg->invert;
	chip->has_index = cfg->index;
	counts_per_turn = (int32_t)(cfg->ppr ? cfg->ppr : DEFAULT_PPR) * 4;

	ret = encoder_hal_errno(HAL_TIM_Encoder_Start(chip->tim,
						      TIM_CHANNEL_ALL));
	if (ret) {
		encoder_err(encoder, "cannot start the timer (%d)", ret);
		free(chip);
		return ret;
	}
	/* Counting from zero: wherever the counter is now. */
	chip->last = (uint32_t)__HAL_TIM_GET_COUNTER(chip->tim);

	encoder->ops = &amt102_ops;
	encoder->priv = chip;
	encoder->counts_per_turn = counts_per_turn;
	encoder->absolute = false;

	encoder_info(encoder, "%s-bit timer, %ld counts per turn",
		     chip->wide ? "32" : "16", (long)counts_per_turn);
	return 0;
}

void amt102_remove(struct encoder_dev *encoder)
{
	struct amt102 *chip = to_amt102(encoder);

	if (!chip)
		return;

	HAL_TIM_Encoder_Stop(chip->tim, TIM_CHANNEL_ALL);
	free(chip);
	encoder->ops = NULL;
	encoder->priv = NULL;
	encoder->counts_per_turn = 0;
	encoder->absolute = false;
}

#endif /* STM32 HAL */

#endif /* CONFIG_ENCODER_AMT102 */
