/*
 * Host-side check of the AS5600 driver against a fake chip on a stubbed
 * i2c_master. No hardware, no ESP toolchain -- see test/Makefile.
 *
 * The fake is a 256-byte register file. The test turns the "shaft" by
 * writing a count into ANGLE, then checks what encoder_read() makes of it:
 * wraps across 0 both ways, invert, set_zero, and that a failed read changes
 * nothing -- not the sample, not the turn count.
 */
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "as5600.h"
#include "as5600_regs.h"
#include "encoder.h"

#define PI	3.14159265358979323846
#define K	(2 * PI / AS5600_COUNTS)	/* radians per count */

static uint8_t regs[256];
static uint16_t dev_addr;
static uint32_t dev_hz;
static int added, removed, reads, writes;
static esp_err_t bus_error;	/* every transfer fails with this if set */
/*
 * Nothing at 0x36. As IDF does it: i2c_master_probe() says NOT_FOUND, but a
 * NACKed transfer says only INVALID_STATE.
 */
static bool absent;
static bool nack_reads;	/* ACKs i2c_master_probe, NACKs every read */
static int probes;

#define BUS ((i2c_master_bus_handle_t)(void *)regs)

static void chip_reset(void)
{
	memset(regs, 0, sizeof(regs));
	/* factory bits in CONF that probe must leave alone */
	regs[AS5600_CONF_H] = 0x3C;
	regs[AS5600_CONF_L] = 0xF3;
	added = removed = reads = writes = probes = 0;
	bus_error = ESP_OK;
	absent = nack_reads = false;
}

/*
 * Turn the shaft to count c. The high nibble of each _H register is not
 * part of the angle; junk there must be masked off.
 */
static void shaft(uint16_t c)
{
	regs[AS5600_ANGLE_H] = (uint8_t)(0xA0 | (c >> 8));
	regs[AS5600_ANGLE_H + 1] = (uint8_t)c;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
				    const i2c_device_config_t *cfg,
				    i2c_master_dev_handle_t *out)
{
	(void)bus;
	assert(cfg->dev_addr_length == I2C_ADDR_BIT_LEN_7);
	dev_addr = cfg->device_address;
	dev_hz = cfg->scl_speed_hz;
	added++;
	*out = (i2c_master_dev_handle_t)(void *)regs;
	return ESP_OK;
}

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address,
			   int xfer_timeout_ms)
{
	(void)bus;
	(void)xfer_timeout_ms;
	probes++;
	if (bus_error != ESP_OK)
		return bus_error;
	return (absent || address != AS5600_ADDR) ? ESP_ERR_NOT_FOUND : ESP_OK;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t d)
{
	(void)d;
	removed++;
	return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t d, const uint8_t *data,
			      size_t len, int t)
{
	(void)d;
	(void)t;
	assert(len >= 2);
	if (bus_error != ESP_OK)
		return bus_error;
	if (absent)
		return ESP_ERR_INVALID_STATE;
	writes++;
	for (size_t i = 1; i < len; i++)
		regs[(data[0] + i - 1) & 0xFF] = data[i];
	return ESP_OK;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t d,
				      const uint8_t *tx, size_t tx_len,
				      uint8_t *rx, size_t rx_len, int t)
{
	(void)d;
	(void)t;
	assert(tx_len == 1);
	if (bus_error != ESP_OK)
		return bus_error;
	if (absent || nack_reads)
		return ESP_ERR_INVALID_STATE;
	reads++;
	for (size_t i = 0; i < rx_len; i++)
		rx[i] = regs[(tx[0] + i) & 0xFF];
	return ESP_OK;
}

/* Counts the driver's calloc() calls; linked with -Wl,--wrap=calloc. */
static int allocs;
void *__real_calloc(size_t n, size_t size);
void *__wrap_calloc(size_t n, size_t size)
{
	allocs++;
	return __real_calloc(n, size);
}

static struct encoder_dev open_ok(const struct as5600_config *cfg)
{
	struct encoder_dev enc = { 0 };

	chip_reset();
	assert(as5600_probe(&enc, BUS, cfg) == 0);
	assert(enc.ops && enc.priv);
	return enc;
}

