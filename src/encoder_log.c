#include <stdint.h>

#include "encoder_log.h"

/*
 * Decimal text of v, written backwards from end. newlib-nano's printf, the
 * one stm-forge links, has no %lld, so the timestamp cannot go through PRId64.
 */
static const char *encoder_format_i64(char *end, int64_t v)
{
	uint64_t u = v < 0 ? 0 - (uint64_t)v : (uint64_t)v;

	*--end = '\0';
	do {
		*--end = (char)('0' + u % 10);
		u /= 10;
	} while (u);
	if (v < 0)
		*--end = '-';
	return end;
}

void encoder_log_sample(const struct encoder_dev *encoder,
			const struct encoder_sample *sample)
{
	char t[21];	/* "-9223372036854775808" and its NUL */

	encoder_info(encoder, "t %s us  angle %7.4f rad  total %12.4f rad",
		     encoder_format_i64(t + sizeof(t), sample->t_us),
		     (double)sample->angle_rad, sample->total_rad);
}
