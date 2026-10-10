/*
 * Host-side check of the AMT102 driver against a fake PCNT unit and GPIO
 * driver. No hardware, no ESP toolchain -- see test/Makefile.
 *
 * The PCNT does the counting on the real part, so the fake just holds the
 * accumulated count the test sets. What is checked is everything around it:
 * that 4x quadrature decoding is configured edge for edge as the source
 * driver did, that the accumulator is on with watch points at both limits,
 * the index interrupt, counts to radians across 0 both ways, invert,
 * set_zero, and that a failed probe gives back every unit and channel.
 */
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "amt102.h"
#include "encoder.h"

#define PI	3.14159265358979323846

/* ---------------------------------------------------------- fake PCNT ---- */

static int count;
static int units_free, units_live, chans_live;
static bool enabled, started, accum;
static int high_limit, low_limit, watch[4], nwatch;
static uint32_t glitch_ns;
static pcnt_chan_config_t chan_cfg[2];
static pcnt_channel_edge_action_t edge[2][2];
static pcnt_channel_level_action_t level[2][2];
static int nchan;
static esp_err_t glitch_err, get_err;
static char unit_obj, chan_obj[2];

#define UNIT ((pcnt_unit_handle_t)(void *)&unit_obj)

static int chan_index(pcnt_channel_handle_t c)
{
	return (int)((char *)(void *)c - chan_obj);
}

esp_err_t pcnt_new_unit(const pcnt_unit_config_t *config,
			pcnt_unit_handle_t *ret_unit)
{
	if (units_free == 0)
		return ESP_ERR_NOT_FOUND;
	units_free--;
	units_live++;
	high_limit = config->high_limit;
	low_limit = config->low_limit;
	accum = config->flags.accum_count;
	*ret_unit = UNIT;
	return ESP_OK;
}

esp_err_t pcnt_del_unit(pcnt_unit_handle_t unit)
{
	assert(unit == UNIT && !enabled && chans_live == 0 &&
	       "a unit is deleted disabled, after its channels");
	units_live--;
	units_free++;
	return ESP_OK;
}

esp_err_t pcnt_unit_set_glitch_filter(pcnt_unit_handle_t unit,
				      const pcnt_glitch_filter_config_t *config)
{
	assert(unit == UNIT);
	if (glitch_err)
		return glitch_err;
	glitch_ns = config->max_glitch_ns;
	return ESP_OK;
}

esp_err_t pcnt_unit_enable(pcnt_unit_handle_t unit)
{
	(void)unit;
	enabled = true;
	return ESP_OK;
}

esp_err_t pcnt_unit_disable(pcnt_unit_handle_t unit)
{
	(void)unit;
	if (!enabled)
		return ESP_ERR_INVALID_STATE;
	enabled = false;
	return ESP_OK;
}

esp_err_t pcnt_unit_start(pcnt_unit_handle_t unit)
{
	(void)unit;
	assert(enabled);
	started = true;
	return ESP_OK;
}

esp_err_t pcnt_unit_stop(pcnt_unit_handle_t unit)
{
	(void)unit;
	if (!started)
		return ESP_ERR_INVALID_STATE;
	started = false;
	return ESP_OK;
}

esp_err_t pcnt_unit_clear_count(pcnt_unit_handle_t unit)
{
	(void)unit;
	count = 0;
	return ESP_OK;
}

esp_err_t pcnt_unit_get_count(pcnt_unit_handle_t unit, int *value)
{
	(void)unit;
	if (get_err)
		return get_err;
	*value = count;
	return ESP_OK;
}

esp_err_t pcnt_unit_add_watch_point(pcnt_unit_handle_t unit, int watch_point)
{
	(void)unit;
	watch[nwatch++] = watch_point;
	return ESP_OK;
}

esp_err_t pcnt_new_channel(pcnt_unit_handle_t unit,
			   const pcnt_chan_config_t *config,
			   pcnt_channel_handle_t *ret_chan)
{
	assert(unit == UNIT && nchan < 2);
	chan_cfg[nchan] = *config;
	*ret_chan = (pcnt_channel_handle_t)(void *)&chan_obj[nchan];
	nchan++;
	chans_live++;
	return ESP_OK;
}

esp_err_t pcnt_del_channel(pcnt_channel_handle_t chan)
{
	(void)chan;
	chans_live--;
	return ESP_OK;
}

esp_err_t pcnt_channel_set_edge_action(pcnt_channel_handle_t chan,
				       pcnt_channel_edge_action_t pos_act,
				       pcnt_channel_edge_action_t neg_act)
{
	edge[chan_index(chan)][0] = pos_act;
	edge[chan_index(chan)][1] = neg_act;
	return ESP_OK;
}

