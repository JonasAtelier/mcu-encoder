/*
 * Host-side check of the AS5047P driver on a fake spi_master. No hardware,
 * no ESP-IDF -- see test/Makefile. The chip and the cases are
 * ../as5047p_cases.h; this file is the SPI device under them, and the
 * checks only ESP-IDF has: the device added in mode 1 at the right clock,
 * and removed again.
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "as5047p.h"
#include "as5047p_math.h"
#include "as5047p_regs.h"
#include "encoder.h"

static spi_device_interface_config_t device;
static spi_host_device_t device_host;
static int added;
static esp_err_t add_err, transfer_err;
static int allocs, frees;

/* The chip, in ../as5047p_cases.h: one frame in, the answer out. */
static void as5047p_fake_frame(const uint8_t tx[2], uint8_t rx[2]);

esp_err_t spi_bus_add_device(spi_host_device_t host,
			     const spi_device_interface_config_t *config,
			     spi_device_handle_t *out_handle)
{
	if (add_err != ESP_OK)
		return add_err;
	device = *config;
	device_host = host;
	added++;
	*out_handle = (spi_device_handle_t)(void *)&device;
	return ESP_OK;
}

esp_err_t spi_bus_remove_device(spi_device_handle_t handle)
{
	assert(handle == (spi_device_handle_t)(void *)&device);
	added--;
	return ESP_OK;
}

/* Each transaction is one frame: CS is the driver's, framing each one. */
esp_err_t spi_device_polling_transmit(spi_device_handle_t handle,
				      spi_transaction_t *t)
{
	assert(handle == (spi_device_handle_t)(void *)&device);
	assert(t->length == 16 && "one 16-bit frame per chip select");
	assert(t->rxlength == 0 && "full duplex, rx as long as tx");
	assert(t->flags == (SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA) &&
	       "a two-byte frame travels in the transaction itself");
	if (transfer_err != ESP_OK)
		return transfer_err;
	as5047p_fake_frame(t->tx_data, t->rx_data);
	return ESP_OK;
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

static struct as5047p_config base_config(void)
{
	return (struct as5047p_config){ .host = SPI2_HOST, .cs_gpio = 10 };
}

static void bus_fail(bool on)
{
	transfer_err = on ? ESP_ERR_TIMEOUT : ESP_OK;
}

#include "../as5047p_cases.h"

/* What only ESP-IDF has: the device the driver adds, and its removal. */
static void test_device(void)
{
	struct as5047p_config cfg = base_config();
	struct encoder_dev encoder;

	fake_power_on();
	encoder = probe_ok(&cfg);
	assert(added == 1 && device_host == SPI2_HOST);
	/* "uses mode=1 (CPOL=0, CPHA=1)", SPI Interface (slave), p.11 */
	assert(device.mode == 1);
	assert(device.clock_speed_hz == 4000000 && "0 selects 4 MHz");
	assert(device.spics_io_num == 10);
	as5047p_remove(&encoder);
	assert(added == 0);

	cfg.spi_hz = 10000000;
	encoder = probe_ok(&cfg);
	assert(device.clock_speed_hz == 10000000);
	as5047p_remove(&encoder);

	/* The device refused: mapped, nothing leaked or left on the bus. */
	add_err = ESP_ERR_INVALID_ARG;
	allocs = frees = 0;
	assert(as5047p_probe(&encoder, &cfg) == -EINVAL);
	assert(allocs == 1 && frees == 1 && added == 0);
	add_err = ESP_OK;

	/* A failed probe after the add takes the device off again. */
	fake_power_on();
	fake_stuck = 0x0000;
	assert(as5047p_probe(&encoder, &cfg) == -ENXIO);
	assert(added == 0);
}

int main(void)
{
	run_as5047p_cases();
	test_device();
	printf("as5047p driver checks passed\n");
	return 0;
}
