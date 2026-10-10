/*
 * Kernel dev_err()-style logging for encoder drivers and their users.
 *
 * ESP-IDF: everything goes out under the one ESP-IDF tag "encoder", prefixed
 * with the device's name, so one esp_log_level_set("encoder", ...) controls
 * every chip and a line still says which chip it came from. There is
 * deliberately no level machinery of our own; encoder_dbg() is also subject
 * to CONFIG_LOG_MAXIMUM_LEVEL, and CONFIG_ENCODER_DEBUG is not used.
 *
 * STM32 HAL: every line goes out through printf() as "encoder: <name>: ...",
 * so a line still says which chip it came from. Where printf() ends up is
 * the app's business: with newlib, retarget _write() to a UART. There is
 * deliberately no level machinery; encoder_dbg() alone is compiled out
 * unless CONFIG_ENCODER_DEBUG is 1 (stm-forge: ENCODER_DEBUG = y).
 */
#ifndef ENCODER_LOG_H
#define ENCODER_LOG_H

#ifdef ESP_PLATFORM
#include "esp_log.h"
#else
#include <stdio.h>
#endif

#include "encoder.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef ESP_PLATFORM
#define ENCODER_LOG_TAG	"encoder"

#define encoder_err(encoder, fmt, ...) \
	ESP_LOGE(ENCODER_LOG_TAG, "%s: " fmt, (encoder)->name, ##__VA_ARGS__)
#define encoder_warn(encoder, fmt, ...) \
	ESP_LOGW(ENCODER_LOG_TAG, "%s: " fmt, (encoder)->name, ##__VA_ARGS__)
#define encoder_info(encoder, fmt, ...) \
	ESP_LOGI(ENCODER_LOG_TAG, "%s: " fmt, (encoder)->name, ##__VA_ARGS__)
#define encoder_dbg(encoder, fmt, ...) \
	ESP_LOGD(ENCODER_LOG_TAG, "%s: " fmt, (encoder)->name, ##__VA_ARGS__)

#else /* STM32 HAL */
#define encoder_err(encoder, fmt, ...) \
	printf("encoder: %s: " fmt "\n", (encoder)->name, ##__VA_ARGS__)
#define encoder_warn(encoder, fmt, ...) \
	printf("encoder: %s: " fmt "\n", (encoder)->name, ##__VA_ARGS__)
#define encoder_info(encoder, fmt, ...) \
	printf("encoder: %s: " fmt "\n", (encoder)->name, ##__VA_ARGS__)

#if CONFIG_ENCODER_DEBUG
#define encoder_dbg(encoder, fmt, ...) \
	printf("encoder: %s: " fmt "\n", (encoder)->name, ##__VA_ARGS__)
#else
/* Still type-checked, so a debug line cannot rot while it is off. */
#define encoder_dbg(encoder, fmt, ...) do { \
	if (0) \
		printf("encoder: %s: " fmt "\n", (encoder)->name, \
		       ##__VA_ARGS__); \
} while (0)
#endif
#endif /* ESP_PLATFORM */

/* One info line: timestamp, single-turn angle, total angle. */
void encoder_log_sample(const struct encoder_dev *encoder,
			const struct encoder_sample *sample);

#ifdef __cplusplus
}
#endif

#endif /* ENCODER_LOG_H */
