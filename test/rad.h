/*
 * What encoder_read() reports for c counts at n counts per turn, by the
 * formulas of src/encoder.c -- so a test can ask for bit-exact agreement --
 * and NEAR(), for checking those formulas against plain numbers.
 */
#ifndef RAD_H
#define RAD_H

#include <math.h>
#include <stdint.h>

#define TWO_PI	6.283185307179586476925

static inline float angle_of(int64_t c, int32_t n)
{
	int64_t turn = c % n;

	if (turn < 0)
		turn += n;
	return (float)((double)turn * (TWO_PI / n));
}

static inline double total_of(int64_t c, int32_t n)
{
	return (double)c * (TWO_PI / n);
}

#define NEAR(a, b)	(fabs((double)(a) - (double)(b)) < 1e-6)

#endif /* RAD_H */
