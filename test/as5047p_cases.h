/*
 * A fake AS5047P at the level of its 16-bit SPI frames, and the driver's
 * cases against it, shared by esp/test_as5047p.c and stm/test_as5047p.c.
 * Each platform's file feeds the fake from its own SPI calls and provides:
 *
 *   base_config()          a config the platform's probe accepts
 *   bus_fail(on)           every later SPI transfer fails as a timeout, or not
 *   allocs, frees          the driver's calloc() and free() calls
 *
 * The fake follows docs/AS5047P_v2-00.pdf: every frame carries even parity
 * in bit 15 (Figures 13, 14); a read answers in the *next* frame (Figure
 * 15), with EF set when the command before it was bad (Figure 14); the
 * register addresses are Figure 18's. It knows nothing of the driver.
 */

#include "rad.h"

static uint16_t fake_angle_com, fake_angle_unc, fake_diaagc, fake_mag;
static uint16_t fake_errfl;
static uint16_t fake_answer;	/* sent in the next frame */
static bool fake_answer_ef;
static uint16_t fake_last_addr;
static int fake_frames;
static int fake_stuck;		/* >= 0: MISO reads this, whatever was asked */
static bool fake_bad_parity;	/* replies go out with PARD wrong */
static int fake_reject;		/* the next n commands are refused */

static void fake_power_on(void)
{
	fake_angle_com = fake_angle_unc = 0;
	fake_diaagc = 0x0180;	/* the reset value, Figure 18 */
	fake_mag = 0;
	fake_errfl = 0x0001;	/* FRERR, as after a power-up glitch */
	fake_answer = 0;
	fake_answer_ef = false;
	fake_last_addr = 0xFFFF;
	fake_frames = 0;
	fake_stuck = -1;
	fake_bad_parity = false;
	fake_reject = 0;
}

static uint16_t fake_register(uint16_t addr)
{
	uint16_t value;

	switch (addr) {
	case AS5047P_NOP:
		return 0;
	case AS5047P_ERRFL:
		value = fake_errfl;
		fake_errfl = 0;		/* reading clears it, Figure 19 */
		return value;
	case AS5047P_DIAAGC:
		return fake_diaagc;
	case AS5047P_MAG:
		return fake_mag;
	case AS5047P_ANGLEUNC:
		return fake_angle_unc;
	case AS5047P_ANGLECOM:
		return fake_angle_com;
	default:
		assert(0 && "the driver reads only Figure 18's registers");
		return 0;
	}
}

/* One frame: the answer to the previous command out, this one taken in. */
static void as5047p_fake_frame(const uint8_t tx[2], uint8_t rx[2])
{
	uint16_t command = (uint16_t)(tx[0] << 8 | tx[1]);
	uint16_t reply;

	reply = (uint16_t)((fake_answer & AS5047P_FRAME_DATA) |
			   (fake_answer_ef ? AS5047P_FRAME_ERROR : 0));
	reply = as5047p_with_parity(reply);
	if (fake_bad_parity)
		reply ^= AS5047P_FRAME_PARITY;
	if (fake_stuck >= 0)
		reply = (uint16_t)fake_stuck;
	rx[0] = (uint8_t)(reply >> 8);
	rx[1] = (uint8_t)reply;
	fake_frames++;

	fake_answer = 0;
	fake_answer_ef = false;
	if (fake_reject > 0 || as5047p_with_parity(command) != command) {
		if (fake_reject > 0)
			fake_reject--;
		fake_answer_ef = true;
		fake_errfl |= 0x0004;	/* PARERR, Figure 19 */
		return;
	}
	assert((command & AS5047P_CMD_READ) && "the driver only reads");
	fake_last_addr = command & AS5047P_FRAME_DATA;
	fake_answer = fake_register(fake_last_addr);
}

static struct encoder_dev probe_ok(const struct as5047p_config *cfg)
{
	struct encoder_dev encoder = { 0 };

	assert(as5047p_probe(&encoder, cfg) == 0);
	assert(encoder.ops && encoder.priv);
	return encoder;
}