esp_err_t pcnt_channel_set_level_action(pcnt_channel_handle_t chan,
					pcnt_channel_level_action_t high_act,
					pcnt_channel_level_action_t low_act)
{
	level[chan_index(chan)][0] = high_act;
	level[chan_index(chan)][1] = low_act;
	return ESP_OK;
}

/* ---------------------------------------------------------- fake GPIO ---- */

static gpio_config_t io_cfg;
static int io_configs;
static bool isr_service;
static gpio_num_t isr_pin = GPIO_NUM_NC;
static gpio_isr_t isr_fn;
static void *isr_arg;

esp_err_t gpio_config(const gpio_config_t *cfg)
{
	io_cfg = *cfg;
	io_configs++;
	return ESP_OK;
}

esp_err_t gpio_install_isr_service(int flags)
{
	(void)flags;
	if (isr_service)
		return ESP_ERR_INVALID_STATE;	/* someone else installed it */
	isr_service = true;
	return ESP_OK;
}

esp_err_t gpio_isr_handler_add(gpio_num_t pin, gpio_isr_t fn, void *arg)
{
	assert(isr_service);
	isr_pin = pin;
	isr_fn = fn;
	isr_arg = arg;
	return ESP_OK;
}

esp_err_t gpio_isr_handler_remove(gpio_num_t pin)
{
	assert(pin == isr_pin);
	isr_pin = GPIO_NUM_NC;
	isr_fn = NULL;
	return ESP_OK;
}

static void foreign_isr(void *arg)
{
	(void)arg;
}

/* Counts the driver's calloc() calls; linked with -Wl,--wrap=calloc. */
static int allocs;
void *__real_calloc(size_t n, size_t size);
void *__wrap_calloc(size_t n, size_t size)
{
	allocs++;
	return __real_calloc(n, size);
}

static void hw_reset(void)
{
	count = 0;
	units_free = 4;
	units_live = chans_live = nchan = nwatch = 0;
	enabled = started = accum = false;
	glitch_ns = 0;
	glitch_err = get_err = ESP_OK;
	io_configs = 0;
	isr_service = false;
	isr_pin = GPIO_NUM_NC;
	isr_fn = NULL;
}

static struct amt102_config base(void)
{
	struct amt102_config c = {
		.a_gpio = GPIO_NUM_18,
		.b_gpio = GPIO_NUM_19,
		.z_gpio = GPIO_NUM_21,
		.ppr = 2048,
	};

	return c;
}

static struct encoder_dev open_ok(const struct amt102_config *c)
{
	struct encoder_dev enc = { 0 };

	hw_reset();
	assert(amt102_probe(&enc, c) == 0);
	assert(enc.ops && enc.priv);
	return enc;
}

static void expect(struct encoder_dev *enc, int64_t since_zero, int32_t cpr)
{
	struct encoder_sample s;
	int64_t turn = ((since_zero % cpr) + cpr) % cpr;

	assert(enc->counts_per_turn == cpr);
	assert(encoder_read(enc, &s) == 0);
	assert(fabs(s.total_rad - (double)since_zero * 2 * PI / cpr) < 1e-9);
	assert(fabs(s.angle_rad - (double)turn * 2 * PI / cpr) < 1e-5);
	assert(s.angle_rad >= 0.0f && s.angle_rad < (float)(2 * PI));
}

/* ------------------------------------------------------------- tests ----- */

