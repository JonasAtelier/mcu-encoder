/*
 * Host-side check of the AS5047P driver against a fake chip on a stubbed
 * spi_master. No hardware, no ESP toolchain -- see test/Makefile.
 *
 * The fake models what makes this chip easy to get wrong: reads are
 * pipelined (a frame answers the command of the frame before it), every
 * frame carries even parity in bit 15 both ways, and bit 14 of a reply is the
 * error flag. It checks the parity of every frame the driver sends with the
 * compiler's own parity builtin, not the driver's function.
 */
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "as5047p.h"
#include "as5047p_regs.h"
#include "encoder.h"

#define PI	3.14159265358979323846
#define N	AS5047P_COUNTS
#define K	(2 * PI / N)

static uint16_t chip[0x4000];
static uint16_t pending;	/* the reply the next frame clocks out */
static bool stuck_on;
static uint16_t stuck;		/* MISO value when stuck_on */
static int corrupt_reads, error_reads;	/* next N register reads go wrong */
static bool ef_latched;		/* EF in every reply until ERRFL is read */
static int errfl_reads;
static int added, removed, frames;
static spi_device_interface_config_t devcfg;
static spi_host_device_t devhost;
static esp_err_t bus_error;
static esp_err_t add_error;	/* spi_bus_add_device fails with this if set */

static uint16_t even(uint16_t v)
{
	v &= 0x7FFF;
	return __builtin_parity(v) ? (uint16_t)(v | 0x8000) : v;
}

static void chip_reset(void)
{
	memset(chip, 0, sizeof(chip));
	chip[AS5047P_DIAAGC] = AS5047P_DIAAGC_LF | 0x80;
	chip[AS5047P_MAG] = 0x1234;
	/* The first frame after power-up reports a framing error. */
	pending = even(AS5047P_FRAME_ERROR);
	ef_latched = true;
	errfl_reads = 0;
	stuck_on = false;
	corrupt_reads = error_reads = 0;
	added = removed = frames = 0;
	bus_error = ESP_OK;
	add_error = ESP_OK;
}

/* Turn the shaft: DAEC and uncompensated registers differ, to tell them apart. */
static void shaft(uint16_t c)
{
	chip[AS5047P_ANGLECOM] = c;
	chip[AS5047P_ANGLEUNC] = (uint16_t)((c + 5) % N);
}

esp_err_t spi_bus_add_device(spi_host_device_t host,
			     const spi_device_interface_config_t *cfg,
			     spi_device_handle_t *out)
{
	if (add_error != ESP_OK)
		return add_error;
	devhost = host;
	devcfg = *cfg;
	added++;
	*out = (spi_device_handle_t)(void *)chip;
	return ESP_OK;
}

esp_err_t spi_bus_remove_device(spi_device_handle_t h)
{
	(void)h;
	removed++;
	return ESP_OK;
}