static void test_probe(void)
{
	struct as5047p_config cfg = base_config();
	struct encoder_dev encoder;

	fake_power_on();
	allocs = frees = 0;
	encoder = probe_ok(&cfg);
	assert(!strcmp(encoder.name, "as5047p"));
	assert(encoder.counts_per_turn == 16384 && encoder.absolute);
	/* ERRFL to clear the power-up flag, then DIAAGC: two frames each. */
	assert(fake_frames == 4 && fake_errfl == 0);
	assert(allocs == 1);
	as5047p_remove(&encoder);
	assert(frees == 1);
	assert(!encoder.ops && !encoder.priv && !encoder.counts_per_turn &&
	       !encoder.absolute);
	as5047p_remove(&encoder);		/* unprobed: a no-op */
	assert(frees == 1);
}

/* MISO stuck, or a garbled answer: refused, and nothing leaked. */
static void test_probe_no_chip(void)
{
	struct as5047p_config cfg = base_config();
	struct encoder_dev encoder = { 0 };

	/* Stuck low: all zeros has good parity and no EF, but is no chip. */
	fake_power_on();
	fake_stuck = 0x0000;
	allocs = frees = 0;
	assert(as5047p_probe(&encoder, &cfg) == -ENXIO);
	assert(allocs == 1 && frees == 1 && !encoder.ops && !encoder.priv);

	/*
	 * Stuck high: 0xFFFF has good parity and EF set, so decoded it would
	 * pass for an error frame. The probe looks at the raw frame first.
	 */
	fake_power_on();
	fake_stuck = 0xFFFF;
	assert(as5047p_probe(&encoder, &cfg) == -ENXIO);

	/* DIAAGC's whole data field set reads as stuck as well. */
	fake_power_on();
	fake_diaagc = AS5047P_FRAME_DATA;
	assert(as5047p_probe(&encoder, &cfg) == -ENXIO);

	/* An answer whose parity is wrong. */
	fake_power_on();
	fake_bad_parity = true;
	assert(as5047p_probe(&encoder, &cfg) == -EBADMSG);

	/* The chip refusing the command: EF in the answer, -EIO. */
	fake_power_on();
	fake_reject = 1000;
	assert(as5047p_probe(&encoder, &cfg) == -EIO);

	/* The bus itself failing. */
	fake_power_on();
	bus_fail(true);
	assert(as5047p_probe(&encoder, &cfg) == -ETIMEDOUT);
	bus_fail(false);
	assert(allocs == frees && !encoder.ops && !encoder.priv);
}

/* Angles, turns, direction and zero, through encoder_read(). */
static void test_read(void)
{
	struct as5047p_config cfg = base_config();
	struct encoder_dev encoder;
	struct encoder_sample s;
	uint16_t counts;

	fake_power_on();
	encoder = probe_ok(&cfg);

	fake_angle_com = 4096;
	assert(encoder_read(&encoder, &s) == 0);
	assert(fake_last_addr == AS5047P_NOP);	/* the address, then a NOP */
	assert(s.angle_rad == angle_of(4096, 16384));
	assert(NEAR(s.angle_rad, M_PI / 2) && NEAR(s.total_rad, M_PI / 2));

	/*
	 * Forward through the chip's zero, in steps under half a turn: one
	 * turn more. A step of more than half would count the other way.
	 */
	fake_angle_com = 10000;
	assert(encoder_read(&encoder, &s) == 0);
	fake_angle_com = 16000;
	assert(encoder_read(&encoder, &s) == 0);
	fake_angle_com = 100;
	assert(encoder_read(&encoder, &s) == 0);
	assert(s.total_rad == total_of(16384 + 100, 16384));

	/* Zero here: the next read is 0, and the turns start again. */
	assert(encoder_set_zero(&encoder) == 0);
	assert(encoder_read(&encoder, &s) == 0);
	assert(s.angle_rad == 0.0f && s.total_rad == 0.0);
	fake_angle_com = 100 - 50;
	assert(encoder_read(&encoder, &s) == 0);
	assert(s.angle_rad == angle_of(-50, 16384));
	assert(s.total_rad == total_of(-50, 16384));

	/* Raw counts: no zero, no turns. */
	fake_angle_com = 1234;
	assert(as5047p_read_counts(&encoder, &counts) == 0 && counts == 1234);

	/*
	 * A refused command: -EIO and nothing written. The driver reads ERRFL
	 * behind it, which clears the error, so the next read is clean.
	 */
	fake_reject = 1;
	counts = 7;
	fake_frames = 0;
	assert(as5047p_read_counts(&encoder, &counts) == -EIO);
	assert(counts == 7 && fake_frames == 4);
	assert(fake_last_addr == AS5047P_NOP);
	assert(as5047p_read_counts(&encoder, &counts) == 0 && counts == 1234);
	/* Bad parity is not EF: no ERRFL read behind it. */
	fake_bad_parity = true;
	fake_frames = 0;
	assert(as5047p_read_counts(&encoder, &counts) == -EBADMSG);
	assert(fake_frames == 2);
	fake_bad_parity = false;
	bus_fail(true);
	assert(encoder_read(&encoder, &s) == -ETIMEDOUT);
	bus_fail(false);
	as5047p_remove(&encoder);

	/* Inverted, and the uncompensated angle. */
	cfg.invert = true;
	cfg.raw_angle = true;
	fake_power_on();
	encoder = probe_ok(&cfg);
	/* Inverted is angle -> -angle: 0 stays 0, 1 forward is 16383. */
	fake_angle_unc = 0;
	fake_angle_com = 5;
	assert(as5047p_read_counts(&encoder, &counts) == 0);
	assert(counts == 0 && "inverted from ANGLEUNC, not ANGLECOM");
	fake_angle_unc = 1;
	assert(as5047p_read_counts(&encoder, &counts) == 0 && counts == 16383);
	as5047p_remove(&encoder);
}

