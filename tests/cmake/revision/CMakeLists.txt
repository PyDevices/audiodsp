# Build-time revision harness for audiodsp's CMake glue (audiodsp#156).
#
#   cmake -S tests/cmake/revision -B <build dir> -DAUDIODSP=<this checkout>
#   cmake --build <build dir> && <build dir>/firmware
#
# Prints the version and revision a firmware built from this checkout would
# report. A CMake port configures once and then builds incrementally, so the
# check that matters is the second build: move the checkout's HEAD, build
# again without reconfiguring, and the revision printed must follow.
# clean-build.yml does exactly that.
#
# It is a stand-in for a port: a `usermod` interface target, this repository's
# micropython.cmake included, and a program linking `usermod`. Only the
# revision is under test, so the module's sources are swapped for one file and
# nothing from MicroPython is needed.
cmake_minimum_required(VERSION 3.16)
project(audiodsp_revision_check C)

if(NOT DEFINED AUDIODSP)
    message(FATAL_ERROR "pass -DAUDIODSP=<path to this checkout>")
endif()

add_library(usermod INTERFACE)
include(${AUDIODSP}/micropython.cmake)
set_property(TARGET usermod_mpaudio PROPERTY
    INTERFACE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/firmware.c)
add_executable(firmware)
target_link_libraries(firmware usermod)
