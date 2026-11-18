/*
 * Shared by the chip drivers inside this library. Not public API.
 *
 * Two layers sit under the bus chips, the way regmap sits under a kernel
 * driver:
 *
 *   - The bus kind is chosen at run time. A chip holds a struct encoder_bus
 *     and reaches it only through bus->ops, so a driver can sit on I2C, SPI
 *     or a UART. Every category library offers all three kinds, used or not:
 *     the AS5600 is on I2C, the AS5047P on SPI.
 *   - The platform is chosen at link time. encoder_port_esp.c and
 *     encoder_port_stm.c each define the ops tables, the attach functions
 *     and the clock below for their platform; the other one compiles to
 *     nothing.
 *
 * The AMT102 is a pulse device, not a bus chip: it counts on the ESP32's
 * PCNT or an STM32 timer in encoder mode, and sits outside the bus layer.
 *
 * There is no receive(): every chip here answers a register read or a
 * frame, none streams, and none wants its command and its answer as two
 * transactions.
 *
 * ESP_PLATFORM, which ESP-IDF defines for every component, and the HAL's own
 * module switches are defined or not, never 0 or 1, so they are the one place
 * #ifdef is right; this library's CONFIG_ENCODER_* options are always tested
 * with #if.
 */
#ifndef ENCODER_PRIV_H
#define ENCODER_PRIV_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The most bytes after cmd: tx on I2C and UART, tx and rx together on SPI. */
#define ENCODER_BUS_MAX_LEN	31

struct encoder_bus;

enum encoder_bus_kind {
	ENCODER_BUS_I2C,
	ENCODER_BUS_SPI,
	ENCODER_BUS_UART,
};

struct encoder_bus_ops {
	enum encoder_bus_kind kind;
	/*
	 * One transaction: cmd, then tx_len bytes of tx, then rx_len bytes
	 * into rx. cmd is the register address, with any SPI read bit already
	 * set by the chip, and a command byte on a UART.
	 *
	 *   I2C:  rx_len ? cmd, repeated start, read : cmd and tx, one write;
	 *         a bare cmd is a one-byte write. Sending tx and then reading
	 *         is -EINVAL.
	 *   SPI:  CS low, cmd, tx, rx, CS high -- high again even when the
	 *         transfer failed.
	 *   UART: drop stale input, send cmd and tx, then wait for rx_len
	 *         bytes; fewer within the timeout is -ETIMEDOUT.
	 *
	 * On SPI more than ENCODER_BUS_MAX_LEN bytes after cmd is -EINVAL; on
	 * I2C and UART only tx is bounded so, and rx on the HAL by its 16-bit
	 * count. 0 or a negative errno.
	 */
	int (*transfer)(struct encoder_bus *bus, uint8_t cmd,
			const uint8_t *tx, size_t tx_len, uint8_t *rx,
			size_t rx_len);
	/*
	 * One SPI frame, full duplex: CS low, len bytes of tx out while len
	 * bytes come back into rx, byte for byte at the same clocks, CS high
	 * -- high again even when the transfer failed. For a part whose
	 * answer arrives during the frame that follows the command, so that
	 * transfer(), which keeps only what comes after cmd and tx, would lose
	 * its first byte. More than 1 + ENCODER_BUS_MAX_LEN bytes is -EINVAL.
	 * NULL on I2C and UART.
	 */
	int (*exchange)(struct encoder_bus *bus, const uint8_t *tx,
			uint8_t *rx, size_t len);
	/* Undo the attach. The bus itself is the app's and is left alone. */
	void (*detach)(struct encoder_bus *bus);
};

#ifdef ESP_PLATFORM
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_err.h"

/* The I2C argument of every I2C probe on this platform. */
typedef i2c_master_bus_handle_t encoder_i2c_bus;

/*
 * One device on one bus, as its driver holds it. ENCODER_HAVE_BUS says the
 * platform has at least one bus kind, so the struct exists.
 */
#define ENCODER_HAVE_BUS
struct encoder_bus {
	const struct encoder_bus_ops *ops;
	uint32_t timeout_ms;
	union {
		i2c_master_dev_handle_t i2c;
		spi_device_handle_t spi;
		uart_port_t uart;
	};
};

