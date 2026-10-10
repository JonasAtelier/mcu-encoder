/*
 * amt102 -- CUI AMT102-V incremental encoder, as an encoder chip driver.
 *
 * A capacitive quadrature encoder: two square waves 90 degrees apart (A, B)
 * and one pulse per turn (X, the index). It is *incremental* -- it reports
 * how far the shaft has moved, never where it is. Every boot starts at zero
 * wherever the shaft happens to be sitting, so probe sets
 * encoder->absolute = false.
 *
 * Resolution is set by the DIP switches on the encoder body, not in
 * software. Whatever you dialled in there must be repeated in
 * struct amt102_config.ppr or every angle this driver reports is wrong by
 * the ratio.
 *
 * Counting is done in hardware in 4x mode, so both edges of both channels
 * count: 2048 PPR gives 8192 counts per turn. No CPU time is spent on it and
 * no pulses are lost while your code is busy. On ESP-IDF that is the
 * ESP32's PCNT peripheral, which the driver sets up on the GPIOs you name.
 * On the STM32 HAL it is a timer the app has already set up in encoder mode
 * on both inputs (TIM_ENCODERMODE_TI12) -- pins, input filter and all, as
 * CubeMX generates it; the driver only starts, reads and stops it.
 *
 * Angles come out of encoder_read(). Everything below that is AMT102
 * specific takes the struct encoder_dev that amt102_probe() filled, and
 * returns -EINVAL if handed a device some other driver probed.
 */
#ifndef AMT102_H
#define AMT102_H

#include <stdbool.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "driver/gpio.h"
#else
#include CMSIS_device_header
#endif

#include "encoder.h"

#ifdef __cplusplus
extern "C" {
#endif

struct amt102_config {
#ifdef ESP_PLATFORM
	gpio_num_t a_gpio;	/* encoder A */
	gpio_num_t b_gpio;	/* encoder B */
	/*
	 * Index (X), or GPIO_NUM_NC if you left it unwired. Set it either way:
	 * a zeroed config means GPIO 0, not "none".
	 */
	gpio_num_t z_gpio;
#else
	/*
	 * A timer the app has HAL_TIM_Encoder_Init()ed in encoder mode
	 * TIM_ENCODERMODE_TI12, with Init.Period at its counter's full range:
	 * 0xFFFF on a 16-bit timer, 0xFFFFFFFF on a 32-bit one. The driver
	 * widens it to 64 bits in software, so read at least once per half
	 * counter range of travel -- 4 turns at 2048 PPR on a 16-bit timer.
	 */
	TIM_HandleTypeDef *tim;
#endif

	/*
	 * Pulses per revolution, exactly as set on the encoder's DIP switches:
	 * 48, 96, 100, 125, 192, 200, 250, 256, 360, 400, 500, 512, 800, 1000,
	 * 1024 or 2048. 0 selects 2048, the factory setting.
	 */
	uint16_t ppr;

	bool invert;		/* count the other way round, in software */

#ifdef ESP_PLATFORM
	/*
	 * Ignore pulses shorter than this, in nanoseconds. 0 selects 1000 ns,
	 * which is plenty for a real encoder and kills contact noise on long
	 * cables. Raise it if a stationary shaft still creeps; lower it only
	 * if you spin fast enough that real pulses get shorter than this.
	 */
	uint32_t glitch_ns;
#else
	/*
	 * true if the index (X) reaches an EXTI line the app has set up and
	 * whose callback calls amt102_index_irq(); false if X is unwired. The
	 * timer's own input filter (ICxFilter) does what glitch_ns does on
	 * ESP-IDF.
	 */
	bool index;
#endif
};

/*
 * Start counting from zero: on ESP-IDF claim a PCNT unit, on the STM32 HAL
 * start the app's timer. Fills encoder->name, ops, priv, counts_per_turn and
 * absolute.
 *
 * -EBUSY if encoder is already bound (see struct encoder_dev); nothing is
 * touched. -EINVAL if A, B and X are not all different, or on the HAL for a
 * NULL timer or an Init.Period other than 0xFFFF or 0xFFFFFFFF. -ENOSPC when
 * every PCNT unit or channel is already in use. Other driver failures come
 * back as the errno of the failed call.
 */
int amt102_probe(struct encoder_dev *encoder,
		 const struct amt102_config *cfg);

/* Stops counting and releases the PCNT unit, or stops the timer. Safe on an
 * unprobed encoder_dev. */
void amt102_remove(struct encoder_dev *encoder);

/*
 * Has the index pulse fired since you last asked? Clears on read. Poll it
 * while jogging the shaft, then call encoder_set_zero() to home. Needs
 * z_gpio (on the HAL, cfg.index); without one it returns -EOPNOTSUPP.
 */
int amt102_index_hit(struct encoder_dev *encoder, bool *hit);

#ifndef ESP_PLATFORM
/*
 * The index pulse fired: call this from the app's HAL_GPIO_EXTI_Callback()
 * for the X line. The library owns no interrupt; this only sets the flag
 * amt102_index_hit() reads. Safe in an interrupt, and on a device another
 * driver probed (it does nothing).
 */
void amt102_index_irq(struct encoder_dev *encoder);
#endif

#ifdef __cplusplus
}
#endif

#endif /* AMT102_H */
