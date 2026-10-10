/*
 * The core's cases -- encoder_read(), encoder_set_zero(), the software turn
 * count and encoder_log_sample() -- shared by esp/test_core.c and
 * stm/test_core.c. Each includes this after providing:
 *
 *   set_clock_us(us)       what encoder_time_us() returns next
 *   advance_clock()        move that clock on, as a transfer would
 *   LOG_PREFIX             what the platform's log puts before "encoder: "
 *   stub_log_last          the last log line
 *
 * The per-chip rules -- -EBUSY on a second probe, -EINVAL for another
 * driver's device -- live in each driver, so each test_<chip>.c checks them
 * against that driver's real probe.
 */

#include "rad.h"

static int64_t fake_counts;
static int fake_ret;
static int fake_reads, fake_zeros;

static int fake_read(struct encoder_dev *encoder, int64_t *counts)
{
	(void)encoder;
	fake_reads++;
	/* The transfer takes time: a stamp taken after it would show this. */
	advance_clock();
	if (fake_ret)
		return fake_ret;
	*counts = fake_counts;
	return 0;
}

static int fake_set_zero(struct encoder_dev *encoder)
{
	(void)encoder;
	fake_zeros++;
	return fake_ret;
}

static const struct encoder_ops fake_ops = {
	.read = fake_read,
	.set_zero = fake_set_zero,
};

static struct encoder_dev fake_dev(int32_t counts_per_turn)
{
	struct encoder_dev encoder = {
		.name = "fake",
		.ops = &fake_ops,
		.counts_per_turn = counts_per_turn,
		.absolute = true,
	};

	fake_counts = 0;
	fake_ret = 0;
	fake_reads = fake_zeros = 0;
	return encoder;
}

static void test_read_stamps_and_converts(void)
{
	struct encoder_dev encoder = fake_dev(4096);
	struct encoder_sample s = { .t_us = -1 };

	set_clock_us(123456000);
	fake_counts = 1024;
	assert(encoder_read(&encoder, &s) == 0);
	assert(fake_reads == 1);
	/* Stamped before the read, not after: the fake moved the clock. */
	assert(s.t_us == 123456000);
	assert(s.angle_rad == angle_of(1024, 4096) &&
	       s.total_rad == total_of(1024, 4096));
	assert(NEAR(s.angle_rad, M_PI / 2) && NEAR(s.total_rad, M_PI / 2));

	/* Across turns: the angle stays in [0, 2*pi), the total does not. */
	fake_counts = 3 * 4096 + 2048;
	assert(encoder_read(&encoder, &s) == 0);
	assert(NEAR(s.angle_rad, M_PI) && NEAR(s.total_rad, 7 * M_PI));

	/* Below zero: the angle still in [0, 2*pi), the total negative. */
	fake_counts = -1024;
	assert(encoder_read(&encoder, &s) == 0);
	assert(NEAR(s.angle_rad, 1.5 * M_PI) && NEAR(s.total_rad, -M_PI / 2));
	fake_counts = -4096;
	assert(encoder_read(&encoder, &s) == 0);
	assert(s.angle_rad == 0.0f && NEAR(s.total_rad, -2 * M_PI));

	/*
	 * Far out, a million turns on, the angle still has full resolution:
	 * it comes from the integer count, not from the total.
	 */
	fake_counts = (int64_t)1000000 * 16384 + 1;
	encoder.counts_per_turn = 16384;
	assert(encoder_read(&encoder, &s) == 0);
	assert(s.angle_rad == angle_of(1, 16384));
	assert(s.total_rad == total_of(fake_counts, 16384));
}

static void test_errors(void)
{
	struct encoder_dev encoder = fake_dev(4096);
	struct encoder_sample s = { .angle_rad = 1.0f, .total_rad = 2.0,
				    .t_us = 7 };

	/* Whatever the chip says comes back unchanged, and s is left alone. */
	fake_ret = -ETIMEDOUT;
	assert(encoder_read(&encoder, &s) == -ETIMEDOUT);
	fake_ret = -EIO;
	assert(encoder_read(&encoder, &s) == -EIO);
	fake_ret = -EBADMSG;
	assert(encoder_read(&encoder, &s) == -EBADMSG);
	assert(s.angle_rad == 1.0f && s.total_rad == 2.0 && s.t_us == 7);
	assert(encoder_set_zero(&encoder) == -EBADMSG);
	fake_ret = 0;
	assert(encoder_set_zero(&encoder) == 0 && fake_zeros == 2);

	/* Nothing to call, or no turn to divide by, is the caller's mistake. */
	fake_reads = 0;
	assert(encoder_read(NULL, &s) == -EINVAL);
	assert(encoder_read(&encoder, NULL) == -EINVAL);
	encoder.counts_per_turn = 0;
	assert(encoder_read(&encoder, &s) == -EINVAL);
	encoder.counts_per_turn = 4096;
	encoder.ops = NULL;
	assert(encoder_read(&encoder, &s) == -EINVAL);
	assert(encoder_set_zero(&encoder) == -EINVAL);
	assert(encoder_set_zero(NULL) == -EINVAL);
	assert(fake_reads == 0 && s.t_us == 7);
}

/* The software turn count the absolute chips keep, in encoder_priv.h. */
static void test_track_turns(void)
{
	struct encoder_turns turns = { 0 };

	/* The first reading only sets the reference. */
	encoder_track_turns(&turns, 4000, 4096);
	assert(turns.revolutions == 0 && turns.have_last);
	/* Forward through zero: one more turn. */
	encoder_track_turns(&turns, 50, 4096);
	assert(turns.revolutions == 1);
	/* Back through zero: one less. */
	encoder_track_turns(&turns, 4090, 4096);
	assert(turns.revolutions == 0);
	/* Moves under half a turn are moves, not wraps. */
	encoder_track_turns(&turns, 2043, 4096);
	assert(turns.revolutions == 0);
	encoder_track_turns(&turns, 4090, 4096);
	assert(turns.revolutions == 0);
	/* Exactly half a turn is not more than half: no wrap counted. */
	encoder_track_turns(&turns, 4090 - 2048, 4096);
	assert(turns.revolutions == 0);
}

static void test_log(void)
{
	struct encoder_dev encoder = fake_dev(4096);
	struct encoder_sample s;

	encoder_err(&encoder, "no answer at 0x%02x (%d)", 0x36, -6);
	assert(!strcmp(stub_log_last, LOG_PREFIX("E")
		       "encoder: fake: no answer at 0x36 (-6)"));
	encoder_info(&encoder, "i");
	assert(!strcmp(stub_log_last, LOG_PREFIX("I") "encoder: fake: i"));

	set_clock_us(42000);
	fake_counts = 4096 + 1024;
	assert(encoder_read(&encoder, &s) == 0);
	encoder_log_sample(&encoder, &s);
	assert(!strcmp(stub_log_last, LOG_PREFIX("I")
		       "encoder: fake: t 42000 us  angle  1.5708 rad  "
		       "total       7.8540 rad"));

	/* The timestamp is formatted by hand: no %lld in newlib-nano. */
	s.t_us = INT64_MIN;
	encoder_log_sample(&encoder, &s);
	assert(strstr(stub_log_last, " t -9223372036854775808 us "));
	s.t_us = 0;
	encoder_log_sample(&encoder, &s);
	assert(strstr(stub_log_last, " t 0 us "));
}

static void run_core_cases(void)
{
	test_read_stamps_and_converts();
	test_errors();
	test_track_turns();
	test_log();
}
