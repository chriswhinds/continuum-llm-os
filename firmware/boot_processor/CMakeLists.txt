# Boot processor firmware (ARCH-002 §03) -- a separate CMake project from
# the rest of Continuum: it cross-compiles for the RP2040 with
# arm-none-eabi-gcc via the Pico SDK's own toolchain file, not this repo's
# top-level aarch64/x86_64 build.
#
# Verified working recipe (no apt/root needed -- both the toolchain and
# the SDK are plain downloads):
#
#   # ARM's official prebuilt toolchain -- no package manager required
#   curl -LO https://armkeil.blob.core.windows.net/developer/Files/downloads/gnu/13.3.rel1/binrel/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi.tar.xz
#   tar xf arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi.tar.xz
#   export PATH="$PWD/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi/bin:$PATH"
#
#   git clone --depth 1 --branch 2.1.0 https://github.com/raspberrypi/pico-sdk.git
#   export PICO_SDK_PATH="$PWD/pico-sdk"
#   # the TinyUSB submodule is NOT needed -- this firmware never enables
#   # USB stdio (see pico_enable_stdio_usb below) and the build emits only
#   # a harmless warning without it
#
#   cmake -S firmware/boot_processor -B firmware/boot_processor/build
#   cmake --build firmware/boot_processor/build -j"$(nproc)"
#
# This has actually been run end to end: it compiles with zero warnings
# and produces a byte-correct boot_processor.uf2 (verified UF2 magic
# numbers, ELF32/ARM header, entry point in the RP2040's XIP flash
# region). What has NOT been verified is the firmware running on real
# hardware -- no Pico or logic analyzer was available to flash it against.
#
# Flash by holding BOOTSEL on the Pico while plugging it in (it mounts as
# a USB drive) and copying boot_processor.uf2 over. See ARCH-002 §03 and
# this directory's src/main.c for the design.
cmake_minimum_required(VERSION 3.13)

include($ENV{PICO_SDK_PATH}/pico_sdk_init.cmake)

project(continuum_boot_processor C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

add_executable(boot_processor
  src/main.c
  src/i2c_status.c
  src/console_ring.c
)

target_link_libraries(boot_processor
  pico_stdlib
  hardware_i2c
  hardware_watchdog
  hardware_uart
)

# USB and UART stdio are both left off deliberately -- GPIO14/15 (UART0) is
# reserved for the Pi's console per this firmware's own wiring, and a
# second stdio path would just be noise for a device with no interactive
# operator attached in the field.
pico_enable_stdio_usb(boot_processor 0)
pico_enable_stdio_uart(boot_processor 0)

pico_add_extra_outputs(boot_processor)
