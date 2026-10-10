/*
 * AS5047P: check the magnet, zero the shaft, then stream angle and total
 * through the encoder core.
 *
 * A snippet, not a project: the clock tree, the printf() retarget and the
 * SPI bus are the app's -- here hspi1 and the chip select pin as CubeMX
 * generates them (MX_SPI1_Init(), MX_GPIO_Init()). The handle must be in
 * mode 1 (CPOL low, CPHA second edge) with 8-bit frames, at 10 MHz or less;
 * the chip select an output, idle high. Needs HAL_SPI_MODULE_ENABLED and
 * ENCODER_AS5047P = y; printing the floats needs -u _printf_float.
 */
#include CMSIS_device_header

#include "as5047p.h"
#include "encoder.h"
#include "encoder_log.h"

#define CS_PORT		GPIOA		/* the app's choice, set up as an output */
#define CS_PIN		GPIO_PIN_4

extern SPI_HandleTypeDef hspi1;		/* from the app's MX_SPI1_Init() */

static struct encoder_dev encoder;	/* zeroed */

void encoder_task(void)
{
	const struct as5047p_config cfg = {
		.spi = &hspi1,
		.cs_port = CS_PORT,
		.cs_pin = CS_PIN,
	};
	struct as5047p_status status;

	/* probe already logged why */
	if (as5047p_probe(&encoder, &cfg))
		return;

	/* Check the magnet before trusting a single reading. */
	if (!as5047p_read_status(&encoder, &status)) {
		if (status.magnet_too_weak)
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
