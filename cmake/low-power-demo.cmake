# Optional standalone image: no FreeRTOS or NimBLE is linked.
# Build explicitly with --target low_power_demo or scripts/low-power-demo.sh.
set(CH32_LP_MODE "4" CACHE STRING "Demo mode: 0 RUN, 1 SLEEP, 2 IRQ, 3 masked WFI, 4 event, 5 pending")
set(CH32_LP_PERIOD "5" CACHE STRING "Demo nominal sleep period in seconds")
option(CH32_LP_PARK_IO "Park GPIOA-D and disable SWJ after the 10-second boot window" OFF)
option(CH32_LP_LSE "Use LSE instead of uncalibrated LSI in the demo" OFF)
ch32_validate_integer_range(CH32_LP_MODE 0 5)
ch32_validate_integer_range(CH32_LP_PERIOD 1 100)

add_executable(low_power_demo EXCLUDE_FROM_ALL
    demo/low_power.c
    app/system_ch32v20x.c
    ${SDK_DIR}/Startup/startup_ch32v20x_D8W.S
)
set_target_properties(low_power_demo PROPERTIES SUFFIX ".elf")
target_compile_definitions(low_power_demo PRIVATE
    LP_MODE=${CH32_LP_MODE}
    LP_PERIOD_SECONDS=${CH32_LP_PERIOD}
    LP_LSE=$<BOOL:${CH32_LP_LSE}>
    LP_PARK_IO=$<BOOL:${CH32_LP_PARK_IO}>
)
target_include_directories(low_power_demo PRIVATE port)
target_link_libraries(low_power_demo PRIVATE sdk_periph)
target_link_options(low_power_demo PRIVATE
    -T${CMAKE_CURRENT_SOURCE_DIR}/ld/Link.ld
    -Wl,--gc-sections
    -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/low_power_demo.map
)
set_property(TARGET low_power_demo APPEND PROPERTY LINK_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/ld/Link.ld
)
add_custom_command(TARGET low_power_demo POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:low_power_demo>
            ${CMAKE_CURRENT_BINARY_DIR}/low_power_demo.bin
    VERBATIM
)
