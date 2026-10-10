/*
 * AMT102-V: count quadrature on a timer in encoder mode and stream angle and
 * total through the encoder core, noting each index pulse.
 *
 * A snippet, not a project: the clock tree, the printf() retarget and the
 * timer are the app's -- here htim2 as CubeMX generates it
 * (MX_TIM2_Init()): encoder mode TIM_ENCODERMODE_TI12 on channels 1 and 2,
 * Init.Period at the counter's full range -- 0xFFFF on a 16-bit timer,
 * 0xFFFFFFFF on a 32-bit one; your part's reference manual says which a
 * timer is -- and the input filter it needs. The index, X, goes to an EXTI
 * line whose callback calls amt102_index_irq(), below. Needs
 * HAL_TIM_MODULE_ENABLED and ENCODER_AMT102 = y; printing the floats needs
 * -u _printf_float.
 *
 * Wiring: A, B and X through a 5 V to 3.3 V level shifter unless the pins
 * are 5 V tolerant -- the encoder runs at 5 V.
 */
#include <stdbool.h>

#include CMSIS_device_header

#include "amt102.h"
#include "encoder.h"
#include "encoder_log.h"

#define INDEX_PIN	GPIO_PIN_0	/* the app's EXTI line for X */

extern TIM_HandleTypeDef htim2;		/* from the app's MX_TIM2_Init() */

static struct encoder_dev encoder;	/* zeroed */

/* The HAL's weak callback, for every EXTI line the app enabled. */
void HAL_GPIO_EXTI_Callback(uint16_t pin)
{
	if (pin == INDEX_PIN)
		amt102_index_irq(&encoder);
}

void encoder_task(void)
{
	const struct amt102_config cfg = {
		.tim = &htim2,
		.ppr = 2048,		/* must match the encoder's DIP switches */
		.index = true,
	};

	/* probe already logged why */
	if (amt102_probe(&encoder, &cfg))
		return;

	/*
	 * Incremental: zero is wherever the shaft sat at power-up. To home on
	 * the index pulse instead, jog the shaft until amt102_index_hit()
	 * reports true, then call encoder_set_zero().
	 */
	for (;;) {
		struct encoder_sample sample;
		bool index = false;

		if (!encoder_read(&encoder, &sample))
			encoder_log_sample(&encoder, &sample);

		if (!amt102_index_hit(&encoder, &index) && index)
			encoder_info(&encoder, "index passed");

		HAL_Delay(100);
	}
}
