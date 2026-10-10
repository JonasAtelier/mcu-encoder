/*
 * Pure frame arithmetic for the AS5047P, split out so it can be compiled and
 * asserted on the host (see test/test_as5047p_math.c).
 */
#ifndef AS5047P_MATH_H
#define AS5047P_MATH_H

#include <stdint.h>

#include "as5047p_regs.h"

/*
 * Even parity over bits 14:0, placed in bit 15 -- the chip rejects a frame
 * whose parity is wrong (PARC, Figure 13), and flags its own reply the same
 * way (PARD, Figure 14).
 */
static inline uint16_t as5047p_with_parity(uint16_t frame)
{
	uint16_t value = frame & ~AS5047P_FRAME_PARITY;
	uint16_t bits = value;

	bits ^= bits >> 8;
	bits ^= bits >> 4;
	bits ^= bits >> 2;
	bits ^= bits >> 1;

	return (bits & 1) ? (uint16_t)(value | AS5047P_FRAME_PARITY) : value;
}

#endif /* AS5047P_MATH_H */
