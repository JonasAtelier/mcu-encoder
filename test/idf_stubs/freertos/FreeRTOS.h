#ifndef FREERTOS_H
#define FREERTOS_H

#include <assert.h>
#include <stdint.h>

/*
 * Host stub: just the spinlock the AMT102 index flag uses. It does not lock
 * anything -- the host tests are single-threaded -- but it asserts that
 * every enter has its exit and that nobody enters twice, and that the lock
 * was initialised (calloc's all-zero is not unlocked on the real part).
 */
#define STUB_MUX_UNLOCKED	0x5A
#define STUB_MUX_LOCKED		0xA5

typedef struct {
	int state;
} portMUX_TYPE;

#define portMUX_INITIALIZE(m)	((m)->state = STUB_MUX_UNLOCKED)
#define portENTER_CRITICAL(m) \
	do { assert((m)->state == STUB_MUX_UNLOCKED); \
	     (m)->state = STUB_MUX_LOCKED; } while (0)
#define portEXIT_CRITICAL(m) \
	do { assert((m)->state == STUB_MUX_LOCKED); \
	     (m)->state = STUB_MUX_UNLOCKED; } while (0)
#define portENTER_CRITICAL_ISR(m)	portENTER_CRITICAL(m)
#define portEXIT_CRITICAL_ISR(m)	portEXIT_CRITICAL(m)

/*
 * Ticks, for the bus port's UART timeout. The tick rate is a variable so a
 * test can move it; it starts at ESP-IDF's default 100 Hz (see
 * idf_stubs.c), which is where pdMS_TO_TICKS() truncates a short delay to
 * zero ticks. pdMS_TO_TICKS truncates as IDF's does.
 */
extern unsigned int stub_tick_hz;

#define configTICK_RATE_HZ	stub_tick_hz

typedef uint32_t TickType_t;

#define pdMS_TO_TICKS(msec) \
	((TickType_t)(((uint64_t)(msec) * configTICK_RATE_HZ) / 1000U))

#endif
