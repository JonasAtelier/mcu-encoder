/*
 * AS5600: check the magnet, zero the shaft, then stream angle and total
 * through the encoder core.
 *
 * Wiring: any I2C bus. The address is fixed at 0x36, so one AS5600 per bus.
 */
#include <stdbool.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "as5600.h"
#include "encoder.h"
#include "encoder_log.h"

#define PIN_SDA 21
#define PIN_SCL 22

void app_main(void)
{
	i2c_master_bus_config_t buscfg = {
		.i2c_port = -1,
		.sda_io_num = PIN_SDA,
		.scl_io_num = PIN_SCL,
		.clk_source = I2C_CLK_SRC_DEFAULT,
		.glitch_ignore_cnt = 7,
		.flags.enable_internal_pullup = true,
	};
	struct as5600_config cfg = {
		.hysteresis = AS5600_HYST_2LSB,	/* stop the last bit flickering */
	};
	struct encoder_dev encoder = { 0 };
	struct as5600_status status;
	i2c_master_bus_handle_t bus;

	ESP_ERROR_CHECK(i2c_new_master_bus(&buscfg, &bus));

	/* probe already logged why, under the "encoder" tag */
	if (as5600_probe(&encoder, bus, &cfg))
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

		vTaskDelay(pdMS_TO_TICKS(100));
	}
}
