/*
 * Host-side check of the core: encoder_read() turning a chip's counts into
 * radians, encoder_set_zero(), the log macros, and the helpers every chip
 * shares in src/encoder_priv.h (turn tracking, invert, the errno map),
 * against a fake chip whose ops hand back whatever count the test put there.
 *
 * The per-chip rules -- -EBUSY on a second probe, -EINVAL for another
 * driver's device -- live in each driver, so each test_<chip>.c checks them
 * against that driver's real probe, along with the wraps as that chip sees
 * them.
 */
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "encoder.h"
#include "encoder_log.h"
#include "encoder_priv.h"

#define PI	3.14159265358979323846

static int64_t fake_counts;
static int fake_ret, fake_calls, fake_zeroes;

static int fake_read(struct encoder_dev *encoder, int64_t *counts)
{
	(void)encoder;
	fake_calls++;
	/* The transfer takes time: a stamp taken after it would show this. */
	stub_time_us += 1000;
	if (fake_ret)
		return fake_ret;
	*counts = fake_counts;
	return 0;
}

static int fake_set_zero(struct encoder_dev *encoder)
{
	(void)encoder;
	fake_zeroes++;
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
	};

	fake_counts = 0;
	fake_ret = fake_calls = fake_zeroes = 0;
	return encoder;
}

/* Read the fake at `counts` and return the sample. */
static struct encoder_sample read_at(struct encoder_dev *encoder,
				     int64_t counts)
{
	struct encoder_sample s;

	fake_counts = counts;
	assert(encoder_read(encoder, &s) == 0);
	return s;
}

static void test_read_stamps(void)
{
	struct encoder_dev encoder = fake_dev(4096);
	struct encoder_sample s = { .t_us = -1 };

	stub_time_us = 123456789;
	fake_counts = 1024;
	assert(encoder_read(&encoder, &s) == 0);
	assert(fake_calls == 1);
	/* Stamped before the read, not after: the fake moved the clock. */
	assert(s.t_us == 123456789);
	assert(stub_time_us == 123456789 + 1000);
	assert(fabs(s.angle_rad - PI / 2) < 1e-6);
	assert(fabs(s.total_rad - PI / 2) < 1e-12);
}

static void test_errno_passthrough(void)
{
	struct encoder_dev encoder = fake_dev(4096);
	struct encoder_sample s = { .angle_rad = 0.25f, .total_rad = 99.0,
				    .t_us = 7 };

	/* Whatever the chip says comes back unchanged, and s is left alone. */
	stub_time_us = 42;
	fake_ret = -ETIMEDOUT;
	assert(encoder_read(&encoder, &s) == -ETIMEDOUT);
	fake_ret = -ENXIO;
	assert(encoder_read(&encoder, &s) == -ENXIO);
	fake_ret = -EBADMSG;
	assert(encoder_read(&encoder, &s) == -EBADMSG);
	assert(s.angle_rad == 0.25f && s.total_rad == 99.0 && s.t_us == 7);

	/* Nothing to call is the caller's mistake, not a chip error. */
	fake_calls = 0;
	assert(encoder_read(NULL, &s) == -EINVAL);
	assert(encoder_read(&encoder, NULL) == -EINVAL);
	encoder.counts_per_turn = 0;
	assert(encoder_read(&encoder, &s) == -EINVAL);
	encoder.counts_per_turn = -4096;
	assert(encoder_read(&encoder, &s) == -EINVAL);
	encoder.counts_per_turn = 4096;
	encoder.ops = NULL;
	assert(encoder_read(&encoder, &s) == -EINVAL);
	assert(fake_calls == 0);
	assert(s.t_us == 7);
}

static void test_set_zero(void)
{
	static const struct encoder_ops no_zero = { .read = fake_read };
	struct encoder_dev encoder = fake_dev(4096);

	assert(encoder_set_zero(&encoder) == 0 && fake_zeroes == 1);
	fake_ret = -EIO;
	assert(encoder_set_zero(&encoder) == -EIO && fake_zeroes == 2);

	assert(encoder_set_zero(NULL) == -EINVAL);
	encoder.ops = &no_zero;
	assert(encoder_set_zero(&encoder) == -EINVAL);
	encoder.ops = NULL;
	assert(encoder_set_zero(&encoder) == -EINVAL);
	assert(fake_zeroes == 2);
}

