/*
 * encoder -- a small Linux-IIO-style rotary encoder subsystem for ESP-IDF
 * and the STM32 HAL.
 *
 * One struct encoder_dev per encoder. A chip driver's probe fills in its
 * name, its ops, its private state, its counts per turn and whether it is
 * absolute; encoder_read() does the rest: timestamp, the chip's own read,
 * then counts into radians. Code above this header sees one shape of sample
 * whichever chip produced it.
 *
 * Absolute or incremental matters. An absolute encoder (AS5047P, AS5600)
 * knows where the shaft is within one turn the moment it powers up; its zero
 * is the chip's own until encoder_set_zero() moves it. An incremental one
 * (AMT102) only counts movement, so its zero is wherever the shaft happened
 * to be at power-up -- arbitrary until you home it.
 *
 * Every int-returning function returns 0 or a negative errno.
 */
#ifndef ENCODER_H
#define ENCODER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * No velocity here, deliberately. It takes two samples, and the right amount
 * of smoothing depends on your loop, not on the chip. total_rad and t_us are
 * enough: (total - last_total) / (t_us - last_t_us).
 */
struct encoder_sample {
	float angle_rad;	/* 0 <= angle < 2*pi, from the zero point */
	/*
	 * Across turns from the zero point, signed. A double, not a float: a
	 * float runs out of bits for one count after ~1000 turns of a 14-bit
	 * encoder, and velocity from two totals would then be mostly rounding.
	 */
	double total_rad;
	int64_t t_us;		/* encoder_time_us() at read */
};

struct encoder_dev;

struct encoder_ops {
	/* signed counts since the zero point, across turns */
	int (*read)(struct encoder_dev *encoder, int64_t *counts);
	/* make the current position the zero point; restart the turn count */
	int (*set_zero)(struct encoder_dev *encoder);
};

/*
 * Must start zeroed -- static storage or `= { 0 }` -- because probe treats a
 * non-NULL ops or priv as "already bound" and returns -EBUSY rather than
 * leak the first binding. remove, or a failed probe, zeroes them again, so
 * the same encoder_dev can be probed afresh.
 */
struct encoder_dev {
	const char *name;		/* "as5047p"; log prefix */
	const struct encoder_ops *ops;
	void *priv;			/* chip state, owned by probe/remove */
	int32_t counts_per_turn;	/* set by probe */
	bool absolute;			/* false: zero is where it powered up */
};

/*
 * Timestamp, ops->read, then counts into radians. *sample is written only on
 * success.
 *
 * The multi-turn count is tracked in software from single-turn readings on
 * the absolute chips, so call this often enough that the shaft cannot move
 * half a turn between calls, or a revolution is missed.
 */
int encoder_read(struct encoder_dev *encoder, struct encoder_sample *sample);

/*
 * Make wherever the shaft is now read as 0 rad, and restart the turn
 * count. Lives in RAM only -- call it after every boot if you need it.
 */
int encoder_set_zero(struct encoder_dev *encoder);

/*
 * The clock encoder_read() stamps samples with, in microseconds. On ESP-IDF
 * it is esp_timer_get_time(). On the STM32 HAL the default is weak and only
 * HAL_GetTick() * 1000: millisecond steps that wrap after 49.7 days. Define
 * your own encoder_time_us() for real microseconds -- the DWT->CYCCNT cycle
 * counter divided by the core clock in MHz, or a free running 32-bit timer
 * at 1 MHz extended to 64 bits.
 */
int64_t encoder_time_us(void);

#ifdef __cplusplus
}
#endif

#endif /* ENCODER_H */