/* Read and check against the counts since zero we expect. */
static void expect(struct encoder_dev *enc, int64_t since_zero)
{
	struct encoder_sample s;
	int64_t turn = ((since_zero % AS5600_COUNTS) + AS5600_COUNTS) %
		       AS5600_COUNTS;

	assert(encoder_read(enc, &s) == 0);
	assert(fabs(s.total_rad - (double)since_zero * K) < 1e-9);
	assert(fabs(s.angle_rad - (double)turn * K) < 1e-5);
	assert(s.angle_rad >= 0.0f && s.angle_rad < (float)(2 * PI));
}

/* ------------------------------------------------------------- tests ----- */

static void test_probe(void)
{
	struct as5600_config c = { .filter = AS5600_FILTER_8X,
				   .hysteresis = AS5600_HYST_2LSB,
				   .scl_speed_hz = 1000000 };
	struct encoder_dev enc = { 0 };

	/* NULL config: defaults, and CONF left as the factory had it. */
	chip_reset();
	assert(as5600_probe(&enc, BUS, NULL) == 0);
	assert(!strcmp(enc.name, "as5600"));
	assert(dev_addr == 0x36 && dev_hz == 400000);
	assert(enc.counts_per_turn == AS5600_COUNTS && enc.absolute);
	assert(regs[AS5600_CONF_H] == 0x3C && regs[AS5600_CONF_L] == 0xF3);
	as5600_remove(&enc);
	assert(!enc.ops && !enc.priv && "remove zeroes the binding");
	assert(enc.counts_per_turn == 0 && !enc.absolute);
	assert(added == 1 && removed == 1);

	/* Filter into SF (bits 1:0), hysteresis into HYST (bits 3:2), rest kept. */
	chip_reset();
	assert(as5600_probe(&enc, BUS, &c) == 0);
	assert(dev_hz == 1000000);
	assert(regs[AS5600_CONF_H] == 0x3D);
	assert(regs[AS5600_CONF_L] == 0xFB);
	as5600_remove(&enc);

	/*
	 * Nothing ACKs: i2c_master_probe() says so, and probe turns it into
	 * -ENXIO before allocating or adding anything. A NACKed transfer
	 * alone is only INVALID_STATE, which would have been -EIO.
	 */
	chip_reset();
	absent = true;
	allocs = 0;
	assert(as5600_probe(&enc, BUS, &c) == -ENXIO);
	assert(strstr(stub_log_last, "E encoder: as5600: nothing answers at 0x36"));
	assert(!enc.ops && !enc.priv && enc.counts_per_turn == 0);
	assert(probes == 1 && added == 0 && allocs == 0);

	/* The chip goes away after probe: a NACKed read is -EIO. */
	chip_reset();
	assert(as5600_probe(&enc, BUS, &c) == 0);
	absent = true;
	{
		struct encoder_sample s;

		assert(encoder_read(&enc, &s) == -EIO);
	}
	as5600_remove(&enc);

	/* ACKs its address but NACKs the register read: -EIO, and undone. */
	chip_reset();
	nack_reads = true;
	assert(as5600_probe(&enc, BUS, &c) == -EIO);
	assert(!enc.ops && !enc.priv && added == 1 && removed == 1);

	/* A bus timeout on the probe itself is -ETIMEDOUT, not "absent". */
	chip_reset();
	bus_error = ESP_ERR_TIMEOUT;
	assert(as5600_probe(&enc, BUS, &c) == -ETIMEDOUT);
	assert(!enc.ops && !enc.priv && added == 0);

	/* Out-of-range enums are refused before calloc and the bus. */
	chip_reset();
	allocs = 0;
	c.filter = (enum as5600_filter)4;
	assert(as5600_probe(&enc, BUS, &c) == -EINVAL);
	c.filter = AS5600_FILTER_16X;
	c.hysteresis = (enum as5600_hysteresis)4;
	assert(as5600_probe(&enc, BUS, &c) == -EINVAL);
	assert(allocs == 0 && added == 0 && reads == 0);
	assert(!enc.ops && !enc.priv);
}

static void test_absolute_before_zero(void)
{
	struct encoder_dev enc = open_ok(NULL);

	/* Until set_zero, the chip's own 0 is the zero: it is absolute. */
	shaft(1024);
	expect(&enc, 1024);
	shaft(4095);
	expect(&enc, -1);	/* 1024 -> 4095 is backward across 0 */
	as5600_remove(&enc);
}

