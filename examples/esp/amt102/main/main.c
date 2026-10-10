/*
 * AMT102-V: count quadrature on PCNT and stream angle and total through the
 * encoder core, noting each index pulse.
 *
 * Wiring: A, B and X through a 5 V to 3.3 V level shifter -- the encoder runs
 * at 5 V and the ESP32's GPIOs do not.
 */
#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "amt102.h"
#include "encoder.h"
#include "encoder_log.h"

#define PIN_A	18
#define PIN_B	19
#define PIN_X	21	/* or GPIO_NUM_NC if X is not wired */

void app_main(void)
{
	struct amt102_config cfg = {
		.a_gpio = PIN_A,
		.b_gpio = PIN_B,
		.z_gpio = PIN_X,
		.ppr = 2048,		/* must match the encoder's DIP switches */
	};
	struct encoder_dev encoder = { 0 };

	/* probe already logged why, under the "encoder" tag */
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

		vTaskDelay(pdMS_TO_TICKS(100));
	}
}
