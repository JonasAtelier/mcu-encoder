#include <errno.h>

#include "encoder.h"

#define ENCODER_2PI	6.283185307179586476925

/* C's % keeps the sign of the dividend; we want [0, 2*pi) either way. */
static float encoder_angle_rad(int64_t counts, int32_t counts_per_turn)
{
	int64_t turn = counts % counts_per_turn;

	if (turn < 0)
		turn += counts_per_turn;

	return (float)((double)turn * (ENCODER_2PI / counts_per_turn));
}

int encoder_read(struct encoder_dev *encoder, struct encoder_sample *sample)
{
	int64_t counts;
	int64_t time_us;
	int ret;

	if (!encoder || !encoder->ops || !encoder->ops->read || !sample ||
	    encoder->counts_per_turn <= 0)
		return -EINVAL;

	/* Stamp before the bus transfer: that is closer to the sample. */
	time_us = encoder_time_us();
	ret = encoder->ops->read(encoder, &counts);
	if (ret)
		return ret;

	/*
	 * The angle comes from the integer count, not from total_rad, so it
	 * keeps full resolution within the turn however far the total has run.
	 */
	sample->angle_rad = encoder_angle_rad(counts, encoder->counts_per_turn);
	sample->total_rad = (double)counts *
			    (ENCODER_2PI / encoder->counts_per_turn);
	sample->t_us = time_us;
	return 0;
}

int encoder_set_zero(struct encoder_dev *encoder)
{
	if (!encoder || !encoder->ops || !encoder->ops->set_zero)
		return -EINVAL;

	return encoder->ops->set_zero(encoder);
}