/*
 * esp_err_t -> negative errno, at the driver boundary and nowhere else, for
 * the bus port and the AMT102's PCNT alike. ESP_ERR_NOT_FOUND is -ENXIO
 * (nothing at that address), not -ENODEV (something answered but it is the
 * wrong chip). Only i2c_master_probe() reports a missing ACK that way; an
 * i2c_master transfer that is NACKed returns ESP_ERR_INVALID_STATE, which
 * lands on -EIO here. So a driver that wants -ENXIO for an absent chip asks
 * i2c_master_probe() first.
 */
static inline int encoder_errno(esp_err_t err)
{
	switch (err) {
	case ESP_OK:
		return 0;
	case ESP_ERR_INVALID_ARG:
		return -EINVAL;
	case ESP_ERR_TIMEOUT:
		return -ETIMEDOUT;
	case ESP_ERR_NOT_FOUND:
		return -ENXIO;
	case ESP_ERR_INVALID_SIZE:
		return -EMSGSIZE;
	case ESP_ERR_NO_MEM:
		return -ENOMEM;
	case ESP_ERR_NOT_SUPPORTED:
		return -EOPNOTSUPP;
	default:
		return -EIO;
	}
}

#else /* STM32 HAL */
/*
 * Undefined, the include below fails with only
 * `#include expects "FILENAME" or <FILENAME>`. Every library source that
 * needs the HAL includes this header before any chip header, so one check
 * here names the cause on every build route. The HAL module checks sit in
 * its #else: without the family header no module looks enabled, and each of
 * them would blame a module instead.
 */
#ifndef CMSIS_device_header
#error "encoder: CMSIS_device_header is not defined; define it as the STM32 family header, \"stm32f4xx.h\" or \"stm32f7xx.h\" (CMake: set(MCU_DEVICE_HEADER stm32f4xx.h))"
#else
#include CMSIS_device_header

#if !defined(HAL_I2C_MODULE_ENABLED) && CONFIG_ENCODER_AS5600
#error "the AS5600 driver needs the HAL I2C module: #define HAL_I2C_MODULE_ENABLED in the app's stm32<family>xx_hal_conf.h, with stm32<family>xx_hal_i2c.c in the build"
#endif
#if !defined(HAL_SPI_MODULE_ENABLED) && CONFIG_ENCODER_AS5047P
#error "the AS5047P driver needs the HAL SPI module: #define HAL_SPI_MODULE_ENABLED in the app's stm32<family>xx_hal_conf.h, with stm32<family>xx_hal_spi.c in the build"
#endif
#if !defined(HAL_TIM_MODULE_ENABLED) && CONFIG_ENCODER_AMT102
#error "the AMT102 driver needs the HAL TIM module: #define HAL_TIM_MODULE_ENABLED in the app's stm32<family>xx_hal_conf.h, with stm32<family>xx_hal_tim.c in the build"
#endif
#endif /* CMSIS_device_header */

#ifdef HAL_I2C_MODULE_ENABLED
typedef I2C_HandleTypeDef *encoder_i2c_bus;
#endif

#if defined(HAL_I2C_MODULE_ENABLED) || defined(HAL_SPI_MODULE_ENABLED) || \
    defined(HAL_UART_MODULE_ENABLED)
#define ENCODER_HAVE_BUS
struct encoder_bus {
	const struct encoder_bus_ops *ops;
	uint32_t timeout_ms;
	union {
#ifdef HAL_I2C_MODULE_ENABLED
		struct {
			I2C_HandleTypeDef *handle;
			uint8_t address;	/* 7-bit */
		} i2c;
#endif
#ifdef HAL_SPI_MODULE_ENABLED
		struct {
			SPI_HandleTypeDef *handle;
			GPIO_TypeDef *cs_port;	/* app's output, idle high */
			uint16_t cs_pin;
		} spi;
#endif
#ifdef HAL_UART_MODULE_ENABLED
		UART_HandleTypeDef *uart;
#endif
	};
};
#endif

/*
 * HAL_StatusTypeDef -> negative errno, at the driver boundary and nowhere
 * else, for the bus port and the AMT102's timer alike. HAL_BUSY -- the bus
 * or the handle in use, or a handle never initialised -- is -EAGAIN,
 * because -EBUSY from a probe already means "this encoder_dev is bound". A
 * NACK in the middle of a transfer is HAL_ERROR like any other fault, so
 * -EIO.
 */
static inline int encoder_hal_errno(HAL_StatusTypeDef status)
{
	switch (status) {
	case HAL_OK:
		return 0;
	case HAL_TIMEOUT:
		return -ETIMEDOUT;
	case HAL_BUSY:
		return -EAGAIN;
	default:
		return -EIO;
	}
}

