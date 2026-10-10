# ESP-IDF only. stm-forge never reads this file: it compiles every .c under
# src/, chip folders included, puts src/ on the include path of this
# library's sources and adds include/ -- which is why each port file is empty
# on the other platform. Here the chip files are listed one by one, and
# PRIV_INCLUDE_DIRS "src" lets a chip folder reach encoder_priv.h by plain
# name.
#
# The core is always built, even with CONFIG_ENCODER off; each chip driver
# only when its Kconfig option is on. Kconfig values are available here during
# the build phase, and only SRCS depends on them -- never REQUIRES, which IDF
# resolves before sdkconfig. That is also why ENCODER off still builds the
# core: an empty SRCS would need a second, differently-shaped register call,
# and an unreferenced core costs nothing once --gc-sections has run.
#
# REQUIRES driver, not esp_driver_pcnt: the split components do not exist on
# IDF 5.2, and driver still pulls them in on later versions.
set(srcs "src/encoder.c" "src/encoder_log.c" "src/encoder_port_esp.c")

if(CONFIG_ENCODER AND NOT (CONFIG_ENCODER_AS5047P OR CONFIG_ENCODER_AS5600 OR
                           CONFIG_ENCODER_AMT102))
    message(WARNING "encoder: CONFIG_ENCODER is on but no chip driver is "
                    "selected; set CONFIG_ENCODER_<CHIP>=y "
                    "(esp-forge: ENCODER_<CHIP> = y)")
endif()

if(CONFIG_ENCODER_AS5047P)
    list(APPEND srcs "src/as5047p/as5047p.c")
endif()
if(CONFIG_ENCODER_AS5600)
    list(APPEND srcs "src/as5600/as5600.c")
endif()
if(CONFIG_ENCODER_AMT102)
    list(APPEND srcs "src/amt102/amt102.c")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src"
                       REQUIRES driver
                       PRIV_REQUIRES esp_timer)
