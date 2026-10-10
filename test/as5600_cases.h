/*
 * A fake AS5600 register file, and the driver's cases against it, shared by
 * esp/test_as5600.c and stm/test_as5600.c. Each platform's file feeds the
 * fake from its own I2C calls and provides:
 *
 *   BUS                    the platform's bus handle for the fake
 *   bus_fail(on)           every later transfer fails as a timeout, or not
 *   absent                 nothing ACKs at 0x36
 *   probed                 how many presence checks the probe made
 *   allocs, frees          the driver's calloc() and free() calls
 *
 * The register map is docs/AS5600_DS000365_5_00.pdf Figure 21: 12-bit
 * values big-endian in two registers, the pointer auto-incrementing.
 */

#include "rad.h"

static uint8_t regs[256];
static int writes, reads;

static void chip_power_on(void)
{
	memset(regs, 0, sizeof(regs));
	writes = reads = 0;
}

/* The pointer at reg, then len bytes out, auto-incrementing. */
static void as5600_fake_read(uint8_t reg, uint8_t *buf, size_t len)
{
	for (size_t i = 0; i < len; i++)
		buf[i] = regs[(uint8_t)(reg + i)];
	reads++;
}

static void as5600_fake_write(uint8_t reg, const uint8_t *buf, size_t len)
{
	for (size_t i = 0; i < len; i++)
		regs[(uint8_t)(reg + i)] = buf[i];
	writes++;
}

static void set_angle(uint16_t counts)
{
	regs[AS5600_ANGLE_H] = (uint8_t)(counts >> 8);
	regs[AS5600_ANGLE_H + 1] = (uint8_t)counts;
}

static struct encoder_dev probe_ok(const struct as5600_config *cfg)
{
	struct encoder_dev encoder = { 0 };

	assert(as5600_probe(&encoder, BUS, cfg) == 0);
	assert(encoder.ops && encoder.priv);
	return encoder;
}

/*
 * Probe: presence, one angle read, then CONF read, modified and written
 * back -- only SF and HYST, because blank CONF fields may hold factory
 * settings (Figure 21, note 1).
 */
static void test_probe(void)
{
	struct as5600_config cfg = { .filter = AS5600_FILTER_2X,
				     .hysteresis = AS5600_HYST_2LSB };
	struct encoder_dev encoder;

	chip_power_on();
	regs[AS5600_CONF_H] = 0x3C;	/* WD, FTH set; SF 00 */
	regs[AS5600_CONF_L] = 0xF3;	/* PWMF, OUTS, PM set; HYST 00 */
	allocs = frees = 0;
	probed = 0;
	encoder = probe_ok(&cfg);
	assert(probed == 1 && allocs == 1);
	assert(!strcmp(encoder.name, "as5600"));
	assert(encoder.counts_per_turn == 4096 && encoder.absolute);
	assert(writes == 1);
	/* SF 11 (2x) and HYST 10 (2 LSB), Figure 22; nothing else moved. */
	assert(regs[AS5600_CONF_H] == 0x3F);
	assert(regs[AS5600_CONF_L] == 0xFB);
	as5600_remove(&encoder);
	assert(frees == 1 && !encoder.ops && !encoder.priv &&
	       !encoder.counts_per_turn && !encoder.absolute);

	/* NULL cfg: the defaults, 16x and no hysteresis, written as zeros. */
	chip_power_on();
	regs[AS5600_CONF_H] = 0x3F;
	regs[AS5600_CONF_L] = 0xFF;
	encoder = probe_ok(NULL);
	assert(regs[AS5600_CONF_H] == 0x3C && regs[AS5600_CONF_L] == 0xF3);
	as5600_remove(&encoder);
}

static void test_probe_refused(void)
{
	struct encoder_dev encoder = { 0 };

	/* Nothing at 0x36: -ENXIO, before anything is allocated. */
	chip_power_on();
	absent = true;
	allocs = frees = 0;
	assert(as5600_probe(&encoder, BUS, NULL) == -ENXIO);
	assert(allocs == 0 && reads == 0 && !encoder.ops && !encoder.priv);
	absent = false;

	/* Present, then the bus fails: mapped, and nothing leaked. */
	bus_fail(true);
	assert(as5600_probe(&encoder, BUS, NULL) == -ETIMEDOUT);
	bus_fail(false);
	assert(allocs == 1 && frees == 1 && !encoder.ops && !encoder.priv);

	assert(as5600_probe(NULL, BUS, NULL) == -EINVAL);
	assert(as5600_probe(&encoder, NULL, NULL) == -EINVAL);

	/* A filter or hysteresis outside its enum: refused before the bus. */
	{
		struct as5600_config bad = { .filter = 4 };

		probed = allocs = 0;
		assert(as5600_probe(&encoder, BUS, &bad) == -EINVAL);
		bad.filter = AS5600_FILTER_2X;
		bad.hysteresis = 4;
		assert(as5600_probe(&encoder, BUS, &bad) == -EINVAL);
		assert(probed == 0 && allocs == 0 && !encoder.ops);
	}
}

