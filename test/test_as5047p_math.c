/*
 * Host-side check of the AS5047P frame parity. No embedded toolchain
 * needed -- see test/Makefile.
 *
 * PARC and PARD are even parity over bits 14:0, in bit 15 (Figures 13, 14,
 * 16 of docs/AS5047P_v2-00.pdf): a frame with the parity bit placed has an
 * even number of ones in all 16 bits.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "as5047p_math.h"
#include "as5047p_regs.h"

static int ones(uint16_t v)
{
	int n = 0;

	for (; v; v &= (uint16_t)(v - 1))
		n++;
	return n;
}

/* Worked by hand from the bit counts. */
static void test_known_frames(void)
{
	/* Read ANGLECOM: 0x7FFF has fifteen ones, so PARC is set. */
	assert(as5047p_with_parity(AS5047P_ANGLECOM | AS5047P_CMD_READ) ==
	       0xFFFF);
	/* Read NOP: one one, PARC set. */
	assert(as5047p_with_parity(AS5047P_NOP | AS5047P_CMD_READ) == 0xC000);
	/* Read DIAAGC: 0x7FFC has thirteen ones. */
	assert(as5047p_with_parity(AS5047P_DIAAGC | AS5047P_CMD_READ) ==
	       0xFFFC);
	/* Read ERRFL: 0x4001 has two, PARC clear. */
	assert(as5047p_with_parity(AS5047P_ERRFL | AS5047P_CMD_READ) ==
	       0x4001);
	/* All zeros has none. */
	assert(as5047p_with_parity(0x0000) == 0x0000);
}

/* Every 15-bit value: data kept, parity even, the old bit 15 ignored. */
static void test_every_frame(void)
{
	for (uint32_t v = 0; v < 0x8000; v++) {
		uint16_t frame = as5047p_with_parity((uint16_t)v);

		assert((frame & 0x7FFF) == v);
		assert(ones(frame) % 2 == 0);
		assert(as5047p_with_parity((uint16_t)(v | 0x8000)) == frame);
		/* A frame that already carries its parity is its own fixed
		 * point -- which is how a reply is checked. */
		assert(as5047p_with_parity(frame) == frame);
	}
}

int main(void)
{
	test_known_frames();
	test_every_frame();
	printf("as5047p math: all checks passed\n");
	return 0;
}
