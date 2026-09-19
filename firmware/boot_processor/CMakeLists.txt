# Boot processor firmware (ARCH-002 §03) -- a separate CMake project from
# the rest of Continuum: it cross-compiles for the RP2040 with
# arm-none-eabi-gcc via the Pico SDK's own toolchain file, not this repo's
# top-level aarch64/x86_64 build. Build it on its own:
#
#   export PICO_SDK_PATH=/path/to/pico-sdk
#   cmake -S firmware/boot_processor -B firmware/boot_processor/build
#   cmake --build firmware/boot_processor/build
#
# Output is boot_processor.uf2 -- flash by holding BOOTSEL on the Pico
# while plugging it in (it mounts as a USB drive) and copying the file
# over. See ARCH-002 §03 and this directory's src/main.c for the design.
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
