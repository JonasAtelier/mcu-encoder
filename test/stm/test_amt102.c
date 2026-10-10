/*
 * Host-side check of the AMT102 driver on the STM32 HAL, against a fake
 * timer in encoder mode. No hardware, no ARM toolchain -- see test/Makefile.
 *
 * The app owns the timer: its handle, its Init and the counter. The fake
 * only records whether the driver started and stopped it, and the test
 * moves TIM->CNT the way the encoder's pulses would, wrapping at 16 or 32
 * bits.
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stm32_hal_stub.h"

#include "amt102.h"
#include "encoder.h"
#include "../rad.h"

#define N	8192	/* counts per turn at 2048 PPR */

static TIM_TypeDef regs;
static TIM_HandleTypeDef tim = { .Instance = &regs };
static bool running;
static int starts, stops;
static HAL_StatusTypeDef start_status;
static int allocs, frees;

HAL_StatusTypeDef HAL_TIM_Encoder_Start(TIM_HandleTypeDef *h, uint32_t ch)
{
	assert(h == &tim && ch == TIM_CHANNEL_ALL);
	starts++;
	if (start_status != HAL_OK)
		return start_status;
	running = true;
	return HAL_OK;
}

HAL_StatusTypeDef HAL_TIM_Encoder_Stop(TIM_HandleTypeDef *h, uint32_t ch)
{
	assert(h == &tim && ch == TIM_CHANNEL_ALL);
	stops++;
	running = false;
	return HAL_OK;
}

/* Counts the driver's calloc() and free() calls; see test/Makefile. */
void *__real_calloc(size_t n, size_t size);
void *__wrap_calloc(size_t n, size_t size);
void *__wrap_calloc(size_t n, size_t size)
{
	allocs++;
	return __real_calloc(n, size);
}
void __real_free(void *p);
void __wrap_free(void *p);
void __wrap_free(void *p)
{
	frees++;
	__real_free(p);
}

/* A timer the app set up, its counter wherever it happens to be. */
static void timer(uint32_t period, uint32_t cnt)
{
	tim.Init.Period = period;
	regs.CNT = cnt;
	running = false;
	starts = stops = 0;
	start_status = HAL_OK;
	allocs = frees = 0;
}

/* The encoder's pulses: move the counter by n, wrapping at its period. */
static void turn(int32_t n, uint32_t period)
{
	regs.CNT = (uint32_t)((uint64_t)regs.CNT + (uint64_t)(int64_t)n) &
		   period;
}

static struct encoder_dev probe_ok(const struct amt102_config *cfg)
{
	struct encoder_dev encoder = { 0 };

	assert(amt102_probe(&encoder, cfg) == 0);
	assert(encoder.ops && encoder.priv);
	return encoder;
}

static void test_probe(void)
{
	struct amt102_config cfg = { .tim = &tim };
	struct encoder_dev encoder;
	struct encoder_sample s;

	timer(0xFFFF, 12345);
	encoder = probe_ok(&cfg);
	assert(!strcmp(encoder.name, "amt102"));
	assert(encoder.counts_per_turn == 2048 * 4 && "0 selects 2048 PPR");
	assert(!encoder.absolute);
	assert(running && starts == 1 && allocs == 1);
	/* Counting from zero, wherever the counter was: it is not written. */
	assert(regs.CNT == 12345);
	assert(encoder_read(&encoder, &s) == 0 && s.total_rad == 0.0);
	amt102_remove(&encoder);
	assert(!running && stops == 1 && frees == 1);
	assert(!encoder.ops && !encoder.priv && !encoder.counts_per_turn);

	cfg.ppr = 500;
	timer(0xFFFFFFFF, 0);
	encoder = probe_ok(&cfg);
	assert(encoder.counts_per_turn == 2000);
	amt102_remove(&encoder);
}

/* A 16-bit timer: forward and back across 0xFFFF, and many turns of it. */
static void test_wrap_16bit(void)
{
	struct amt102_config cfg = { .tim = &tim };
	struct encoder_dev encoder;
	struct encoder_sample s;

	timer(0xFFFF, 0xFFF0);
	encoder = probe_ok(&cfg);
	turn(0x20, 0xFFFF);			/* to 0x0010 */
	assert(encoder_read(&encoder, &s) == 0);
	assert(s.total_rad == total_of(0x20, N));
	turn(-0x40, 0xFFFF);			/* back to 0xFFD0 */
	assert(encoder_read(&encoder, &s) == 0);
	assert(s.total_rad == total_of(-0x20, N));

	/*
	 * Twenty turns forward, read every 32767 counts -- the most a 16-bit
	 * counter can tell from a move the other way -- then back past where
	 * it started in reads of 3 turns.
	 */
	for (int i = 0; i < 20; i += 4) {
		turn(4 * 8192 - 1, 0xFFFF);
		assert(encoder_read(&encoder, &s) == 0);
	}
	assert(s.total_rad == total_of(-0x20 + 5 * (4 * 8192 - 1), N));
	for (int i = 0; i < 10; i++) {
		turn(-(3 * 8192), 0xFFFF);
		assert(encoder_read(&encoder, &s) == 0);
	}
	assert(s.total_rad ==
	       total_of(-0x20 + 5 * (4 * 8192 - 1) - 30 * 8192, N));
	assert(s.total_rad < 0.0);
	amt102_remove(&encoder);
}

