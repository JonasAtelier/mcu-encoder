/*
 * Host-side check of the AMT102's timer widening on the STM32 HAL. No
 * embedded toolchain needed -- see test/Makefile.
 *
 * A timer in encoder mode counts up and down and wraps at its full range,
 * 16 or 32 bits. amt102_counter_delta() turns two readings into the signed
 * move between them, which is right as long as the move is under half the
 * range.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "amt102_math.h"

static void test_16bit(void)
{
	/* No wrap. */
	assert(amt102_counter_delta(100, 40, false) == 60);
	assert(amt102_counter_delta(40, 100, false) == -60);
	assert(amt102_counter_delta(7, 7, false) == 0);
	/* Forward across 0xFFFF -> 0x0000, and back. */
	assert(amt102_counter_delta(0x0001, 0xFFFF, false) == 2);
	assert(amt102_counter_delta(0xFFFF, 0x0001, false) == -2);
	assert(amt102_counter_delta(0x0010, 0xFFF0, false) == 0x20);
	/* The largest moves that still come out right. */
	assert(amt102_counter_delta(0x7FFF, 0x0000, false) == 32767);
	assert(amt102_counter_delta(0x8000, 0x0000, false) == -32768);
	assert(amt102_counter_delta(0x0000, 0x7FFF, false) == -32767);
	/* Upper bits of the reading are not the 16-bit counter's. */
	assert(amt102_counter_delta(0x12340005, 0x00000003, false) == 2);
}

static void test_32bit(void)
{
	assert(amt102_counter_delta(100, 40, true) == 60);
	assert(amt102_counter_delta(0x00000001, 0xFFFFFFFF, true) == 2);
	assert(amt102_counter_delta(0xFFFFFFFF, 0x00000001, true) == -2);
	/* 16-bit wrap points mean nothing here. */
	assert(amt102_counter_delta(0x00010001, 0x0000FFFF, true) == 2);
	assert(amt102_counter_delta(0x00000000, 0x0000FFFF, true) == -65535);
	assert(amt102_counter_delta(0x7FFFFFFF, 0x00000000, true) ==
	       INT32_MAX);
	assert(amt102_counter_delta(0x80000000, 0x00000000, true) ==
	       INT32_MIN);
}

/* A walk of many small steps, wrapping both ways, sums to the true total. */
static void test_walk(void)
{
	for (int wide = 0; wide < 2; wide++) {
		uint32_t mask = wide ? 0xFFFFFFFFu : 0xFFFFu;
		uint32_t counter = wide ? 0xFFFFFF00u : 0xFF00u;
		uint32_t last = counter;
		int64_t truth = 0, total = 0;

		for (int i = 0; i < 100000; i++) {
			int32_t step = (i / 1000) % 2 ? -1237 : 1913;

			counter = (counter + (uint32_t)step) & mask;
			truth += step;
			total += amt102_counter_delta(counter, last, wide);
			last = counter;
		}
		assert(total == truth);
	}
}

int main(void)
{
	test_16bit();
	test_32bit();
	test_walk();
	printf("amt102 math: all checks passed\n");
	return 0;
}
