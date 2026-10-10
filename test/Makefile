CC ?= cc
# gnu11, not c11: the encoder_* log macros use `, ##__VA_ARGS__`.
CFLAGS += -std=gnu11 -Wall -Wextra -Werror -pedantic
# src/ for the common private header, and every chip folder, because the
# tests include a chip's private headers by plain name.
CPPFLAGS += -I../include -I../src $(addprefix -I,$(wildcard ../src/*/))

# Two suites, one per platform: the same chip drivers through
# src/encoder_port_esp.c against idf_stubs/ (esp/), and through
# src/encoder_port_stm.c against hal_stubs/ (stm/). esp/test_<chip>.c are
# esp-encoder's own tests. The core's and the bus chips' cases written for
# this library live once, in *_cases.h, and both platforms run them against
# their own fakes: esp/test_*_shared and stm/test_*. The AMT102 is not a bus
# chip, so its two suites test its two counters: PCNT on ESP-IDF, a timer on
# the HAL. The maths tests touch no bus or platform API and run once.

# What ESP-IDF passes: ESP_PLATFORM, and every chip on.
ESP_CPPFLAGS := -Iidf_stubs -DESP_PLATFORM \
		-DCONFIG_ENCODER_AS5047P=1 -DCONFIG_ENCODER_AS5600=1 \
		-DCONFIG_ENCODER_AMT102=1
# What stm-forge passes: the family header the code includes, the HAL modules
# an app's hal_conf would enable, and every chip option on
# (ENCODER_<CHIP> = y).
STM_CPPFLAGS := -Ihal_stubs \
		-DCMSIS_device_header='"stm32_hal_stub.h"' \
		-DHAL_I2C_MODULE_ENABLED -DHAL_SPI_MODULE_ENABLED \
		-DHAL_UART_MODULE_ENABLED -DHAL_TIM_MODULE_ENABLED \
		-DCONFIG_ENCODER_AS5047P=1 -DCONFIG_ENCODER_AS5600=1 \
		-DCONFIG_ENCODER_AMT102=1 -DCONFIG_ENCODER_DEBUG=1

# The core and the stubs every binary of a suite links.
CORE     := ../src/encoder.c ../src/encoder_log.c
ESP_CORE := $(CORE) ../src/encoder_port_esp.c idf_stubs/idf_stubs.c
STM_CORE := $(CORE) ../src/encoder_port_stm.c hal_stubs/hal_stubs.c

MATH_TESTS := test_as5047p_math test_amt102_math
SUITE      := test_core test_as5047p test_as5600 test_amt102
SHARED     := test_core_shared test_as5047p_shared test_as5600_shared
# Plus one port test each, for the SPI exchange and the UART ops.
ESP_TESTS  := $(addprefix esp/,$(SUITE) $(SHARED) test_port_esp)
STM_TESTS  := $(addprefix stm/,$(SUITE) test_port_stm)
TESTS      := $(MATH_TESTS) $(ESP_TESTS) $(STM_TESTS)

.PHONY: all clean test

all: $(TESTS)

test_as5047p_math: test_as5047p_math.c
test_amt102_math: test_amt102_math.c

esp/test_core: esp/test_core.c $(ESP_CORE)
esp/test_as5047p: esp/test_as5047p.c ../src/as5047p/as5047p.c $(ESP_CORE)
esp/test_as5600: esp/test_as5600.c ../src/as5600/as5600.c $(ESP_CORE)
esp/test_amt102: esp/test_amt102.c ../src/amt102/amt102.c $(ESP_CORE)
esp/test_core_shared: esp/test_core_shared.c $(ESP_CORE)
esp/test_as5047p_shared: esp/test_as5047p_shared.c \
			 ../src/as5047p/as5047p.c $(ESP_CORE)
esp/test_as5600_shared: esp/test_as5600_shared.c ../src/as5600/as5600.c \
			$(ESP_CORE)
esp/test_port_esp: esp/test_port_esp.c $(ESP_CORE)

stm/test_core: stm/test_core.c $(STM_CORE)
stm/test_as5047p: stm/test_as5047p.c ../src/as5047p/as5047p.c $(STM_CORE)
stm/test_as5600: stm/test_as5600.c ../src/as5600/as5600.c $(STM_CORE)
stm/test_amt102: stm/test_amt102.c ../src/amt102/amt102.c $(STM_CORE)
stm/test_port_stm: stm/test_port_stm.c $(STM_CORE)

$(ESP_TESTS): CPPFLAGS += $(ESP_CPPFLAGS)
$(STM_TESTS): CPPFLAGS += $(STM_CPPFLAGS)

# Lets the driver tests count calloc() calls, to prove a refused probe
# allocates nothing -- and in this library's own tests free() too, that a
# failed one leaks nothing. GNU ld; only the drivers' own references are
# wrapped. esp-encoder's tests wrap calloc() alone, as they always did.
$(addprefix esp/,test_as5047p test_as5600 test_amt102): \
	LDFLAGS += -Wl,--wrap=calloc
$(addprefix esp/,test_as5047p_shared test_as5600_shared) \
$(addprefix stm/,test_as5047p test_as5600 test_amt102): \
	LDFLAGS += -Wl,--wrap=calloc -Wl,--wrap=free
# Every printf() in a binary that links the HAL stubs also lands in
# stub_log_last, so a test can check what was logged.
$(STM_TESTS): LDFLAGS += -Wl,--wrap=printf

$(TESTS):
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(filter %.c,$^) -o $@ -lm

test: $(TESTS)
	@set -e; for t in $(TESTS); do echo "./$$t"; ./$$t; done

clean:
	rm -f $(TESTS)

# Headers are prerequisites so that editing one rebuilds. Without this, `make
# test` reruns a stale binary and reports green for code it never compiled.
# Recipes use $(filter %.c,$^) so the headers never reach the compiler.
HDRS  := $(wildcard ../include/*.h ../src/*.h ../src/*/*.h)
STUBS := $(wildcard idf_stubs/*.h idf_stubs/*/*.h hal_stubs/*.h)
$(TESTS): $(HDRS) $(STUBS) $(wildcard *_cases.h)