static void test_wrap_forward(void)
{
	struct encoder_dev enc = open_ok(NULL);

	/* The README's five-sample crossing, zero at count 0. */
	shaft(0);
	assert(encoder_set_zero(&enc) == 0);
	shaft(3968);
	expect(&enc, 3968 - 4096);	/* from 0 that far is backward */
	assert(encoder_set_zero(&enc) == 0);
	shaft(3968);
	expect(&enc, 0);
	shaft(4032);
	expect(&enc, 64);
	shaft(0);
	expect(&enc, 128);		/* 360 degrees past 3968's zero... */
	shaft(64);
	expect(&enc, 192);
	shaft(128);
	expect(&enc, 256);
	/* ...and on for three whole turns in steps under half a turn. */
	for (int i = 1; i <= 3 * 4; i++) {
		shaft((uint16_t)((128 + i * 1024) % 4096));
		expect(&enc, 256 + i * 1024);
	}
	as5600_remove(&enc);
}

static void test_wrap_backward_and_zero(void)
{
	struct encoder_dev enc = open_ok(NULL);

	shaft(1000);
	assert(encoder_set_zero(&enc) == 0);
	expect(&enc, 0);
	shaft(10);
	expect(&enc, -990);
	shaft(4090);
	expect(&enc, -1006);		/* across 0 backward */
	shaft(3000);
	expect(&enc, -2096);
	shaft(1500);
	expect(&enc, -3596);
	shaft(200);
	expect(&enc, -4896);		/* more than a turn back */

	/* set_zero mid-turn restarts both angle and total from here. */
	assert(encoder_set_zero(&enc) == 0);
	expect(&enc, 0);
	shaft(4000);
	expect(&enc, -296);
	shaft(300);
	expect(&enc, 100);
	as5600_remove(&enc);
}

static void test_invert(void)
{
	struct as5600_config c = { .invert = true };
	struct encoder_dev enc = open_ok(&c);
	uint16_t n;

	/* Inverted, the chip's 0 is still 0 and one count up is one count back. */
	shaft(0);
	expect(&enc, 0);
	shaft(1);
	expect(&enc, -1);
	/* Physically forward across 0 is backward in software, and vice versa. */
	shaft(4000);
	expect(&enc, 96);
	shaft(100);
	expect(&enc, -100);
	assert(encoder_set_zero(&enc) == 0);
	shaft(4095);
	expect(&enc, 101);		/* 100 -> 4095 is back 101, inverted +101 */
	as5600_remove(&enc);

	enc = open_ok(&c);
	shaft(100);
	assert(encoder_set_zero(&enc) == 0);
	shaft(50);
	expect(&enc, 50);
	shaft(4050);
	expect(&enc, 146);		/* across 0 physically back 96 = forward 96 */

	/* The raw reader takes RAW_ANGLE, inverted, with no zero applied. */
	regs[AS5600_RAW_ANGLE_H] = 0xF0 | 0x00;
	regs[AS5600_RAW_ANGLE_H + 1] = 0x01;
	assert(as5600_read_counts(&enc, &n) == 0 && n == 4095);
	as5600_remove(&enc);

	enc = open_ok(NULL);
	assert(as5600_read_counts(&enc, &n) == 0 && n == 0);
	regs[AS5600_RAW_ANGLE_H] = 0xFF;
	regs[AS5600_RAW_ANGLE_H + 1] = 0xFF;
	assert(as5600_read_counts(&enc, &n) == 0 && n == 4095);
	as5600_remove(&enc);
}

static void test_failed_read_changes_nothing(void)
{
	struct encoder_dev enc = open_ok(NULL);
	struct encoder_sample s = { .angle_rad = 9.0f, .total_rad = 9.0,
				    .t_us = 9 };

	shaft(4000);
	assert(encoder_set_zero(&enc) == 0);
	/* A bus error on the crossing read: the error comes back, s is untouched. */
	shaft(100);
	bus_error = ESP_ERR_TIMEOUT;
	assert(encoder_read(&enc, &s) == -ETIMEDOUT);
	assert(encoder_set_zero(&enc) == -ETIMEDOUT);
	assert(s.angle_rad == 9.0f && s.total_rad == 9.0 && s.t_us == 9);
	bus_error = ESP_OK;
	/* ...and the tracker did not see it: the crossing still counts once. */
	expect(&enc, 196);
	as5600_remove(&enc);
}