static void test_probe(void)
{
	struct amt102_config c = base();
	struct encoder_dev enc = { 0 };

	hw_reset();
	assert(amt102_probe(&enc, &c) == 0);
	assert(!strcmp(enc.name, "amt102"));
	assert(enc.counts_per_turn == 8192 && !enc.absolute);

	/* The 16-bit counter extended by the driver's accumulator. */
	assert(high_limit == 16384 && low_limit == -16384 && accum);
	assert(nwatch == 2 && watch[0] == 16384 && watch[1] == -16384);
	assert(glitch_ns == 1000 && "0 selects 1000 ns");
	assert(enabled && started);

	/* 4x: each channel counts its own edges, the other's level sets the way. */
	assert(chan_cfg[0].edge_gpio_num == 18 && chan_cfg[0].level_gpio_num == 19);
	assert(chan_cfg[1].edge_gpio_num == 19 && chan_cfg[1].level_gpio_num == 18);
	assert(edge[0][0] == PCNT_CHANNEL_EDGE_ACTION_DECREASE &&
	       edge[0][1] == PCNT_CHANNEL_EDGE_ACTION_INCREASE);
	assert(edge[1][0] == PCNT_CHANNEL_EDGE_ACTION_INCREASE &&
	       edge[1][1] == PCNT_CHANNEL_EDGE_ACTION_DECREASE);
	assert(level[0][0] == PCNT_CHANNEL_LEVEL_ACTION_KEEP &&
	       level[0][1] == PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
	assert(level[1][0] == PCNT_CHANNEL_LEVEL_ACTION_KEEP &&
	       level[1][1] == PCNT_CHANNEL_LEVEL_ACTION_INVERSE);

	/* The index: an input, rising edge, our handler on X. */
	assert(io_cfg.pin_bit_mask == 1ULL << 21);
	assert(io_cfg.mode == GPIO_MODE_INPUT &&
	       io_cfg.intr_type == GPIO_INTR_POSEDGE);
	assert(isr_pin == GPIO_NUM_21 && isr_fn);

	amt102_remove(&enc);
	assert(!enc.ops && !enc.priv && "remove zeroes the binding");
	assert(enc.counts_per_turn == 0);
	assert(units_live == 0 && chans_live == 0 && !enabled && !started);
	assert(isr_pin == GPIO_NUM_NC && "handler removed");

	/* Somebody else already installed the ISR service: that is fine. */
	hw_reset();
	isr_service = true;
	c.glitch_ns = 5000;
	assert(amt102_probe(&enc, &c) == 0);
	assert(glitch_ns == 5000 && isr_fn);
	amt102_remove(&enc);
}

static void test_probe_refusals(void)
{
	struct amt102_config c = base();
	struct encoder_dev enc = { 0 };

	/* Pins that collide are refused before calloc and the peripheral. */
	hw_reset();
	allocs = 0;
	c.b_gpio = c.a_gpio;
	assert(amt102_probe(&enc, &c) == -EINVAL);
	assert(strstr(stub_log_last, "E encoder: amt102: A 18, B 18"));
	c = base();
	c.z_gpio = c.b_gpio;
	assert(amt102_probe(&enc, &c) == -EINVAL);
	assert(allocs == 0 && units_live == 0 && units_free == 4);
	assert(!enc.ops && !enc.priv);

	/* Every unit taken is -ENOSPC, not a missing device. */
	hw_reset();
	units_free = 0;
	c = base();
	assert(amt102_probe(&enc, &c) == -ENOSPC);
	assert(!enc.ops && !enc.priv);

	/*
	 * A failed probe never touches a handler it did not add: here somebody
	 * else owns X's interrupt, and PCNT setup fails before ours is added.
	 */
	hw_reset();
	units_free = 0;
	isr_service = true;
	isr_pin = c.z_gpio;
	isr_fn = foreign_isr;
	assert(amt102_probe(&enc, &c) == -ENOSPC);
	assert(isr_pin == c.z_gpio && isr_fn == foreign_isr && "still theirs");

	/* A filter the target cannot do: -EINVAL, and the unit is given back. */
	hw_reset();
	glitch_err = ESP_ERR_INVALID_ARG;
	assert(amt102_probe(&enc, &c) == -EINVAL);
	assert(units_live == 0 && chans_live == 0 && units_free == 4);
	assert(!enc.ops && !enc.priv);
}

static void test_counts_to_radians(void)
{
	struct amt102_config c = base();
	struct encoder_dev enc = open_ok(&c);

	/* The README's 2048 PPR table: 8192 counts a turn. */
	expect(&enc, 0, 8192);
	count = 2048;
	expect(&enc, 2048, 8192);	/* a quarter turn */
	count = 10240;
	expect(&enc, 10240, 8192);	/* a turn and a quarter */
	/* Backward across 0: the angle folds to the top, the total goes negative. */
	count = -1;
	expect(&enc, -1, 8192);
	count = -10240;
	expect(&enc, -10240, 8192);
	/* Past the 16-bit counter: the accumulator carries on. */
	count = 5 * 8192 + 7;
	expect(&enc, 5 * 8192 + 7, 8192);

	/* set_zero mid-turn restarts both. */
	count = 3000;
	assert(encoder_set_zero(&enc) == 0);
	expect(&enc, 0, 8192);
	count = 2990;
	expect(&enc, -10, 8192);
	count = 3000 + 8192;
	expect(&enc, 8192, 8192);
	amt102_remove(&enc);

	/* ppr is whatever the DIP switches say; 0 is 2048. */
	c.ppr = 100;
	enc = open_ok(&c);
	count = 400 + 100;
	expect(&enc, 500, 400);
	amt102_remove(&enc);
	c.ppr = 0;
	enc = open_ok(&c);
	count = 8192;
	expect(&enc, 8192, 8192);
	amt102_remove(&enc);
}

static void test_invert(void)
{
	struct amt102_config c = base();
	struct encoder_dev enc;

	c.invert = true;
	enc = open_ok(&c);
	count = 1;
	expect(&enc, -1, 8192);
	count = -2048;
	expect(&enc, 2048, 8192);
	assert(encoder_set_zero(&enc) == 0);
	count = -2048 - 8192;
	expect(&enc, 8192, 8192);
	count = 0;
	expect(&enc, -2048, 8192);
	amt102_remove(&enc);
}

static void test_index(void)
{
	struct amt102_config c = base();
	struct encoder_dev enc = open_ok(&c);
	bool hit = true;

	assert(amt102_index_hit(&enc, &hit) == 0 && !hit);
	isr_fn(isr_arg);		/* X rises */
	assert(amt102_index_hit(&enc, &hit) == 0 && hit);
	assert(amt102_index_hit(&enc, &hit) == 0 && !hit && "clears on read");
	/* Two edges before a read are one hit; the ISR and the reader both
	 * take the lock (the stub asserts it is initialised and balanced). */
	isr_fn(isr_arg);
	isr_fn(isr_arg);
	assert(amt102_index_hit(&enc, &hit) == 0 && hit);
	assert(amt102_index_hit(&enc, &hit) == 0 && !hit);
	amt102_remove(&enc);

	/* Unwired X: no GPIO touched, and the extra says so. */
	c.z_gpio = GPIO_NUM_NC;
	enc = open_ok(&c);
	assert(io_configs == 0 && !isr_service && !isr_fn);
	assert(amt102_index_hit(&enc, &hit) == -EOPNOTSUPP);
	amt102_remove(&enc);
}

static void test_failed_read_changes_nothing(void)
{
	struct amt102_config c = base();
	struct encoder_dev enc = open_ok(&c);
	struct encoder_sample s = { .angle_rad = 9.0f, .total_rad = 9.0,
				    .t_us = 9 };

	count = 100;
	get_err = ESP_ERR_INVALID_STATE;
	assert(encoder_read(&enc, &s) == -EIO);
	assert(encoder_set_zero(&enc) == -EIO);
	assert(s.angle_rad == 9.0f && s.total_rad == 9.0 && s.t_us == 9);
	get_err = ESP_OK;
	expect(&enc, 100, 8192);	/* and the failed set_zero did not move 0 */
	amt102_remove(&enc);
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
				     .priv = &count };
	struct amt102_config c = base();
	struct encoder_dev enc = { 0 };
	bool b;

	hw_reset();
	allocs = 0;
	assert(amt102_probe(NULL, &c) == -EINVAL);
	assert(amt102_probe(&enc, NULL) == -EINVAL);
	assert(allocs == 0 && units_live == 0);
	assert(amt102_index_hit(NULL, &b) == -EINVAL);
	amt102_remove(NULL);

	assert(amt102_index_hit(&enc, &b) == -EINVAL);
	amt102_remove(&enc);

	assert(amt102_index_hit(&other, &b) == -EINVAL);
	amt102_remove(&other);
	assert(other.ops == &other_ops && other.priv == &count &&
	       "not ours to remove");

	enc = open_ok(&c);
	assert(amt102_index_hit(&enc, NULL) == -EINVAL);
	amt102_remove(&enc);
}

static void test_second_probe_is_refused(void)
{
	static const struct encoder_ops other_ops = { .read = other_read };
	struct encoder_dev other = { .name = "other", .ops = &other_ops };
	struct amt102_config c = base();
	struct encoder_dev enc = open_ok(&c);
	void *priv = enc.priv;
	int n = allocs, u = units_live;
	bool hit;

	assert(amt102_probe(&enc, &c) == -EBUSY);
	assert(allocs == n && units_live == u && "nothing claimed");
	assert(enc.priv == priv && amt102_index_hit(&enc, &hit) == 0 &&
	       "first binding intact");
	amt102_remove(&enc);

	assert(amt102_probe(&other, &c) == -EBUSY);
	assert(!strcmp(other.name, "other") && other.ops == &other_ops);

	hw_reset();
	assert(amt102_probe(&enc, &c) == 0);
	amt102_remove(&enc);
}

int main(void)
{
	test_probe();
	test_probe_refusals();
	test_counts_to_radians();
	test_invert();
	test_index();
	test_failed_read_changes_nothing();
	test_null_and_foreign();
	test_second_probe_is_refused();
	printf("all amt102 driver checks passed\n");
	return 0;
}