/* Angles, turns, direction and zero, through encoder_read(). */
static void test_read(void)
{
	struct as5600_config cfg = { 0 };
	struct encoder_dev encoder;
	struct encoder_sample s;
	uint16_t counts;

	chip_power_on();
	encoder = probe_ok(&cfg);

	set_angle(1024);
	assert(encoder_read(&encoder, &s) == 0);
	assert(s.angle_rad == angle_of(1024, 4096));
	assert(NEAR(s.angle_rad, M_PI / 2) && NEAR(s.total_rad, M_PI / 2));
	/* Bits 15:12 of the pair are not the angle's. */
	regs[AS5600_ANGLE_H] |= 0xF0;
	assert(encoder_read(&encoder, &s) == 0);
	assert(s.angle_rad == angle_of(1024, 4096));

	/* Forward through zero, in steps under half a turn. */
	set_angle(3000);
	assert(encoder_read(&encoder, &s) == 0);
	set_angle(4000);
	assert(encoder_read(&encoder, &s) == 0);
	set_angle(96);
	assert(encoder_read(&encoder, &s) == 0);
	assert(s.total_rad == total_of(4096 + 96, 4096));

	/* Zero here; then a little backwards is negative. */
	assert(encoder_set_zero(&encoder) == 0);
	set_angle(4096 + 96 - 1024);	/* 1024 back, through zero */
	assert(encoder_read(&encoder, &s) == 0);
	assert(s.total_rad == total_of(-1024, 4096));
	assert(NEAR(s.angle_rad, 1.5 * M_PI));

	/* Raw counts come from RAW ANGLE, not ANGLE. */
	regs[AS5600_RAW_ANGLE_H] = 0x0A;
	regs[AS5600_RAW_ANGLE_H + 1] = 0xBC;
	assert(as5600_read_counts(&encoder, &counts) == 0 && counts == 0xABC);

	bus_fail(true);
	counts = 7;
	assert(as5600_read_counts(&encoder, &counts) == -ETIMEDOUT);
	assert(counts == 7);
	assert(encoder_read(&encoder, &s) == -ETIMEDOUT);
	bus_fail(false);
	as5600_remove(&encoder);

	cfg.invert = true;
	chip_power_on();
	encoder = probe_ok(&cfg);
	regs[AS5600_RAW_ANGLE_H] = 0;
	regs[AS5600_RAW_ANGLE_H + 1] = 0;
	/* Inverted is angle -> -angle: 0 stays 0, 1 forward is 4095. */
	assert(as5600_read_counts(&encoder, &counts) == 0 && counts == 0);
	regs[AS5600_RAW_ANGLE_H + 1] = 1;
	assert(as5600_read_counts(&encoder, &counts) == 0 && counts == 4095);
	as5600_remove(&encoder);
}

static void test_status(void)
{
	struct encoder_dev encoder;
	struct as5600_status st;

	chip_power_on();
	encoder = probe_ok(NULL);
	/* MD and ML, Figure 23; AGC; MAGNITUDE(11:0) with junk above. */
	regs[AS5600_STATUS] = (1u << 5) | (1u << 4);
	regs[AS5600_AGC] = 0x80;
	regs[AS5600_MAGNITUDE_H] = 0xF5;
	regs[AS5600_MAGNITUDE_H + 1] = 0x67;
	assert(as5600_read_status(&encoder, &st) == 0);
	assert(st.magnet_detected && st.magnet_too_weak &&
	       !st.magnet_too_strong);
	assert(st.agc == 0x80 && st.magnitude == 0x567);

	regs[AS5600_STATUS] = 1u << 3;		/* MH alone */
	assert(as5600_read_status(&encoder, &st) == 0);
	assert(!st.magnet_detected && !st.magnet_too_weak &&
	       st.magnet_too_strong);
	as5600_remove(&encoder);
}

static void test_refusals(void)
{
	static const struct encoder_ops other_ops = { 0 };
	struct encoder_dev other = { .name = "other", .ops = &other_ops,
				     .priv = regs };
	struct encoder_dev encoder;
	struct as5600_status st;
	uint16_t counts;
	void *priv;

	chip_power_on();
	encoder = probe_ok(NULL);
	priv = encoder.priv;
	reads = writes = allocs = probed = 0;
	assert(as5600_probe(&encoder, BUS, NULL) == -EBUSY);
	assert(encoder.priv == priv && probed == 0 && reads == 0 &&
	       allocs == 0);

	assert(as5600_read_counts(&other, &counts) == -EINVAL);
	assert(as5600_read_status(&other, &st) == -EINVAL);
	as5600_remove(&other);
	assert(other.ops == &other_ops && other.priv == regs);
	assert(as5600_read_counts(&encoder, NULL) == -EINVAL);
	assert(as5600_read_status(&encoder, NULL) == -EINVAL);
	as5600_remove(NULL);
	as5600_remove(&encoder);
	as5600_remove(&encoder);		/* unprobed now: a no-op */
}

static void run_as5600_cases(void)
{
	test_probe();
	test_probe_refused();
	test_read();
	test_status();
	test_refusals();
}