static void test_status(void)
{
	struct encoder_dev enc = open_ok(NULL);
	struct as5600_status st;

	regs[AS5600_STATUS] = AS5600_STATUS_MD | AS5600_STATUS_ML;
	regs[AS5600_AGC] = 200;
	regs[AS5600_MAGNITUDE_H] = 0xF5;	/* high nibble is not data */
	regs[AS5600_MAGNITUDE_H + 1] = 0x67;
	assert(as5600_read_status(&enc, &st) == 0);
	assert(st.magnet_detected && st.magnet_too_weak && !st.magnet_too_strong);
	assert(st.agc == 200 && st.magnitude == 0x567);

	regs[AS5600_STATUS] = AS5600_STATUS_MH;
	assert(as5600_read_status(&enc, &st) == 0);
	assert(!st.magnet_detected && !st.magnet_too_weak && st.magnet_too_strong);
	as5600_remove(&enc);
}

static int other_read(struct encoder_dev *enc, int64_t *counts)
{
	(void)enc;
	(void)counts;
	return 0;
}

static void test_null_and_foreign(void)
{
	static const struct encoder_ops other_ops = { .read = other_read };
	struct encoder_dev other = { .name = "other", .ops = &other_ops,
				     .priv = regs };
	struct encoder_dev enc = { 0 };
	struct as5600_status st;
	uint16_t n;

	chip_reset();
	allocs = 0;
	assert(as5600_probe(NULL, BUS, NULL) == -EINVAL);
	assert(as5600_probe(&enc, NULL, NULL) == -EINVAL);
	assert(added == 0 && allocs == 0);
	assert(as5600_read_counts(NULL, &n) == -EINVAL);
	assert(as5600_read_status(NULL, &st) == -EINVAL);
	as5600_remove(NULL);

	/* Unprobed: every extra refuses it. */
	assert(as5600_read_counts(&enc, &n) == -EINVAL);
	assert(as5600_read_status(&enc, &st) == -EINVAL);
	as5600_remove(&enc);

	/* Another chip's encoder_dev is refused, never cast to ours. */
	assert(as5600_read_counts(&other, &n) == -EINVAL);
	assert(as5600_read_status(&other, &st) == -EINVAL);
	as5600_remove(&other);
	assert(other.ops == &other_ops && other.priv == regs &&
	       "not ours to remove");
	assert(reads == 0 && writes == 0 && "and no bus traffic for it");

	enc = open_ok(NULL);
	assert(as5600_read_counts(&enc, NULL) == -EINVAL);
	assert(as5600_read_status(&enc, NULL) == -EINVAL);
	as5600_remove(&enc);
}

static void test_second_probe_is_refused(void)
{
	static const struct encoder_ops other_ops = { .read = other_read };
	struct encoder_dev other = { .name = "other", .ops = &other_ops };
	struct encoder_dev enc = open_ok(NULL);
	void *priv = enc.priv;
	int a = added, r = reads, w = writes, n = allocs;
	uint16_t c;

	assert(as5600_probe(&enc, BUS, NULL) == -EBUSY);
	assert(added == a && reads == r && writes == w && allocs == n);
	assert(enc.priv == priv && as5600_read_counts(&enc, &c) == 0 &&
	       "first binding intact");
	as5600_remove(&enc);

	/* Bound by another driver: refused too, and left exactly as it was. */
	assert(as5600_probe(&other, BUS, NULL) == -EBUSY);
	assert(!strcmp(other.name, "other") && other.ops == &other_ops);

	/* A removed device can be probed afresh. */
	chip_reset();
	assert(as5600_probe(&enc, BUS, NULL) == 0);
	as5600_remove(&enc);
}

int main(void)
{
	test_probe();
	test_absolute_before_zero();
	test_wrap_forward();
	test_wrap_backward_and_zero();
	test_invert();
	test_failed_read_changes_nothing();
	test_status();
	test_null_and_foreign();
	test_second_probe_is_refused();
	printf("all as5600 driver checks passed\n");
	return 0;
}
