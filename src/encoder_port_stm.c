/*
 * encoder_priv.h on the STM32 HAL. stm-forge compiles every .c in src/, so
 * under ESP-IDF this file is empty; the ESP-IDF CMakeLists never lists it.
 */
#ifndef ESP_PLATFORM

#include <string.h>

#include "encoder.h"
#include "encoder_priv.h"

/*
 * The core is always built; each chip driver only under its own option. With
 * the library on and no chip picked, say so -- stm-forge compiles this file
 * only when PKG_encoder = y, so this is the CMakeLists' "ENCODER on, no
 * chip" warning on the ESP-IDF side.
 */
#if !(CONFIG_ENCODER_AS5047P || CONFIG_ENCODER_AS5600 || \
      CONFIG_ENCODER_AMT102)
#warning "encoder: no chip driver is selected; set ENCODER_<CHIP> = y under PKG_encoder = y (or -DCONFIG_ENCODER_<CHIP>=1)"
#endif

/* Milliseconds dressed as microseconds; see encoder.h for a real clock. */
__attribute__((weak)) int64_t encoder_time_us(void)
{
	return (int64_t)HAL_GetTick() * 1000;
}

#if defined(HAL_I2C_MODULE_ENABLED) || defined(HAL_SPI_MODULE_ENABLED) || \
    defined(HAL_UART_MODULE_ENABLED)
/* Nothing to undo: the handle is the app's, and nothing was added to it. */
static void encoder_hal_detach(struct encoder_bus *bus)
{
	(void)bus;
}
#endif

#ifdef HAL_I2C_MODULE_ENABLED
/*
 * HAL_I2C_IsDeviceReady() returns HAL_ERROR when no trial was ACKed, which is
 * -ENXIO. A busy or stuck bus keeps its own errno. On the F4, no pull-ups or
 * SDA held low sets the peripheral's BUSY flag, so the HAL gives up after
 * 25 ms with HAL_BUSY, -EAGAIN; HAL_TIMEOUT, -ETIMEDOUT, means a START never
 * completed.
 */
int encoder_i2c_probe(encoder_i2c_bus handle, uint8_t address,
		      uint32_t timeout_ms)
{
	HAL_StatusTypeDef status;

	status = HAL_I2C_IsDeviceReady(handle, (uint16_t)(address << 1), 1,
				       timeout_ms);
	return status == HAL_ERROR ? -ENXIO : encoder_hal_errno(status);
}

/* HAL_I2C_Mem_Read(): cmd, repeated start, burst. Mem_Write(): cmd, data. */
static int encoder_i2c_transfer(struct encoder_bus *bus, uint8_t cmd,
				const uint8_t *tx, size_t tx_len, uint8_t *rx,
				size_t rx_len)
{
	uint16_t address = (uint16_t)(bus->i2c.address << 1);

	if ((tx_len && rx_len) || tx_len > ENCODER_BUS_MAX_LEN ||
	    rx_len > UINT16_MAX)
		return -EINVAL;
	if (rx_len)
		return encoder_hal_errno(HAL_I2C_Mem_Read(bus->i2c.handle,
							  address, cmd,
							  I2C_MEMADD_SIZE_8BIT,
							  rx, (uint16_t)rx_len,
							  bus->timeout_ms));
	if (tx_len)
		return encoder_hal_errno(HAL_I2C_Mem_Write(bus->i2c.handle,
							   address, cmd,
							   I2C_MEMADD_SIZE_8BIT,
							   (uint8_t *)tx,
							   (uint16_t)tx_len,
							   bus->timeout_ms));
	/* A bare command, for a part with no registers: its own START, STOP. */
	return encoder_hal_errno(HAL_I2C_Master_Transmit(bus->i2c.handle,
							 address, &cmd, 1,
							 bus->timeout_ms));
}

static const struct encoder_bus_ops encoder_i2c_ops = {
	.kind = ENCODER_BUS_I2C,
	.transfer = encoder_i2c_transfer,
	.detach = encoder_hal_detach,
};

int encoder_i2c_attach(struct encoder_bus *bus, encoder_i2c_bus handle,
		       uint8_t address, uint32_t scl_hz, uint32_t timeout_ms)
{
	(void)scl_hz;
	bus->ops = &encoder_i2c_ops;
	bus->timeout_ms = timeout_ms;
	bus->i2c.handle = handle;
	bus->i2c.address = address;
	return 0;
}
#endif /* HAL_I2C_MODULE_ENABLED */

#ifdef HAL_SPI_MODULE_ENABLED
/*
 * One transaction under a software chip select: CS low, cmd and tx out --
 * full duplex with zeros after them when there is something to read -- CS
 * high. CS goes high even when the transfer failed; see encoder_bus_ops.
 */