/* Counts to radians: the angle folds into [0, 2*pi), the total does not. */
static void test_radians(void)
{
	const int32_t cprs[] = { 4096, 16384, 8192, 192 };
	struct encoder_sample s;
	size_t i;

	for (i = 0; i < sizeof(cprs) / sizeof(cprs[0]); i++) {
		int32_t n = cprs[i];
		struct encoder_dev encoder = fake_dev(n);

		s = read_at(&encoder, 0);
		assert(s.angle_rad == 0.0f && s.total_rad == 0.0);

		/* The last count is below 2*pi, never equal to it. */
		s = read_at(&encoder, n - 1);
		assert(s.angle_rad < (float)(2 * PI) && s.angle_rad > 6.2f);

		/* One count back from zero is at the top of the turn. */
		s = read_at(&encoder, -1);
		assert(fabs(s.angle_rad - 2 * PI * (n - 1) / n) < 1e-5);
		assert(fabs(s.total_rad + 2 * PI / n) < 1e-12);

		/* A turn and a quarter each way. */
		s = read_at(&encoder, n + n / 4);
		assert(fabs(s.angle_rad - PI / 2) < 1e-5);
		assert(fabs(s.total_rad - 2.5 * PI) < 1e-12);
		s = read_at(&encoder, -(n + n / 4));
		assert(fabs(s.angle_rad - 1.5 * PI) < 1e-5);
		assert(fabs(s.total_rad + 2.5 * PI) < 1e-12);
	}

	/*
	 * Why total_rad is a double: an hour at 3000 rpm on a 14-bit encoder
	 * is 180000 turns, and one count on top must still show.
	 */
	{
		struct encoder_dev encoder = fake_dev(16384);

		s = read_at(&encoder, (int64_t)180000 * 16384 + 1);
		assert(fabs(s.total_rad - (180000 * 2 * PI + 2 * PI / 16384)) <
		       1e-6);
		assert(fabs(s.angle_rad - 2 * PI / 16384) < 1e-6);
		s = read_at(&encoder, (int64_t)180000 * 16384 + 2);
		assert(s.total_rad - (180000 * 2 * PI + 2 * PI / 16384) >
		       0.9 * 2 * PI / 16384 && "one count still resolves");
	}
}

/* Counts in, revolutions out, for a 4096-count encoder. */
static void test_track_turns(void)
{
	struct encoder_turns t = { 0 };
	const int32_t n = 4096;

	/* The first reading is taken as it stands: turn 0. */
	encoder_track_turns(&t, 4000, n);
	assert(t.revolutions == 0);
	/* Forward across 0: 4000 -> 95 is +191, one turn up. */
	encoder_track_turns(&t, 95, n);
	assert(t.revolutions == 1);
	encoder_track_turns(&t, 2000, n);
	assert(t.revolutions == 1);
	/* Backward across 0 twice. */
	encoder_track_turns(&t, 10, n);
	encoder_track_turns(&t, 4090, n);
	assert(t.revolutions == 0);
	encoder_track_turns(&t, 100, n);
	assert(t.revolutions == 1);
	encoder_track_turns(&t, 4000, n);
	encoder_track_turns(&t, 2500, n);
	encoder_track_turns(&t, 1000, n);
	encoder_track_turns(&t, 5, n);
	assert(t.revolutions == 0);
	encoder_track_turns(&t, 4095, n);
	assert(t.revolutions == -1);

	/*
	 * Exactly half a turn is ambiguous and is not a wrap; one more count
	 * is. That is the sampling limit: move strictly under half a turn.
	 */
	t = (struct encoder_turns){ 0 };
	encoder_track_turns(&t, 0, n);
	encoder_track_turns(&t, 2048, n);
	assert(t.revolutions == 0);
	encoder_track_turns(&t, 0, n);
	assert(t.revolutions == 0);
	encoder_track_turns(&t, 2049, n);
	assert(t.revolutions == -1);
}

static void test_invert(void)
{
	assert(encoder_invert(0, 4096) == 0);
	assert(encoder_invert(1, 4096) == 4095);
	assert(encoder_invert(4095, 4096) == 1);
	assert(encoder_invert(2048, 4096) == 2048);
	assert(encoder_invert(16383, 16384) == 1);
}

static void test_errno_map(void)
{
	assert(encoder_errno(ESP_OK) == 0);
	assert(encoder_errno(ESP_ERR_INVALID_ARG) == -EINVAL);
	assert(encoder_errno(ESP_ERR_TIMEOUT) == -ETIMEDOUT);
	assert(encoder_errno(ESP_ERR_NOT_FOUND) == -ENXIO);
	assert(encoder_errno(ESP_ERR_INVALID_SIZE) == -EMSGSIZE);
	assert(encoder_errno(ESP_ERR_NO_MEM) == -ENOMEM);
	assert(encoder_errno(ESP_ERR_NOT_SUPPORTED) == -EOPNOTSUPP);
	/* A NACKed i2c_master transfer: -EIO, not "nothing there". */
	assert(encoder_errno(ESP_ERR_INVALID_STATE) == -EIO);
	assert(encoder_errno(ESP_FAIL) == -EIO);
}

static void test_log_prefix(void)
{
	struct encoder_dev encoder = fake_dev(4096);
	struct encoder_sample s;

	encoder_err(&encoder, "reply 0x%04x", 0xBEEF);
	assert(!strcmp(stub_log_last, "E encoder: fake: reply 0xbeef"));

	/* No arguments after the format must still compile and work. */
	encoder_warn(&encoder, "no arguments");
	assert(!strcmp(stub_log_last, "W encoder: fake: no arguments"));
	encoder_info(&encoder, "i");
	assert(!strcmp(stub_log_last, "I encoder: fake: i"));
	encoder_dbg(&encoder, "d");
	assert(!strcmp(stub_log_last, "D encoder: fake: d"));

	stub_time_us = 42;
	s = read_at(&encoder, 1024 - 4096 * 2);	/* quarter turn, two back */
	encoder_log_sample(&encoder, &s);
	assert(!strcmp(stub_log_last,
		       "I encoder: fake: t 42 us  angle  1.5708 rad  total     -10.9956 rad"));
}

int main(void)
{
	test_read_stamps();
	test_errno_passthrough();
	test_set_zero();
	test_radians();
	test_track_turns();
	test_invert();
	test_errno_map();
	test_log_prefix();
	printf("all core checks passed\n");
	return 0;
}
