# Firmware do nó LoRaWAN para o módulo Radioenge (STM32L071CZ + SX1272)
# com a LoRaMac-node v4.7.1: um firmware só para ABP e OTAA (AT+NJM em tempo de execução),
# com relés, medidores PZEM-004T v3 e o protocolo de aplicação ADAILSILVA_V1.
#
#   cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-arm-none-eabi.cmake
#   cmake --build build
#
# ou simplesmente: scripts/build.sh
cmake_minimum_required(VERSION 3.13)

set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain-arm-none-eabi.cmake CACHE FILEPATH "")
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

project(radioenge_node C ASM)

find_program(ARM_SIZE arm-none-eabi-size HINTS ${TOOLCHAIN_BIN_DIR})
find_program(ARM_OBJCOPY arm-none-eabi-objcopy HINTS ${TOOLCHAIN_BIN_DIR})

# ------------------------------------------------------------------ opções
set(CONSOLE_UART 1 CACHE STRING "Console: 1 = USART1 (pinos 2/3 do módulo), 2 = USART2 (GPIO1/GPIO0)")
set(CONSOLE_BAUDRATE 115200 CACHE STRING "Velocidade do console")
option(USE_DEBUGGER "Mantém o SWD ativo em SLEEP/STOP (gasta mais)" OFF)
option(RTC_FORCE_LSI "Teste: ignora o cristal de 32,768 kHz e usa o LSI calibrado" OFF)
# Versão do firmware: vazio = a do app/node_build.h. O GitHub Actions passa a da tag
# (v0.0.2 -> FW_VERSION=0.0.2), como nos projetos ESP32.
set(FW_VERSION "$ENV{FW_VERSION}" CACHE STRING "Versão gravada no firmware (vazio = app/node_build.h)")

# Um firmware só: ABP e OTAA vêm juntos e a ativação é escolhida em tempo de execução
# (AT+NJM / "set njm"); a de fábrica fica em app/node_build.h (NODE_ACTIVATION).
set(FW_NAME radioenge_node_abp_otaa)

set(LMN ${CMAKE_CURRENT_SOURCE_DIR}/lib/LoRaMac-node/src)
set(BRD ${CMAKE_CURRENT_SOURCE_DIR}/board/RadioengeL071)
set(CUBE ${CMAKE_CURRENT_SOURCE_DIR}/lib/STM32CubeL0)
set(HAL ${CUBE}/STM32L0xx_HAL_Driver)

# ------------------------------------------------------------------ fontes
set(SOURCES
    # aplicação
    app/main.c
    app/console.c
    app/node_config.c
    app/node_log.c
    app/node_diag.c
    app/node_io.c
    app/at_cmd.c
    app/node_relay.c
    app/node_pzem.c
    app/node_v1.c

    # placa
    ${BRD}/adc-board.c
    ${BRD}/board.c
    ${BRD}/delay-board.c
    ${BRD}/eeprom-board.c
    ${BRD}/gpio-board.c
    ${BRD}/lpm-board.c
    ${BRD}/rtc-board.c
    ${BRD}/spi-board.c
    ${BRD}/sx1272-board.c
    ${BRD}/sysIrqHandlers.c
    ${BRD}/uart-board.c
    ${BRD}/cmsis/system_stm32l0xx.c
    ${BRD}/cmsis/startup_stm32l071xx.s
    ${LMN}/boards/mcu/utilities.c

    # HAL STM32L0
    ${HAL}/Src/stm32l0xx_hal.c
    ${HAL}/Src/stm32l0xx_hal_adc.c
    ${HAL}/Src/stm32l0xx_hal_adc_ex.c
    ${HAL}/Src/stm32l0xx_hal_cortex.c
    ${HAL}/Src/stm32l0xx_hal_dma.c
    ${HAL}/Src/stm32l0xx_hal_flash.c
    ${HAL}/Src/stm32l0xx_hal_flash_ex.c
    ${HAL}/Src/stm32l0xx_hal_gpio.c
    ${HAL}/Src/stm32l0xx_hal_pwr.c
    ${HAL}/Src/stm32l0xx_hal_pwr_ex.c
    ${HAL}/Src/stm32l0xx_hal_rcc.c
    ${HAL}/Src/stm32l0xx_hal_rcc_ex.c
    ${HAL}/Src/stm32l0xx_hal_rtc.c
    ${HAL}/Src/stm32l0xx_hal_rtc_ex.c
    ${HAL}/Src/stm32l0xx_hal_spi.c

    # sistema
    ${LMN}/system/adc.c
    ${LMN}/system/delay.c
    ${LMN}/system/fifo.c
    ${LMN}/system/gpio.c
    ${LMN}/system/nvmm.c
    ${LMN}/system/systime.c
    ${LMN}/system/timer.c
    ${LMN}/system/uart.c

    # rádio
    ${LMN}/radio/sx1272/sx1272.c

    # MAC (só AU915; o RegionBaseUS.c é comum a AU915 e US915)
    ${LMN}/mac/LoRaMac.c
    ${LMN}/mac/LoRaMacAdr.c
    ${LMN}/mac/LoRaMacClassB.c
    ${LMN}/mac/LoRaMacCommands.c
    ${LMN}/mac/LoRaMacConfirmQueue.c
    ${LMN}/mac/LoRaMacCrypto.c
    ${LMN}/mac/LoRaMacParser.c
    ${LMN}/mac/LoRaMacSerializer.c
    ${LMN}/mac/region/Region.c
    ${LMN}/mac/region/RegionCommon.c
    ${LMN}/mac/region/RegionBaseUS.c
    ${LMN}/mac/region/RegionAU915.c

    # elemento seguro em software
    ${LMN}/peripherals/soft-se/aes.c
    ${LMN}/peripherals/soft-se/cmac.c
    ${LMN}/peripherals/soft-se/soft-se.c
    ${LMN}/peripherals/soft-se/soft-se-hal.c

    # LmHandler
    ${LMN}/apps/LoRaMac/common/LmHandler/LmHandler.c
    ${LMN}/apps/LoRaMac/common/LmHandler/packages/LmhpCompliance.c
    ${LMN}/apps/LoRaMac/common/LmHandler/packages/LmhpClockSync.c
    ${LMN}/apps/LoRaMac/common/LmHandler/packages/LmhpRemoteMcastSetup.c
    ${LMN}/apps/LoRaMac/common/LmHandler/packages/LmhpFragmentation.c
    ${LMN}/apps/LoRaMac/common/LmHandler/packages/FragDecoder.c
    ${LMN}/apps/LoRaMac/common/NvmDataMgmt.c
)

