# CMake toolchain file for WCH CH32V20x (QingKe RV32IMAC) using the
# official WCH GNU toolchain (riscv32-wch-elf-gcc).
#
# Override the toolchain location with:
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/riscv32-wch.toolchain.cmake \
#         -DWCH_TOOLCHAIN_DIR=/path/to/riscv32-wch-elf-gcc-15 ..
# or with the WCH_TOOLCHAIN_DIR environment variable.

if(NOT WCH_TOOLCHAIN_DIR)
    if(DEFINED ENV{WCH_TOOLCHAIN_DIR})
        set(WCH_TOOLCHAIN_DIR $ENV{WCH_TOOLCHAIN_DIR})
    else()
        set(WCH_TOOLCHAIN_DIR "$ENV{HOME}/DevTools/riscv32-wch-elf-gcc-15")
    endif()
endif()

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR riscv)

set(TOOLCHAIN_PREFIX "${WCH_TOOLCHAIN_DIR}/bin/riscv32-wch-elf")

set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)
set(CMAKE_ASM_COMPILER ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_OBJCOPY      ${TOOLCHAIN_PREFIX}-objcopy CACHE INTERNAL "")
set(CMAKE_OBJDUMP      ${TOOLCHAIN_PREFIX}-objdump CACHE INTERNAL "")
set(CMAKE_SIZE         ${TOOLCHAIN_PREFIX}-size    CACHE INTERNAL "")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# CH32V208 = QingKe V4C: RV32IMAC + CSR instructions + fence.i.
# -mcmodel=medany / -msmall-data-limit=8 mirror the MounRiver (WCH SDK) flags.
set(RISCV_COMMON_FLAGS "-march=rv32imac_zicsr_zifencei -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8")

set(CMAKE_C_FLAGS_INIT   "${RISCV_COMMON_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${RISCV_COMMON_FLAGS}")
set(CMAKE_ASM_FLAGS_INIT "${RISCV_COMMON_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-nostartfiles -specs=nano.specs")

set(CMAKE_FIND_ROOT_PATH "${WCH_TOOLCHAIN_DIR}/riscv32-wch-elf")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
