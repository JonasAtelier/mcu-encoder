/*
 * AS5047P: check the magnet, zero the shaft, then stream angle and total
 * through the encoder core.
 *
 * Wiring: any SPI bus, mode 1. The pins below are the ones the README uses.
 */
#include "driver/spi_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "as5047p.h"
#include "encoder.h"
#include "encoder_log.h"

#define SPI_HOST_ID	SPI2_HOST
#define PIN_SCLK	18
#define PIN_MISO	19
#define PIN_MOSI	23
#define PIN_CS		5

void app_main(void)
{
	spi_bus_config_t buscfg = {
		.sclk_io_num = PIN_SCLK,
		.miso_io_num = PIN_MISO,
		.mosi_io_num = PIN_MOSI,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
		.max_transfer_sz = 4,
	};
	struct as5047p_config cfg = {
		.host = SPI_HOST_ID,
		.cs_gpio = PIN_CS,
	};
	struct encoder_dev encoder = { 0 };
	struct as5047p_status status;

	ESP_ERROR_CHECK(spi_bus_initialize(SPI_HOST_ID, &buscfg,
					   SPI_DMA_CH_AUTO));

	/* probe already logged why, under the "encoder" tag */
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

		vTaskDelay(pdMS_TO_TICKS(100));
	}
}
