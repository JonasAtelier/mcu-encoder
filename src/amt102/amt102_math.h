/*
 * Pure counter arithmetic for the AMT102 on an STM32 timer, split out so it
 * can be compiled and asserted on the host (see test/test_amt102_math.c).
 */
#ifndef AMT102_MATH_H
#define AMT102_MATH_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Counts moved between two readings of a free-running up/down counter that
 * wraps at its full range: 16 bits, or 32 with wide. The difference is
 * taken modulo the counter's range and read as signed, so a move of less
 * than half the range either way comes out right across the wrap -- 0xFFFF
 * to 0x0001 is +2, 0x0001 to 0xFFFF is -2. A move of half the range or more
 * between two readings is indistinguishable from one the other way.
 */
static inline int32_t amt102_counter_delta(uint32_t now, uint32_t last,
					   bool wide)
{
	if (wide)
		return (int32_t)(now - last);
	return (int16_t)(uint16_t)(now - last);
}

#endif /* AMT102_MATH_H */