add_executable(${FW_NAME}.elf ${SOURCES})

# avisos de código de terceiros (LoRaMac-node e HAL da ST) não interessam aqui
file(GLOB_RECURSE _third_party ${LMN}/*.c ${HAL}/Src/*.c)
set_source_files_properties(${_third_party} PROPERTIES COMPILE_OPTIONS "-Wno-maybe-uninitialized;-Wno-unused-variable;-Wno-unused-but-set-variable")

target_include_directories(${FW_NAME}.elf PRIVATE
    app
    ${BRD}
    ${BRD}/cmsis
    ${LMN}/boards
    ${HAL}/Inc
    ${CUBE}/CMSIS/Device/ST/STM32L0xx/Include
    ${CUBE}/CMSIS/Include
    ${LMN}/system
    ${LMN}/radio
    ${LMN}/radio/sx1272
    ${LMN}/mac
    ${LMN}/mac/region
    ${LMN}/peripherals/soft-se
    ${LMN}/apps/LoRaMac/common
    ${LMN}/apps/LoRaMac/common/LmHandler
    ${LMN}/apps/LoRaMac/common/LmHandler/packages
)

target_compile_definitions(${FW_NAME}.elf PRIVATE
    STM32L071xx
    USE_HAL_DRIVER
    SX1272MB2DAS
    REGION_AU915
    ACTIVE_REGION=LORAMAC_REGION_AU915
    LORAMAC_CLASSB_ENABLED
    SOFT_SE
    OVER_THE_AIR_ACTIVATION=1    # padrão da LmHandler; o app/main.c aplica AT+NJM (ABP ou OTAA)
    CONSOLE_UART=${CONSOLE_UART}
    NVM_FCNT_STORE_PERIOD=32
    CONSOLE_BAUDRATE=${CONSOLE_BAUDRATE}
    $<$<BOOL:${USE_DEBUGGER}>:USE_DEBUGGER>
    $<$<BOOL:${RTC_FORCE_LSI}>:RTC_FORCE_LSI>
    $<$<BOOL:${FW_VERSION}>:NODE_FW_VERSION="${FW_VERSION}">
)

set(CPU_FLAGS -mcpu=cortex-m0plus -mthumb -mabi=aapcs)
target_compile_options(${FW_NAME}.elf PRIVATE
    ${CPU_FLAGS}
    -std=gnu11 -Wall -Wno-unused-parameter
    -ffunction-sections -fdata-sections -fno-builtin -fno-common
    $<$<COMPILE_LANGUAGE:ASM>:-x assembler-with-cpp>
)
target_link_options(${FW_NAME}.elf PRIVATE
    ${CPU_FLAGS}
    -T${BRD}/cmsis/stm32l071cz_flash.ld
    --specs=nano.specs --specs=nosys.specs
    -Wl,--gc-sections -Wl,--print-memory-usage
    -Wl,-Map=${CMAKE_BINARY_DIR}/${FW_NAME}.map
    -Wl,--no-warn-rwx-segments
)
target_link_libraries(${FW_NAME}.elf PRIVATE m)

add_custom_command(TARGET ${FW_NAME}.elf POST_BUILD
    COMMAND ${ARM_OBJCOPY} -O binary ${FW_NAME}.elf ${FW_NAME}.bin
    COMMAND ${ARM_OBJCOPY} -O ihex ${FW_NAME}.elf ${FW_NAME}.hex
    COMMAND ${ARM_SIZE} ${FW_NAME}.elf
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Gerando ${FW_NAME}.bin e ${FW_NAME}.hex")
