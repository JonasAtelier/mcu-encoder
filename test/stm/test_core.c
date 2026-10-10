/*
 * Host-side check of the core on the STM32 HAL: the cases are
 * ../core_cases.h; this file supplies the clock -- the weak default,
 * HAL_GetTick() milliseconds as microseconds -- and the log prefix.
 */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "stm32_hal_stub.h"

#include "encoder.h"
#include "encoder_log.h"
#include "encoder_priv.h"

/* The HAL's lines are bare printf(), no level letter. */
#define LOG_PREFIX(level)	""

static void set_clock_us(int64_t us)
{
	stub_tick = (uint32_t)(us / 1000);
}

static void advance_clock(void)
{
	stub_tick += 1;
}

#include "../core_cases.h"

/* 32-bit milliseconds do not wrap in the 64-bit microseconds. */
static void test_clock_range(void)
{
	stub_tick = UINT32_MAX;
	assert(encoder_time_us() == (int64_t)UINT32_MAX * 1000);
}

int main(void)
{
	run_core_cases();
	test_clock_range();
	printf("all core checks passed\n");
	return 0;
}
