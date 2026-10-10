/*
 * AS5600: check the magnet, zero the shaft, then stream angle and total
 * through the encoder core.
 *
 * A snippet, not a project: the clock tree, the printf() retarget and the
 * I2C bus are the app's -- here hi2c1 as CubeMX generates it
 * (MX_I2C1_Init()), at 1 MHz or less. The address is fixed at 0x36, so one
 * AS5600 per bus. Needs HAL_I2C_MODULE_ENABLED and ENCODER_AS5600 = y;
 * printing the floats needs -u _printf_float.
 */
#include CMSIS_device_header

#include "as5600.h"
#include "encoder.h"
#include "encoder_log.h"

extern I2C_HandleTypeDef hi2c1;		/* from the app's MX_I2C1_Init() */

static struct encoder_dev encoder;	/* zeroed */

void encoder_task(void)
{
	const struct as5600_config cfg = {
		.hysteresis = AS5600_HYST_2LSB,	/* stop the last bit flickering */
	};
	struct as5600_status status;

	/* probe already logged why */
	if (as5600_probe(&encoder, &hi2c1, &cfg))
		return;

	/* Check the magnet before trusting a single reading. */
	if (!as5600_read_status(&encoder, &status)) {
		if (!status.magnet_detected)
			encoder_warn(&encoder, "no magnet detected");
		else if (status.magnet_too_weak)
			encoder_warn(&encoder, "magnet too weak - move it closer");
		else if (status.magnet_too_strong)
			encoder_warn(&encoder,
				     "magnet too strong - move it further away");
		encoder_info(&encoder, "agc %u (aim for mid-range), magnitude %u",
			     status.agc, status.magnitude);
	}

	/* Wherever the shaft sits now becomes zero. */
	encoder_set_zero(&encoder);

	for (;;) {
		struct encoder_sample sample;

		if (!encoder_read(&encoder, &sample))
			encoder_log_sample(&encoder, &sample);
		HAL_Delay(100);
	}
}