static int encoder_spi_transfer(struct encoder_bus *bus, uint8_t cmd,
				const uint8_t *tx, size_t tx_len, uint8_t *rx,
				size_t rx_len)
{
	uint8_t out[1 + ENCODER_BUS_MAX_LEN] = { 0 };
	uint8_t in[1 + ENCODER_BUS_MAX_LEN];
	uint16_t n = (uint16_t)(1 + tx_len + rx_len);
	HAL_StatusTypeDef status;

	if (tx_len + rx_len > ENCODER_BUS_MAX_LEN)
		return -EINVAL;
	out[0] = cmd;
	if (tx_len)
		memcpy(out + 1, tx, tx_len);

	HAL_GPIO_WritePin(bus->spi.cs_port, bus->spi.cs_pin, GPIO_PIN_RESET);
	if (rx_len)
		status = HAL_SPI_TransmitReceive(bus->spi.handle, out, in, n,
						 bus->timeout_ms);
	else
		status = HAL_SPI_Transmit(bus->spi.handle, out, n,
					  bus->timeout_ms);
	HAL_GPIO_WritePin(bus->spi.cs_port, bus->spi.cs_pin, GPIO_PIN_SET);
	if (status == HAL_OK && rx_len)
		memcpy(rx, in + 1 + tx_len, rx_len);
	return encoder_hal_errno(status);
}

/* One frame, full duplex: rx[i] is what came back while tx[i] went out. */
static int encoder_spi_exchange(struct encoder_bus *bus, const uint8_t *tx,
				uint8_t *rx, size_t len)
{
	HAL_StatusTypeDef status;

	if (!len || len > 1 + ENCODER_BUS_MAX_LEN)
		return -EINVAL;

	HAL_GPIO_WritePin(bus->spi.cs_port, bus->spi.cs_pin, GPIO_PIN_RESET);
	status = HAL_SPI_TransmitReceive(bus->spi.handle, tx, rx,
					 (uint16_t)len, bus->timeout_ms);
	HAL_GPIO_WritePin(bus->spi.cs_port, bus->spi.cs_pin, GPIO_PIN_SET);
	return encoder_hal_errno(status);
}

static const struct encoder_bus_ops encoder_spi_ops = {
	.kind = ENCODER_BUS_SPI,
	.transfer = encoder_spi_transfer,
	.exchange = encoder_spi_exchange,
	.detach = encoder_hal_detach,
};

int encoder_spi_attach(struct encoder_bus *bus, SPI_HandleTypeDef *handle,
		       GPIO_TypeDef *cs_port, uint16_t cs_pin,
		       uint32_t timeout_ms)
{
	bus->ops = &encoder_spi_ops;
	bus->timeout_ms = timeout_ms;
	bus->spi.handle = handle;
	bus->spi.cs_port = cs_port;
	bus->spi.cs_pin = cs_pin;
	/* Deselected first, so the first transfer starts on a falling edge. */
	HAL_GPIO_WritePin(cs_port, cs_pin, GPIO_PIN_SET);
	return 0;
}
#endif /* HAL_SPI_MODULE_ENABLED */

#ifdef HAL_UART_MODULE_ENABLED
/*
 * Blocking HAL_UART_Receive() hands back whatever already sits in the data
 * register, so a late byte from an earlier exchange would be read as the
 * start of this reply. Drop it first: clear an overrun, then read the data
 * register empty (on the F7 that macro is a flush request). Bounded by the
 * timeout, in case the line never goes quiet.
 */
static void encoder_uart_drop_input(struct encoder_bus *bus)
{
	UART_HandleTypeDef *handle = bus->uart;
	uint32_t start = HAL_GetTick();

	__HAL_UART_CLEAR_OREFLAG(handle);
	while (__HAL_UART_GET_FLAG(handle, UART_FLAG_RXNE) &&
	       (uint32_t)(HAL_GetTick() - start) <= bus->timeout_ms)
		__HAL_UART_FLUSH_DRREGISTER(handle);
}

static int encoder_uart_transfer(struct encoder_bus *bus, uint8_t cmd,
				 const uint8_t *tx, size_t tx_len,
				 uint8_t *rx, size_t rx_len)
{
	uint8_t out[1 + ENCODER_BUS_MAX_LEN];
	int ret;

	if (tx_len > ENCODER_BUS_MAX_LEN || rx_len > UINT16_MAX)
		return -EINVAL;
	out[0] = cmd;
	if (tx_len)
		memcpy(out + 1, tx, tx_len);

	encoder_uart_drop_input(bus);
	ret = encoder_hal_errno(HAL_UART_Transmit(bus->uart, out,
						  (uint16_t)(1 + tx_len),
						  bus->timeout_ms));
	if (ret || !rx_len)
		return ret;
	/* A reply that stops short runs into the timeout: HAL_TIMEOUT. */
	return encoder_hal_errno(HAL_UART_Receive(bus->uart, rx,
						  (uint16_t)rx_len,
						  bus->timeout_ms));
}

static const struct encoder_bus_ops encoder_uart_ops = {
	.kind = ENCODER_BUS_UART,
	.transfer = encoder_uart_transfer,
	.detach = encoder_hal_detach,
};

int encoder_uart_attach(struct encoder_bus *bus, UART_HandleTypeDef *handle,
			uint32_t timeout_ms)
{
	bus->ops = &encoder_uart_ops;
	bus->timeout_ms = timeout_ms;
	bus->uart = handle;
	return 0;
}
#endif /* HAL_UART_MODULE_ENABLED */

#endif /* !ESP_PLATFORM */
