/*
 * Host-side check of the AS5600 driver behind a fake HAL. No hardware, no
 * ARM toolchain -- see test/Makefile. The chip and the cases are
 * ../as5600_cases.h; this file is the I2C handle under them, and the checks
 * only the HAL has: the 7-bit address shifted for it, and its errors
 * mapped.
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stm32_hal_stub.h"

#include "as5600.h"
#include "as5600_regs.h"
#include "encoder.h"

static I2C_HandleTypeDef i2c;
#define BUS (&i2c)

static bool absent;
static int probed;
static HAL_StatusTypeDef transfer_status;
static int allocs, frees;

/* The chip, in ../as5600_cases.h. */
static void as5600_fake_read(uint8_t reg, uint8_t *buf, size_t len);
static void as5600_fake_write(uint8_t reg, const uint8_t *buf, size_t len);

HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *h,
					uint16_t address, uint32_t trials,
					uint32_t timeout)
{
	assert(h == &i2c && trials >= 1 && timeout > 0);
	assert(address == 0x36 << 1 && "the HAL takes the address << 1");
	probed++;
	return absent ? HAL_ERROR : HAL_OK;
}

HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *h, uint16_t address,
				   uint16_t reg, uint16_t reg_size,
				   uint8_t *data, uint16_t len,
				   uint32_t timeout)
{
	assert(h == &i2c && address == 0x36 << 1);
	assert(reg_size == I2C_MEMADD_SIZE_8BIT && reg < 256 && timeout > 0);
	if (transfer_status != HAL_OK)
		return transfer_status;
	as5600_fake_read((uint8_t)reg, data, len);
	return HAL_OK;
}

HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *h, uint16_t address,
				    uint16_t reg, uint16_t reg_size,
				    uint8_t *data, uint16_t len,
				    uint32_t timeout)
{
	assert(h == &i2c && address == 0x36 << 1);
	assert(reg_size == I2C_MEMADD_SIZE_8BIT && reg < 256 && timeout > 0);
	if (transfer_status != HAL_OK)
		return transfer_status;
	as5600_fake_write((uint8_t)reg, data, len);
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

static void bus_fail(bool on)
{
	transfer_status = on ? HAL_TIMEOUT : HAL_OK;
}

#include "../as5600_cases.h"

/* The HAL's other two answers. */
static void test_hal_errors(void)
{
	struct encoder_dev encoder;
	struct encoder_sample s;

	chip_power_on();
	encoder = probe_ok(NULL);
	transfer_status = HAL_BUSY;
	assert(encoder_read(&encoder, &s) == -EAGAIN);
	transfer_status = HAL_ERROR;
	assert(encoder_read(&encoder, &s) == -EIO);
	transfer_status = HAL_OK;
	as5600_remove(&encoder);
}

int main(void)
{
	run_as5600_cases();
	test_hal_errors();
	printf("as5600 driver checks passed\n");
	return 0;
}
