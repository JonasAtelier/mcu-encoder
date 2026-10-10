/*
 * Host-side check of the AS5600 driver on a fake i2c_master. No hardware, no
 * ESP-IDF -- see test/Makefile. The chip and the cases are
 * ../as5600_cases.h; this file is the bus under them, and the checks only
 * ESP-IDF has: presence by i2c_master_probe() before the device is added,
 * the device added at the right address and clock, and removed again.
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2c_master.h"

#include "as5600.h"
#include "as5600_regs.h"
#include "encoder.h"

static int bus_token;
#define BUS ((i2c_master_bus_handle_t)(void *)&bus_token)

static bool absent;
static int probed, added;
static uint16_t dev_addr;
static uint32_t dev_scl_hz;
static esp_err_t transfer_err;
static int allocs, frees;

/* The chip, in ../as5600_cases.h. */
static void as5600_fake_read(uint8_t reg, uint8_t *buf, size_t len);
static void as5600_fake_write(uint8_t reg, const uint8_t *buf, size_t len);

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address,
			   int timeout_ms)
{
	assert(bus == BUS && timeout_ms > 0);
	assert(address == 0x36);
	assert(added == 0 && "presence before the device is added");
	probed++;
	return absent ? ESP_ERR_NOT_FOUND : ESP_OK;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
				    const i2c_device_config_t *config,
				    i2c_master_dev_handle_t *out)
{
	assert(bus == BUS);
	assert(config->dev_addr_length == I2C_ADDR_BIT_LEN_7);
	dev_addr = config->device_address;
	dev_scl_hz = config->scl_speed_hz;
	added++;
	*out = (i2c_master_dev_handle_t)(void *)&added;
	return ESP_OK;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t dev)
{
	assert(dev == (i2c_master_dev_handle_t)(void *)&added);
	added--;
	return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev,
			      const uint8_t *data, size_t len, int timeout_ms)
{
	assert(dev == (i2c_master_dev_handle_t)(void *)&added);
	assert(len >= 2 && timeout_ms > 0);
	if (transfer_err != ESP_OK)
		return transfer_err;
	as5600_fake_write(data[0], data + 1, len - 1);
	return ESP_OK;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev,
				      const uint8_t *tx, size_t tx_len,
				      uint8_t *rx, size_t rx_len,
				      int timeout_ms)
{
	assert(dev == (i2c_master_dev_handle_t)(void *)&added);
	assert(tx_len == 1 && rx_len >= 1 && timeout_ms > 0);
	if (transfer_err != ESP_OK)
		return transfer_err;
	as5600_fake_read(tx[0], rx, rx_len);
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

static void bus_fail(bool on)
{
	transfer_err = on ? ESP_ERR_TIMEOUT : ESP_OK;
}

#include "../as5600_cases.h"

static void test_device(void)
{
	struct as5600_config cfg = { 0 };
	struct encoder_dev encoder;

	chip_power_on();
	encoder = probe_ok(&cfg);
	assert(added == 1 && dev_addr == 0x36);
	assert(dev_scl_hz == 400000 && "0 means 400000");
	as5600_remove(&encoder);
	assert(added == 0);

	cfg.scl_speed_hz = 1000000;
	encoder = probe_ok(&cfg);
	assert(dev_scl_hz == 1000000);
	as5600_remove(&encoder);

	/* A failed probe after the add takes the device off again. */
	bus_fail(true);
	assert(as5600_probe(&encoder, BUS, &cfg) == -ETIMEDOUT);
	bus_fail(false);
	assert(added == 0);
}

int main(void)
{
	run_as5600_cases();
	test_device();
	printf("as5600 driver checks passed\n");
	return 0;
}
