/*
 * Host-side check of the AS5047P driver behind a fake HAL. No hardware, no
 * ARM toolchain -- see test/Makefile. The chip and the cases are
 * ../as5047p_cases.h; this file is the SPI handle and chip select under
 * them, and the checks only the HAL has: the handle's mode and frame
 * checked before anything else, and a chip select around every frame.
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stm32_hal_stub.h"

#include "as5047p.h"
#include "as5047p_math.h"
#include "as5047p_regs.h"
#include "encoder.h"

static SPI_HandleTypeDef spi;
static GPIO_TypeDef cs_port;
static bool cs_low;
static int cs_falls;
static HAL_StatusTypeDef transfer_status;
static int allocs, frees;

/* The chip, in ../as5047p_cases.h: one frame in, the answer out. */
static void as5047p_fake_frame(const uint8_t tx[2], uint8_t rx[2]);

void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state)
{
	assert(port == &cs_port && pin == GPIO_PIN_4);
	if (state == GPIO_PIN_RESET && !cs_low)
		cs_falls++;
	cs_low = state == GPIO_PIN_RESET;
}

/* Each call is one frame, inside its own chip select. */
HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef *h,
					  const uint8_t *tx, uint8_t *rx,
					  uint16_t size, uint32_t timeout)
{
	assert(h == &spi && timeout > 0);
	assert(size == 2 && "one 16-bit frame, as two 8-bit ones");
	assert(cs_low && "the chip only listens with CSn low");
	if (transfer_status != HAL_OK)
		return transfer_status;
	as5047p_fake_frame(tx, rx);
	return HAL_OK;
}

/* Counts the driver's calloc() and free() calls; see test/Makefile. */
void *__real_calloc(size_t n, size_t size);
void *__wrap_calloc(size_t n, size_t size);
void *__wrap_calloc(size_t n, size_t size)
{
	allocs++;
	return __real_calloc(n, size);
}
void __real_free(void *p);
void __wrap_free(void *p);
void __wrap_free(void *p)
{
	frees++;
	__real_free(p);
}

/* Mode 1 (CPOL low, CPHA second edge), 8-bit frames: what the probe wants. */
static struct as5047p_config base_config(void)
{
	spi.Init.CLKPolarity = SPI_POLARITY_LOW;
	spi.Init.CLKPhase = SPI_PHASE_2EDGE;
	spi.Init.DataSize = SPI_DATASIZE_8BIT;
	return (struct as5047p_config){ .spi = &spi, .cs_port = &cs_port,
					.cs_pin = GPIO_PIN_4 };
}

static void bus_fail(bool on)
{
	transfer_status = on ? HAL_TIMEOUT : HAL_OK;
}

#include "../as5047p_cases.h"

/* What only the HAL has: the handle checked first, CS around each frame. */
static void test_handle(void)
{
	struct as5047p_config cfg = base_config();
	struct encoder_dev encoder = { 0 };

	/* A chip select per frame, high again after each. */
	fake_power_on();
	cs_falls = 0;
	encoder = probe_ok(&cfg);
	assert(cs_falls == fake_frames && !cs_low);

	/* CS goes high even when the transfer failed. */
	transfer_status = HAL_BUSY;
	{
		uint16_t counts;

		assert(as5047p_read_counts(&encoder, &counts) == -EAGAIN);
	}
	assert(!cs_low);
	transfer_status = HAL_ERROR;
	{
		struct encoder_sample s;

		assert(encoder_read(&encoder, &s) == -EIO);
	}
	transfer_status = HAL_OK;
	as5047p_remove(&encoder);

	/* Not mode 1, or not 8-bit, or no handle: refused before anything. */
	fake_power_on();
	allocs = 0;
	cfg = base_config();
	spi.Init.CLKPolarity = SPI_POLARITY_HIGH;
	assert(as5047p_probe(&encoder, &cfg) == -EINVAL);
	cfg = base_config();
	spi.Init.CLKPhase = SPI_PHASE_1EDGE;
	assert(as5047p_probe(&encoder, &cfg) == -EINVAL);
	cfg = base_config();
	spi.Init.DataSize = SPI_DATASIZE_16BIT;
	assert(as5047p_probe(&encoder, &cfg) == -EINVAL);
	cfg = base_config();
	cfg.spi = NULL;
	assert(as5047p_probe(&encoder, &cfg) == -EINVAL);
	cfg = base_config();
	cfg.cs_port = NULL;
	assert(as5047p_probe(&encoder, &cfg) == -EINVAL);
	assert(fake_frames == 0 && allocs == 0 && !encoder.ops);
}

int main(void)
{
	run_as5047p_cases();
	test_handle();
	printf("as5047p driver checks passed\n");
	return 0;
}
