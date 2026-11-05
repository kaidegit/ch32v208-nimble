# User-facing settings shared by the firmware, libraries and demo images.
# Keep validation here so invalid cache values fail before compilation.
function(ch32_validate_integer_range name minimum maximum)
    if(NOT "${${name}}" MATCHES "^[0-9]+$")
        message(FATAL_ERROR "${name} must be an integer (${minimum}..${maximum})")
    endif()
    if("${${name}}" LESS "${minimum}" OR "${${name}}" GREATER "${maximum}")
        message(FATAL_ERROR "${name} must be ${minimum}..${maximum}")
    endif()
endfunction()

# Clock and BLE application settings.
set(CH32_CHIP "CH32V208WBU6" CACHE STRING "probe-rs chip name")
set(CH32_SYSCLK_MHZ "96" CACHE STRING "HSE-based CPU clock in MHz")
set_property(CACHE CH32_SYSCLK_MHZ PROPERTY STRINGS 72 96 144)
if(NOT CH32_SYSCLK_MHZ MATCHES "^(72|96|144)$")
    message(FATAL_ERROR "CH32_SYSCLK_MHZ must be 72, 96 or 144")
endif()

option(CH32_BLE_TEST_IDENTITY "Use a separate static random address for GATT testing" OFF)
option(CH32_BLE_PACKET_LOGS "Log BLE packets and GATT payloads" OFF)
set(CH32_ADV_INTERVAL_MS "100" CACHE STRING "Legacy advertising interval in milliseconds")
ch32_validate_integer_range(CH32_ADV_INTERVAL_MS 20 10240)
# BLE legacy advertising uses 625 us units; preserve integer truncation.
math(EXPR CH32_ADV_INTERVAL_UNITS "${CH32_ADV_INTERVAL_MS} * 8 / 5")

# Tickless STOP always retains a mandatory 10-second reflash window.
option(CH32_LOW_POWER "Enable FreeRTOS tickless STOP via RTC events" ON)
option(CH32_POWER_LSE "Use LSE instead of HSE-measured LSI for RTC" OFF)
option(CH32_POWER_DIAGNOSTICS "Print power statistics and heartbeat every second" ON)
option(CH32_POWER_RF_GATE "Gate RF analogue enables while PHY is idle" ON)
option(CH32_POWER_GPIO_ANALOG "Park unused GPIO as analogue inputs instead of pull-downs" OFF)
set(CH32_POWER_WAKE_GUARD_US "3000" CACHE STRING "STOP early-wake margin in microseconds")
ch32_validate_integer_range(CH32_POWER_WAKE_GUARD_US 1000 10000)

# The clock, kernel, logging and BLE libraries all consume these settings.
add_compile_definitions(
    CH32_SYSCLK_MHZ=${CH32_SYSCLK_MHZ}
    CH32_ADV_INTERVAL_UNITS=${CH32_ADV_INTERVAL_UNITS}
    CH32_LOW_POWER=$<BOOL:${CH32_LOW_POWER}>
    CH32_POWER_LSE=$<BOOL:${CH32_POWER_LSE}>
    CH32_POWER_DIAGNOSTICS=$<BOOL:${CH32_POWER_DIAGNOSTICS}>
    CH32_POWER_RF_GATE=$<BOOL:${CH32_POWER_RF_GATE}>
    CH32_POWER_GPIO_ANALOG=$<BOOL:${CH32_POWER_GPIO_ANALOG}>
    CH32_POWER_WAKE_GUARD_US=${CH32_POWER_WAKE_GUARD_US}
)
