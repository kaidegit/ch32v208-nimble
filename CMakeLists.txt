#
# CH32V208 + Apache NimBLE (controller + host) verification project.
#
# Architecture (see README.md):
#   NimBLE host ── native transport ── NimBLE controller/LL
#                                          │
#                            ble_phy_wch.c / ble_hw_wch.c   (this project)
#                                          │
#                            wch_rf.c : BB / LLE / RFEND / AES
#                                       register-level driver
#                                          │
#                     FreeRTOS + NPL(freertos) + SEGGER RTT log
#
cmake_minimum_required(VERSION 3.20)

project(ch32v208_nimble C ASM)

# ---------------------------------------------------------------- paths ----
set(WS_DIR        ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(NIMBLE_DIR    ${WS_DIR}/nimble)                  # apache/mynewt-nimble @ master (1.9.0-dev)
set(SDK_DIR       ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sdk)
set(FREERTOS_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/freertos)
set(ELOG_DIR      ${CMAKE_CURRENT_SOURCE_DIR}/third_party/easylogger)

if(NOT EXISTS ${NIMBLE_DIR}/porting)
    message(FATAL_ERROR "nimble checkout not found at ${NIMBLE_DIR}")
endif()

# --------------------------------------------------------------- options ---
set(CH32_CHIP "CH32V208WBU6" CACHE STRING "probe-rs chip name used in docs/scripts")
option(CH32_BLE_TEST_IDENTITY "Use a separate static random address for GATT schema testing" OFF)

# ------------------------------------------------------------- flags -------
add_compile_options(
    -Wall
    -Wextra
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-missing-field-initializers
    -ffunction-sections
    -fdata-sections
    $<$<CONFIG:Debug>:-O0>
    $<$<CONFIG:Release>:-Os>
    -g3
    -fno-common
)

# ================================================================ libs =====

# ---------------------------------------------------------- WCH SDK HAL ---
file(GLOB SDK_SRC
    ${SDK_DIR}/Peripheral/src/*.c
    ${SDK_DIR}/Core/*.c
)
list(FILTER SDK_SRC EXCLUDE REGEX ".*ch32v20x_can\\.c|.*ch32v20x_usb.*|.*ch32v20x_eth.*|.*ch32v20x_iwdg.*|.*ch32v20x_wwdg.*|.*ch32v20x_opa.*")
add_library(sdk_periph STATIC ${SDK_SRC})
target_include_directories(sdk_periph PUBLIC
    ${SDK_DIR}/Peripheral/inc
    ${SDK_DIR}/Core
    ${CMAKE_CURRENT_SOURCE_DIR}/config
)
target_compile_definitions(sdk_periph PUBLIC CH32V20x_D8W)

# ------------------------------------------------------ FreeRTOS kernel ---
set(FREERTOS_SRC
    ${FREERTOS_DIR}/croutine.c
    ${FREERTOS_DIR}/event_groups.c
    ${FREERTOS_DIR}/list.c
    ${FREERTOS_DIR}/queue.c
    ${FREERTOS_DIR}/stream_buffer.c
    ${FREERTOS_DIR}/tasks.c
    ${FREERTOS_DIR}/timers.c
    ${FREERTOS_DIR}/portable/GCC/RISC-V/port.c
    ${FREERTOS_DIR}/portable/GCC/RISC-V/portASM.S
    ${FREERTOS_DIR}/portable/MemMang/heap_4.c
)
add_library(freertos_kernel STATIC ${FREERTOS_SRC})
target_include_directories(freertos_kernel PUBLIC
    ${FREERTOS_DIR}/include
    ${FREERTOS_DIR}/portable/GCC/RISC-V
    ${CMAKE_CURRENT_SOURCE_DIR}/config
)

# ------------------------------------------------------------- logger -----
# EasyLogger core (vendored, see third_party/easylogger/VENDORED.md) plus
# the platform port: USART1 DMA ring sink, console facade, NimBLE NPL log
# sink, polled emergency output (port/console_uart.c, port/wch_log.h).
# config/ comes first on the include path so config/elog_cfg.h shadows the
# upstream easylogger/inc/elog_cfg.h defaults.
# elog_async.c (POSIX pthread) and elog_buf.c are NOT compiled: async
# decoupling is the project's own DMA ring.
add_library(logger STATIC
    ${ELOG_DIR}/src/elog.c
    ${ELOG_DIR}/src/elog_utils.c
    port/console_uart.c
)
target_include_directories(logger PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/config
    ${CMAKE_CURRENT_SOURCE_DIR}/port
    ${ELOG_DIR}/inc
    ${SDK_DIR}/Peripheral/inc
    ${SDK_DIR}/Core
    ${FREERTOS_DIR}/include
    ${FREERTOS_DIR}/portable/GCC/RISC-V
)
target_link_libraries(logger PUBLIC freertos_kernel sdk_periph)

# ------------------------------------------------------------ NimBLE core -
# host + controller + transport + porting glue (FreeRTOS NPL).
# Excluded: porting/nimble/src/hal_timer.c      (nRF-specific, ours is port/hal_timer_wch.c)
#           porting/nimble/src/nimble_port.c    (hardcoded 32768 cputime,  ours is port/nimble_port_wch.c)
#           porting/nimble/src/os_cputime_pwr2.c(power-of-2 tick freq variant)
file(GLOB NIMBLE_HOST_SRC        ${NIMBLE_DIR}/nimble/host/src/*.c)
file(GLOB NIMBLE_CONTROLLER_SRC  ${NIMBLE_DIR}/nimble/controller/src/*.c)
file(GLOB NIMBLE_TRANSPORT_SRC   ${NIMBLE_DIR}/nimble/transport/src/*.c)
file(GLOB NIMBLE_PORT_SRC        ${NIMBLE_DIR}/porting/nimble/src/*.c)
list(FILTER NIMBLE_PORT_SRC EXCLUDE REGEX ".*hal_timer\\.c|.*nimble_port\\.c|.*os_cputime_pwr2\\.c")
file(GLOB NIMBLE_NPL_FREERTOS_SRC ${NIMBLE_DIR}/porting/npl/freertos/src/*.c)

add_library(nimble_core STATIC
    ${NIMBLE_HOST_SRC}
    ${NIMBLE_CONTROLLER_SRC}
    ${NIMBLE_TRANSPORT_SRC}
    ${NIMBLE_PORT_SRC}
    ${NIMBLE_NPL_FREERTOS_SRC}
    ${NIMBLE_DIR}/nimble/host/services/gap/src/ble_svc_gap.c
    ${NIMBLE_DIR}/nimble/host/services/gatt/src/ble_svc_gatt.c
)
target_include_directories(nimble_core PUBLIC
    # config/ first: provides vendored syscfg/syscfg.h (+ overrides)
    ${CMAKE_CURRENT_SOURCE_DIR}/config
    ${NIMBLE_DIR}/porting/nimble/include
    ${NIMBLE_DIR}/porting/npl/freertos/include
    ${NIMBLE_DIR}/nimble/include
    ${NIMBLE_DIR}/nimble/host/include
    ${NIMBLE_DIR}/nimble/host/services/gap/include
    ${NIMBLE_DIR}/nimble/host/services/gatt/include
    ${NIMBLE_DIR}/nimble/controller/include
    ${NIMBLE_DIR}/nimble/transport/include
)
target_compile_definitions(nimble_core PUBLIC
    NIMBLE_CFG_CONTROLLER=1
    NIMBLE_CFG_HOST=1
)
target_link_libraries(nimble_core PUBLIC freertos_kernel)

# -------------------------------------------------------- WCH BLE driver ---
# Register-level RF/BB/LLE/AES driver + NimBLE PHY/HW adaptation + timer port.
add_library(wch_ble STATIC
    drivers/wch_ble/wch_rf.c
    drivers/wch_ble/wch_rf_analog.c
    drivers/wch_ble/ble_phy_wch.c
    drivers/wch_ble/ble_hw_wch.c
    port/hal_timer_wch.c
    port/nimble_port_wch.c
    port/ble_ll_coded.c
    # Software AES-128 + CMAC behind the config/mbedtls/ headers; NimBLE's
    # ble_ll_crypto.c / ble_sm_alg.c include <mbedtls/cmac.h> unconditionally.
    port/crypto_mbedtls_shim.c
)
target_include_directories(wch_ble PUBLIC
    drivers/wch_ble
    port
    ${CMAKE_CURRENT_SOURCE_DIR}/config
)
target_link_libraries(wch_ble PUBLIC nimble_core sdk_periph logger)
# ISR-sensitive code goes into .highcode (see ld/Link.ld)
set_source_files_properties(drivers/wch_ble/wch_rf.c
    PROPERTIES COMPILE_FLAGS "-fno-builtin")

# =============================================================== images ===

# Full firmware: NimBLE host + controller over the WCH RF driver.
add_executable(ch32v208_nimble
    port/syscalls.c
    app/main.c
    app/app_ble.c
    app/ch32v20x_it.c
    app/system_ch32v20x.c
    ${SDK_DIR}/Startup/startup_ch32v20x_D8W.S
)
# .elf suffix so scripts/ and probe-rs commands can reference
# build/ch32v208_nimble.elf verbatim.
set_target_properties(ch32v208_nimble PROPERTIES SUFFIX ".elf")
target_compile_definitions(ch32v208_nimble PRIVATE
    APP_BLE_TEST_IDENTITY=$<BOOL:${CH32_BLE_TEST_IDENTITY}>)
# wch_ble and nimble_core reference each other (LL calls ble_phy_*, the PHY
# port calls ble_ll_*), so the archives must be rescanned as a group.
target_link_libraries(ch32v208_nimble
    PRIVATE -Wl,--start-group
            wch_ble nimble_core sdk_periph freertos_kernel logger
            -Wl,--end-group)
target_include_directories(ch32v208_nimble PRIVATE app port)
target_link_options(ch32v208_nimble PRIVATE
    -T${CMAKE_CURRENT_SOURCE_DIR}/ld/Link.ld
    -Wl,--wrap=ble_ll_conn_hci_le_set_phy
    -Wl,--gc-sections
    -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/ch32v208_nimble.map
    -Wl,--print-memory-usage
)
add_custom_command(TARGET ch32v208_nimble POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:ch32v208_nimble>
            ${CMAKE_CURRENT_BINARY_DIR}/ch32v208_nimble.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:ch32v208_nimble>
            ${CMAKE_CURRENT_BINARY_DIR}/ch32v208_nimble.bin
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:ch32v208_nimble>
    VERBATIM
)

# Bring-up image: startup + clock + FreeRTOS + UART log banner only
# (main.c's app_ble_start() has a weak default; used to validate the
#  toolchain/boot path before the BLE stack is linked in).
add_executable(boot
    port/syscalls.c
    app/main.c
    app/ch32v20x_it.c
    app/system_ch32v20x.c
    ${SDK_DIR}/Startup/startup_ch32v20x_D8W.S
)
set_target_properties(boot PROPERTIES SUFFIX ".elf")
target_link_libraries(boot PRIVATE sdk_periph freertos_kernel logger)
target_include_directories(boot PRIVATE app port)
target_link_options(boot PRIVATE
    -T${CMAKE_CURRENT_SOURCE_DIR}/ld/Link.ld
    -Wl,--gc-sections
    -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/boot.map
)

# Relink both images when the CRT memory layout changes.
set_property(TARGET ch32v208_nimble boot APPEND PROPERTY LINK_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/ld/Link.ld)
