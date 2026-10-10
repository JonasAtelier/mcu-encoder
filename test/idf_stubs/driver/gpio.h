#ifndef GPIO_H
#define GPIO_H

#include <stdint.h>

#include "esp_err.h"

/* Host stub for the ESP-IDF gpio driver: only what the drivers use. */

typedef enum {
	GPIO_NUM_NC = -1,
	GPIO_NUM_0 = 0,
	GPIO_NUM_5 = 5,
	GPIO_NUM_18 = 18,
	GPIO_NUM_19 = 19,
	GPIO_NUM_21 = 21,
	GPIO_NUM_22 = 22,
	GPIO_NUM_23 = 23,
} gpio_num_t;

typedef enum {
	GPIO_MODE_INPUT = 1,
} gpio_mode_t;

typedef enum {
	GPIO_INTR_DISABLE = 0,
	GPIO_INTR_POSEDGE = 1,
} gpio_int_type_t;

typedef struct {
	uint64_t pin_bit_mask;
	gpio_mode_t mode;
	int pull_up_en;
	int pull_down_en;
	gpio_int_type_t intr_type;
} gpio_config_t;

typedef void (*gpio_isr_t)(void *arg);

esp_err_t gpio_config(const gpio_config_t *cfg);
esp_err_t gpio_install_isr_service(int intr_alloc_flags);
esp_err_t gpio_isr_handler_add(gpio_num_t gpio_num, gpio_isr_t isr_handler,
			       void *args);
esp_err_t gpio_isr_handler_remove(gpio_num_t gpio_num);

#endif