/* A 32-bit timer: the wrap is at 0xFFFFFFFF, not 0xFFFF. */
static void test_wrap_32bit(void)
{
	struct amt102_config cfg = { .tim = &tim };
	struct encoder_dev encoder;
	struct encoder_sample s;

	timer(0xFFFFFFFF, 0xFFFFFFF0);
	encoder = probe_ok(&cfg);
	turn(0x20, 0xFFFFFFFF);			/* to 0x00000010 */
	assert(encoder_read(&encoder, &s) == 0);
	assert(s.total_rad == total_of(0x20, N));
	/* Through 0x0000FFFF -> 0x00010000, which is no wrap here: 10 turns
	 * in one read, which a 16-bit counter could not tell apart. */
	turn(10 * 8192, 0xFFFFFFFF);
	assert(encoder_read(&encoder, &s) == 0);
	assert(s.total_rad == total_of(0x20 + 10 * 8192, N));
	turn(-(10 * 8192 + 0x40), 0xFFFFFFFF);	/* back across zero */
	assert(encoder_read(&encoder, &s) == 0);
	assert(s.total_rad == total_of(-0x20, N));
	amt102_remove(&encoder);
}

static void test_zero_and_invert(void)
{
	struct amt102_config cfg = { .tim = &tim };
	struct encoder_dev encoder;
	struct encoder_sample s;

	timer(0xFFFF, 100);
	encoder = probe_ok(&cfg);
	turn(5000, 0xFFFF);
	assert(encoder_set_zero(&encoder) == 0);
	assert(encoder_read(&encoder, &s) == 0 && s.total_rad == 0.0);
	turn(-2048, 0xFFFF);
	assert(encoder_read(&encoder, &s) == 0);
	assert(NEAR(s.total_rad, -M_PI / 2) && NEAR(s.angle_rad, 1.5 * M_PI));
	amt102_remove(&encoder);

	cfg.invert = true;
	timer(0xFFFF, 0);
	encoder = probe_ok(&cfg);
	turn(2048, 0xFFFF);
	assert(encoder_read(&encoder, &s) == 0 && NEAR(s.total_rad, -M_PI / 2));
	amt102_remove(&encoder);
}

/* The index: the app's EXTI callback calls amt102_index_irq(). */
static void test_index(void)
{
	static const struct encoder_ops other_ops = { 0 };
	struct encoder_dev other = { .name = "other", .ops = &other_ops };
	struct amt102_config cfg = { .tim = &tim };
	struct encoder_dev encoder;
	bool hit = true;

	timer(0xFFFF, 0);
	encoder = probe_ok(&cfg);
	assert(amt102_index_hit(&encoder, &hit) == -EOPNOTSUPP);
	amt102_remove(&encoder);

	cfg.index = true;
	encoder = probe_ok(&cfg);
	assert(amt102_index_hit(&encoder, &hit) == 0 && !hit);
	amt102_index_irq(&encoder);
	/* Read and cleared with interrupts masked, PRIMASK put back after. */
	stub_primask = 0;
	stub_irq_masks = 0;
	assert(amt102_index_hit(&encoder, &hit) == 0 && hit);
	assert(stub_irq_masks == 1 && stub_primask == 0);
	stub_primask = 1;		/* called with interrupts already off */
	assert(amt102_index_hit(&encoder, &hit) == 0 && !hit);
	assert(stub_primask == 1 && "not unmasked behind the caller's back");
	stub_primask = 0;
	assert(amt102_index_hit(&encoder, &hit) == 0 && !hit && "cleared");
	amt102_index_irq(&other);		/* not ours: nothing */
	amt102_index_irq(NULL);
	assert(other.ops == &other_ops && !other.priv);
	assert(amt102_index_hit(&other, &hit) == -EINVAL);
	assert(amt102_index_hit(&encoder, NULL) == -EINVAL);
	amt102_remove(&encoder);
}

static void test_refusals(void)
{
	static const struct {
		HAL_StatusTypeDef status;
		int expected;
	} map[] = {
		{ HAL_ERROR,	-EIO },
		{ HAL_BUSY,	-EAGAIN },
		{ HAL_TIMEOUT,	-ETIMEDOUT },
	};
	struct amt102_config cfg = { .tim = &tim };
	struct encoder_dev encoder = { 0 };

	/* A counter that does not run its full range wraps early: refused,
	 * before anything is allocated or started. */
	timer(0x7FFF, 0);
	assert(amt102_probe(&encoder, &cfg) == -EINVAL);
	timer(1000, 0);
	assert(amt102_probe(&encoder, &cfg) == -EINVAL);
	timer(0xFFFFFFFE, 0);
	assert(amt102_probe(&encoder, &cfg) == -EINVAL);
	cfg.tim = NULL;
	assert(amt102_probe(&encoder, &cfg) == -EINVAL);
	assert(starts == 0 && allocs == 0 && !encoder.ops);
	cfg.tim = &tim;

	/* The HAL refusing to start: mapped, nothing leaked. */
	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
		timer(0xFFFF, 0);
		start_status = map[i].status;
		assert(amt102_probe(&encoder, &cfg) == map[i].expected);
		assert(allocs == 1 && frees == 1 && !encoder.ops);
	}

	timer(0xFFFF, 0);
	encoder = probe_ok(&cfg);
	allocs = starts = 0;
	assert(amt102_probe(&encoder, &cfg) == -EBUSY);
	assert(allocs == 0 && starts == 0);
	amt102_remove(&encoder);
	amt102_remove(&encoder);		/* unprobed now: a no-op */
	assert(stops == 1);
	assert(amt102_probe(NULL, &cfg) == -EINVAL);
	assert(amt102_probe(&encoder, NULL) == -EINVAL);
}

int main(void)
{
	test_probe();
	test_wrap_16bit();
	test_wrap_32bit();
	test_zero_and_invert();
	test_index();
	test_refusals();
	printf("amt102 driver checks passed\n");
	return 0;
}