#endif /* STM32 HAL */

#ifdef ENCODER_HAVE_BUS
/* len bytes from register reg, as the chip sends them. */
static inline int encoder_reg_read(struct encoder_bus *bus, uint8_t reg,
				   void *buf, size_t len)
{
	return bus->ops->transfer(bus, reg, NULL, 0, buf, len);
}

/* bus->ops->exchange(), or -EOPNOTSUPP on a kind that has none. */
static inline int encoder_bus_exchange(struct encoder_bus *bus,
				       const uint8_t *tx, uint8_t *rx,
				       size_t len)
{
	if (!bus->ops->exchange)
		return -EOPNOTSUPP;
	return bus->ops->exchange(bus, tx, rx, len);
}

static inline void encoder_bus_detach(struct encoder_bus *bus)
{
	bus->ops->detach(bus);
}
#endif

#if defined(ESP_PLATFORM) || defined(HAL_I2C_MODULE_ENABLED)
/*
 * Is anything at this 7-bit address? -ENXIO when nothing ACKs -- nothing at
 * that address -- not -ENODEV, which means something answered but it is the
 * wrong chip. A NACK in the middle of a transfer is a plain -EIO on both
 * platforms, which is why every probe decides presence here, never by its
 * first transfer.
 */
int encoder_i2c_probe(encoder_i2c_bus handle, uint8_t address,
		      uint32_t timeout_ms);

/*
 * Bind bus to the I2C device at address. On ESP-IDF the driver adds the
 * device at scl_hz, 0 for 400 kHz; on the HAL the clock is the handle's and
 * scl_hz is ignored. Undo with encoder_bus_detach(). Every attach leaves
 * bus->ops NULL when it fails, so a bus with ops is one with something to
 * detach.
 */
int encoder_i2c_attach(struct encoder_bus *bus, encoder_i2c_bus handle,
		       uint8_t address, uint32_t scl_hz, uint32_t timeout_ms);
#endif

#ifdef ESP_PLATFORM
/*
 * Add the SPI device on host's cs_pin, in SPI mode 0-3 at clock_hz. The
 * host is the app's, already spi_bus_initialize()d.
 */
int encoder_spi_attach(struct encoder_bus *bus, spi_host_device_t host,
		       int cs_pin, uint32_t clock_hz, uint8_t mode);

/* port's driver is the app's, already installed and configured. */
int encoder_uart_attach(struct encoder_bus *bus, uart_port_t port,
			uint32_t timeout_ms);
#else
#ifdef HAL_SPI_MODULE_ENABLED
/*
 * Bind bus to the SPI device behind cs_port/cs_pin, and deselect it, so the
 * first transfer starts on a falling edge. Mode, clock and frame are the
 * handle's; the chip select is an output the app has set up.
 */
int encoder_spi_attach(struct encoder_bus *bus, SPI_HandleTypeDef *handle,
		       GPIO_TypeDef *cs_port, uint16_t cs_pin,
		       uint32_t timeout_ms);
#endif
#ifdef HAL_UART_MODULE_ENABLED
/* handle is the app's, already HAL_UART_Init()ed. */
int encoder_uart_attach(struct encoder_bus *bus, UART_HandleTypeDef *handle,
			uint32_t timeout_ms);
#endif
#endif

/*
 * Revolutions counted in software from single-turn readings, for the
 * absolute chips. A jump of more than half a turn between two readings can
 * only be a wrap, so long as the caller samples faster than the shaft turns.
 */
struct encoder_turns {
	int32_t last;		/* previous reading, for wrap detection */
	int32_t revolutions;	/* completed turns since the last zero */
	bool have_last;
};

static inline void encoder_track_turns(struct encoder_turns *turns,
				       int32_t counts, int32_t counts_per_turn)
{
	if (turns->have_last) {
		int32_t delta = counts - turns->last;

		if (delta > counts_per_turn / 2)
			turns->revolutions--;
		else if (delta < -(counts_per_turn / 2))
			turns->revolutions++;
	}

	turns->last = counts;
	turns->have_last = true;
}

/*
 * Software direction for an absolute count: angle -> -angle, so the chip's
 * 0 stays 0 and one count forward becomes counts_per_turn - 1.
 */
static inline int32_t encoder_invert(int32_t counts, int32_t counts_per_turn)
{
	return (counts_per_turn - counts) % counts_per_turn;
}

#endif /* ENCODER_PRIV_H */
