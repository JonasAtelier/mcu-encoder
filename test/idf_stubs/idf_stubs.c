/* The stub definitions every test binary links. */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/pulse_cnt.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"

char stub_log_last[256];
int64_t stub_time_us;
unsigned int stub_tick_hz = 100;	/* ESP-IDF's default CONFIG_FREERTOS_HZ */

int64_t esp_timer_get_time(void)
{
	return stub_time_us;
}

void stub_log(char level, const char *tag, const char *fmt, ...)
{
	va_list ap;
	int n;

	n = snprintf(stub_log_last, sizeof(stub_log_last), "%c %s: ", level,
		     tag);
	va_start(ap, fmt);
	vsnprintf(stub_log_last + n, sizeof(stub_log_last) - n, fmt, ap);
	va_end(ap);
	fprintf(stderr, "%s\n", stub_log_last);
}

/*
 * src/encoder_port_esp.c is in every binary and calls the bus ones, and the
 * AMT102's driver calls the GPIO and PCNT ones. A test with a fake chip
 * defines the ones it needs, which win over these weak ones; a test that
 * reaches one it did not define has a bug, so stop it there.
 */
__attribute__((weak)) esp_err_t
i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
			  const i2c_device_config_t *config,
			  i2c_master_dev_handle_t *out_handle)
{
	(void)bus;
	(void)config;
	(void)out_handle;
	abort();
}

__attribute__((weak)) esp_err_t
i2c_master_bus_rm_device(i2c_master_dev_handle_t device)
{
	(void)device;
	abort();
}

__attribute__((weak)) esp_err_t
i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address,
		 int xfer_timeout_ms)
{
	(void)bus;
	(void)address;
	(void)xfer_timeout_ms;
	abort();
}

__attribute__((weak)) esp_err_t
i2c_master_transmit(i2c_master_dev_handle_t device, const uint8_t *data,
		    size_t length, int timeout_ms)
{
	(void)device;
	(void)data;
	(void)length;
	(void)timeout_ms;
	abort();
}

__attribute__((weak)) esp_err_t
i2c_master_transmit_receive(i2c_master_dev_handle_t device,
			    const uint8_t *tx, size_t tx_length, uint8_t *rx,
			    size_t rx_length, int timeout_ms)
{
	(void)device;
	(void)tx;
	(void)tx_length;
	(void)rx;
	(void)rx_length;
	(void)timeout_ms;
	abort();
}

__attribute__((weak)) esp_err_t
spi_bus_add_device(spi_host_device_t host,
		   const spi_device_interface_config_t *config,
		   spi_device_handle_t *out_handle)
{
	(void)host;
	(void)config;
	(void)out_handle;
	abort();
}

__attribute__((weak)) esp_err_t
spi_bus_remove_device(spi_device_handle_t device)
{
	(void)device;
	abort();
}

__attribute__((weak)) esp_err_t
spi_device_polling_transmit(spi_device_handle_t device,
			    spi_transaction_t *transfer)
{
	(void)device;
	(void)transfer;
	abort();
}

__attribute__((weak)) bool
uart_is_driver_installed(uart_port_t uart_num)
{
	(void)uart_num;
	abort();
}

__attribute__((weak)) esp_err_t
uart_flush_input(uart_port_t uart_num)
{
	(void)uart_num;
	abort();
}

__attribute__((weak)) int
uart_write_bytes(uart_port_t uart_num, const void *src, size_t size)
{
	(void)uart_num;
	(void)src;
	(void)size;
	abort();
}

__attribute__((weak)) int
uart_read_bytes(uart_port_t uart_num, void *buf, uint32_t length,
		TickType_t ticks_to_wait)
{
	(void)uart_num;
	(void)buf;
	(void)length;
	(void)ticks_to_wait;
	abort();
}

__attribute__((weak)) esp_err_t
gpio_config(const gpio_config_t *config)
{
	(void)config;
	abort();
}

__attribute__((weak)) esp_err_t
gpio_install_isr_service(int intr_alloc_flags)
{
	(void)intr_alloc_flags;
	abort();
}

__attribute__((weak)) esp_err_t
gpio_isr_handler_add(gpio_num_t gpio, gpio_isr_t isr_handler,
		     void *args)
{
	(void)gpio;
	(void)isr_handler;
	(void)args;
	abort();
}

__attribute__((weak)) esp_err_t
gpio_isr_handler_remove(gpio_num_t gpio)
{
	(void)gpio;
	abort();
}

__attribute__((weak)) esp_err_t
pcnt_new_unit(const pcnt_unit_config_t *config,
	      pcnt_unit_handle_t *ret_unit)
{
	(void)config;
	(void)ret_unit;
	abort();
}

__attribute__((weak)) esp_err_t
pcnt_del_unit(pcnt_unit_handle_t unit)
{
	(void)unit;
	abort();
}

__attribute__((weak)) esp_err_t
pcnt_unit_set_glitch_filter(pcnt_unit_handle_t unit,
			    const pcnt_glitch_filter_config_t *config)
{
	(void)unit;
	(void)config;
	abort();
}

__attribute__((weak)) esp_err_t
pcnt_new_channel(pcnt_unit_handle_t unit, const pcnt_chan_config_t *config,
		 pcnt_channel_handle_t *ret_chan)
{
	(void)unit;
	(void)config;
	(void)ret_chan;
	abort();
}

__attribute__((weak)) esp_err_t
pcnt_del_channel(pcnt_channel_handle_t chan)
{
	(void)chan;
	abort();
}

__attribute__((weak)) esp_err_t
pcnt_channel_set_edge_action(pcnt_channel_handle_t chan,
			     pcnt_channel_edge_action_t pos_act,
			     pcnt_channel_edge_action_t neg_act)
{
	(void)chan;
	(void)pos_act;
	(void)neg_act;
	abort();
}

__attribute__((weak)) esp_err_t
pcnt_channel_set_level_action(pcnt_channel_handle_t chan,
			      pcnt_channel_level_action_t high_act,
			      pcnt_channel_level_action_t low_act)
{
	(void)chan;
	(void)high_act;
	(void)low_act;
	abort();
}

__attribute__((weak)) esp_err_t
pcnt_unit_add_watch_point(pcnt_unit_handle_t unit, int watch_point)
{
	(void)unit;
	(void)watch_point;
	abort();
}

__attribute__((weak)) esp_err_t
pcnt_unit_enable(pcnt_unit_handle_t unit)
{
	(void)unit;
	abort();
}

__attribute__((weak)) esp_err_t
pcnt_unit_disable(pcnt_unit_handle_t unit)
{
	(void)unit;
	abort();
}

__attribute__((weak)) esp_err_t
pcnt_unit_start(pcnt_unit_handle_t unit)
{
	(void)unit;
	abort();
}

__attribute__((weak)) esp_err_t
pcnt_unit_stop(pcnt_unit_handle_t unit)
{
	(void)unit;
	abort();
}

__attribute__((weak)) esp_err_t
pcnt_unit_clear_count(pcnt_unit_handle_t unit)
{
	(void)unit;
	abort();
}

__attribute__((weak)) esp_err_t
pcnt_unit_get_count(pcnt_unit_handle_t unit, int *value)
{
	(void)unit;
	(void)value;
	abort();
}