esp_err_t spi_device_polling_transmit(spi_device_handle_t h, spi_transaction_t *t)
{
	uint16_t f, reply, addr;

	(void)h;
	assert(t->length == 16);
	assert(t->flags == (SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA));
	if (bus_error != ESP_OK)
		return bus_error;
	frames++;

	f = (uint16_t)((t->tx_data[0] << 8) | t->tx_data[1]);
	assert(__builtin_parity(f) == 0 && "driver sends even parity");
	assert(f & AS5047P_CMD_READ && "the driver only reads");

	reply = pending;
	addr = f & AS5047P_FRAME_DATA;
	/* Reading ERRFL is what clears EF; nothing else does. */
	if (addr == AS5047P_ERRFL) {
		ef_latched = false;
		errfl_reads++;
	}
	if (addr != AS5047P_NOP && error_reads > 0) {
		error_reads--;
		ef_latched = true;
	}
	pending = ef_latched ? even(AS5047P_FRAME_ERROR) : even(chip[addr]);
	if (addr != AS5047P_NOP && corrupt_reads > 0) {
		corrupt_reads--;
		pending ^= 0x8000;
	}
	if (stuck_on)
		reply = stuck;

	t->rx_data[0] = (uint8_t)(reply >> 8);
	t->rx_data[1] = (uint8_t)reply;
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

static const struct as5047p_config base = {
	.host = SPI2_HOST,
	.cs_gpio = GPIO_NUM_5,
};

static struct encoder_dev open_ok(const struct as5047p_config *cfg)
{
	struct encoder_dev enc = { 0 };

	chip_reset();
	assert(as5047p_probe(&enc, cfg) == 0);
	assert(enc.ops && enc.priv);
	return enc;
}

static void expect(struct encoder_dev *enc, int64_t since_zero)
{
	struct encoder_sample s;
	int64_t turn = ((since_zero % N) + N) % N;

	assert(encoder_read(enc, &s) == 0);
	assert(fabs(s.total_rad - (double)since_zero * K) < 1e-9);
	assert(fabs(s.angle_rad - (double)turn * K) < 1e-5);
	assert(s.angle_rad >= 0.0f && s.angle_rad < (float)(2 * PI));
}

/* ------------------------------------------------------------- tests ----- */

static void test_probe(void)
{
	struct as5047p_config c = base;
	struct encoder_dev enc = { 0 };

	chip_reset();
	assert(as5047p_probe(&enc, &c) == 0);
	assert(!strcmp(enc.name, "as5047p"));
	assert(devhost == SPI2_HOST && devcfg.spics_io_num == 5);
	assert(devcfg.mode == 1 && "CPOL 0, CPHA 1");
	assert(devcfg.clock_speed_hz == 4000000 && devcfg.queue_size == 1);
	assert(frames == 4 && "ERRFL clear, then DIAAGC: two frames each");
	assert(enc.counts_per_turn == N && enc.absolute);
	as5047p_remove(&enc);
	assert(!enc.ops && !enc.priv && "remove zeroes the binding");
	assert(enc.counts_per_turn == 0 && !enc.absolute);
	assert(added == 1 && removed == 1);

	chip_reset();
	c.spi_hz = 10000000;
	assert(as5047p_probe(&enc, &c) == 0);
	assert(devcfg.clock_speed_hz == 10000000);
	as5047p_remove(&enc);

	/* MISO stuck either way is nothing answering, not a wrong chip. */
	chip_reset();
	stuck_on = true;
	stuck = 0x0000;
	assert(as5047p_probe(&enc, &base) == -ENXIO);
	assert(strstr(stub_log_last, "E encoder: as5047p: MISO stuck at 0x0000"));
	assert(!enc.ops && !enc.priv && added == 1 && removed == 1);
	chip_reset();
	stuck_on = true;
	stuck = 0xFFFF;		/* good parity, EF set: still -ENXIO */
	assert(as5047p_probe(&enc, &base) == -ENXIO);
	assert(strstr(stub_log_last, "E encoder: as5047p: MISO stuck at 0xffff"));
	chip_reset();
	chip[AS5047P_DIAAGC] = AS5047P_FRAME_DATA;	/* 0x3FFF, even parity */
	assert(as5047p_probe(&enc, &base) == -ENXIO);

	/* A reply with bad parity, or with EF set, fails the probe. */
	chip_reset();
	corrupt_reads = 2;	/* ERRFL (ignored) and DIAAGC */
	assert(as5047p_probe(&enc, &base) == -EBADMSG);
	assert(strstr(stub_log_last, "bad parity"));
	assert(!enc.ops && !enc.priv && added == 1 && removed == 1);
	chip_reset();
	error_reads = 2;
	assert(as5047p_probe(&enc, &base) == -EIO);
	assert(strstr(stub_log_last, "an error frame"));

	/* IDF says NOT_FOUND for "no free CS slot": that is -ENOSPC, not absent. */
	chip_reset();
	allocs = 0;
	add_error = ESP_ERR_NOT_FOUND;
	assert(as5047p_probe(&enc, &base) == -ENOSPC);
	add_error = ESP_ERR_INVALID_ARG;
	assert(as5047p_probe(&enc, &base) == -EINVAL);
	assert(!enc.ops && !enc.priv && added == 0 && frames == 0);

	/* A bus failure is the errno of the failed transfer. */
	chip_reset();
	bus_error = ESP_ERR_TIMEOUT;
	assert(as5047p_probe(&enc, &base) == -ETIMEDOUT);
	assert(!enc.ops && !enc.priv && added == 1 && removed == 1);
}

static void test_daec_register(void)
{
	struct as5047p_config c = base;
	struct encoder_dev enc = open_ok(&c);
	uint16_t n;

	shaft(1000);
	assert(as5047p_read_counts(&enc, &n) == 0 && n == 1000 && "ANGLECOM");
	expect(&enc, 1000);
	as5047p_remove(&enc);

	c.raw_angle = true;
	enc = open_ok(&c);
	shaft(1000);
	assert(as5047p_read_counts(&enc, &n) == 0 && n == 1005 && "ANGLEUNC");
	as5047p_remove(&enc);
}

static void test_wraps_and_zero(void)
{
	struct encoder_dev enc = open_ok(&base);

	/* Before set_zero the angle is the chip's absolute one. */
	shaft(16000);
	expect(&enc, 16000);
	shaft(300);
	expect(&enc, 16384 + 300);		/* forward across 0 */

	shaft(8000);
	assert(encoder_set_zero(&enc) == 0);
	expect(&enc, 0);
	/* Forward three turns in steps under half a turn. */
	for (int i = 1; i <= 12; i++) {
		shaft((uint16_t)((8000 + i * 4096) % N));
		expect(&enc, i * 4096);
	}
	/* And back five, past the zero and below it. */
	for (int i = 11; i >= -8; i--) {
		shaft((uint16_t)(((8000 + i * 4096) % N + N) % N));
		expect(&enc, i * 4096);
	}
	/* set_zero mid-turn restarts both angle and total from here. */
	shaft(12);
	expect(&enc, (12 - 8000) - 8 * 4096 - 0);
	assert(encoder_set_zero(&enc) == 0);
	expect(&enc, 0);
	shaft(16380);
	expect(&enc, -16);
	as5047p_remove(&enc);
}

static void test_invert(void)
{
	struct as5047p_config c = base;
	struct encoder_dev enc;
	uint16_t n;

	c.invert = true;
	enc = open_ok(&c);
	shaft(0);
	expect(&enc, 0);
	assert(as5047p_read_counts(&enc, &n) == 0 && n == 0);
	shaft(1);
	expect(&enc, -1);
	assert(as5047p_read_counts(&enc, &n) == 0 && n == 16383);
	shaft(100);
	assert(encoder_set_zero(&enc) == 0);
	shaft(16300);				/* physically back 184 across 0 */
	expect(&enc, 184);
	as5047p_remove(&enc);
}

static void test_failed_read_changes_nothing(void)
{
	struct encoder_dev enc = open_ok(&base);
	struct encoder_sample s = { .angle_rad = 9.0f, .total_rad = 9.0,
				    .t_us = 9 };

	shaft(16000);
	assert(encoder_set_zero(&enc) == 0);
	shaft(200);
	corrupt_reads = 1;
	assert(encoder_read(&enc, &s) == -EBADMSG);
	error_reads = 1;
	assert(encoder_read(&enc, &s) == -EIO);
	bus_error = ESP_ERR_TIMEOUT;
	assert(encoder_read(&enc, &s) == -ETIMEDOUT);
	bus_error = ESP_OK;
	assert(s.angle_rad == 9.0f && s.total_rad == 9.0 && s.t_us == 9);
	/* The crossing is counted once, on the first good read. */
	expect(&enc, 584);
	as5047p_remove(&enc);
}

/*
 * EF is sticky on the real part: once a frame goes wrong, every reply
 * carries it until ERRFL is read. One glitch must cost one read, not every
 * read until a re-probe.
 */
static void test_error_flag_is_cleared(void)
{
	struct encoder_dev enc = open_ok(&base);
	struct as5047p_status st;
	struct encoder_sample s;
	int before;

	shaft(1000);
	before = errfl_reads;
	error_reads = 1;
	assert(encoder_read(&enc, &s) == -EIO);
	assert(errfl_reads == before + 1 && "the failed read cleared EF");
	assert(!ef_latched);
	expect(&enc, 1000);

	/* Same through the status extra. */
	error_reads = 1;
	assert(as5047p_read_status(&enc, &st) == -EIO);
	assert(as5047p_read_status(&enc, &st) == 0 && st.offset_ready);

	/* Bad parity is not EF: no ERRFL read for it. */
	before = errfl_reads;
	corrupt_reads = 1;
	assert(encoder_read(&enc, &s) == -EBADMSG);
	assert(errfl_reads == before);
	expect(&enc, 1000);
	as5047p_remove(&enc);
}

static void test_status(void)
{
	struct encoder_dev enc = open_ok(&base);
	struct as5047p_status st;

	chip[AS5047P_DIAAGC] = AS5047P_DIAAGC_MAGL | AS5047P_DIAAGC_COF | 0xAB;
	assert(as5047p_read_status(&enc, &st) == 0);
	assert(st.magnet_too_weak && !st.magnet_too_strong);
	assert(st.cordic_overflow && !st.offset_ready);
	assert(st.agc == 0xAB && st.magnitude == 0x1234);

	chip[AS5047P_DIAAGC] = AS5047P_DIAAGC_MAGH | AS5047P_DIAAGC_LF;
	assert(as5047p_read_status(&enc, &st) == 0);
	assert(!st.magnet_too_weak && st.magnet_too_strong);
	assert(!st.cordic_overflow && st.offset_ready && st.agc == 0);

	corrupt_reads = 1;
	assert(as5047p_read_status(&enc, &st) == -EBADMSG);
	as5047p_remove(&enc);
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
				     .priv = chip };
	struct encoder_dev enc = { 0 };
	struct as5047p_status st;
	uint16_t n;

	chip_reset();
	allocs = 0;
	assert(as5047p_probe(NULL, &base) == -EINVAL);
	assert(as5047p_probe(&enc, NULL) == -EINVAL);
	assert(added == 0 && allocs == 0);
	assert(as5047p_read_counts(NULL, &n) == -EINVAL);
	assert(as5047p_read_status(NULL, &st) == -EINVAL);
	as5047p_remove(NULL);

	assert(as5047p_read_counts(&enc, &n) == -EINVAL);
	assert(as5047p_read_status(&enc, &st) == -EINVAL);
	as5047p_remove(&enc);

	assert(as5047p_read_counts(&other, &n) == -EINVAL);
	assert(as5047p_read_status(&other, &st) == -EINVAL);
	as5047p_remove(&other);
	assert(other.ops == &other_ops && other.priv == chip &&
	       "not ours to remove");
	assert(frames == 0 && "and no bus traffic for it");

	enc = open_ok(&base);
	assert(as5047p_read_counts(&enc, NULL) == -EINVAL);
	assert(as5047p_read_status(&enc, NULL) == -EINVAL);
	as5047p_remove(&enc);
}

static void test_second_probe_is_refused(void)
{
	static const struct encoder_ops other_ops = { .read = other_read };
	struct encoder_dev other = { .name = "other", .ops = &other_ops };
	struct encoder_dev enc = open_ok(&base);
	void *priv = enc.priv;
	int a = added, f = frames, n = allocs;
	uint16_t c;

	assert(as5047p_probe(&enc, &base) == -EBUSY);
	assert(added == a && frames == f && allocs == n);
	assert(enc.priv == priv && as5047p_read_counts(&enc, &c) == 0 &&
	       "first binding intact");
	as5047p_remove(&enc);

	assert(as5047p_probe(&other, &base) == -EBUSY);
	assert(!strcmp(other.name, "other") && other.ops == &other_ops);

	chip_reset();
	assert(as5047p_probe(&enc, &base) == 0);
	as5047p_remove(&enc);
}

int main(void)
{
	test_probe();
	test_daec_register();
	test_wraps_and_zero();
	test_invert();
	test_failed_read_changes_nothing();
	test_error_flag_is_cleared();
	test_status();
	test_null_and_foreign();
	test_second_probe_is_refused();
	printf("all as5047p driver checks passed\n");
	return 0;
}