static void test_status(void)
{
	struct as5047p_config cfg = base_config();
	struct encoder_dev encoder;
	struct as5047p_status st;

	fake_power_on();
	encoder = probe_ok(&cfg);
	/* MAGL, COF, LF, AGC 0x5A: Figure 21. */
	fake_diaagc = (1u << 11) | (1u << 9) | (1u << 8) | 0x5A;
	fake_mag = 0x1234;
	assert(as5047p_read_status(&encoder, &st) == 0);
	assert(st.magnet_too_weak && !st.magnet_too_strong);
	assert(st.cordic_overflow && st.offset_ready);
	assert(st.agc == 0x5A && st.magnitude == 0x1234);

	fake_diaagc = 1u << 10;			/* MAGH */
	assert(as5047p_read_status(&encoder, &st) == 0);
	assert(!st.magnet_too_weak && st.magnet_too_strong);
	assert(!st.cordic_overflow && !st.offset_ready && st.agc == 0);
	as5047p_remove(&encoder);
}

/* Bound already, someone else's, or NULL: refused, nothing on the bus. */
static void test_refusals(void)
{
	static const struct encoder_ops other_ops = { 0 };
	struct as5047p_config cfg = base_config();
	struct encoder_dev other = { .name = "other", .ops = &other_ops,
				     .priv = &cfg };
	struct encoder_dev encoder;
	struct as5047p_status st;
	uint16_t counts;
	void *priv;

	fake_power_on();
	encoder = probe_ok(&cfg);
	priv = encoder.priv;
	fake_frames = 0;
	allocs = 0;
	assert(as5047p_probe(&encoder, &cfg) == -EBUSY);
	assert(encoder.priv == priv && fake_frames == 0 && allocs == 0);

	assert(as5047p_read_counts(&other, &counts) == -EINVAL);
	assert(as5047p_read_status(&other, &st) == -EINVAL);
	as5047p_remove(&other);
	assert(other.ops == &other_ops && other.priv == &cfg);
	assert(as5047p_read_counts(&encoder, NULL) == -EINVAL);
	assert(as5047p_read_status(&encoder, NULL) == -EINVAL);
	assert(as5047p_read_counts(NULL, &counts) == -EINVAL);
	as5047p_remove(NULL);
	as5047p_remove(&encoder);

	assert(as5047p_probe(NULL, &cfg) == -EINVAL);
	assert(as5047p_probe(&encoder, NULL) == -EINVAL);
	assert(fake_frames == 0);
}

static void run_as5047p_cases(void)
{
	test_probe();
	test_probe_no_chip();
	test_read();
	test_status();
	test_refusals();
}
