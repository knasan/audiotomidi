# Minimal Aubio build (no FFmpeg/sndfile) for FetchContent or local overlay.
# Expects AUBIO_SOURCE_DIR pointing at the upstream aubio source tree.

cmake_minimum_required(VERSION 3.20)

if(NOT AUBIO_SOURCE_DIR)
    message(FATAL_ERROR "AUBIO_SOURCE_DIR must be set")
endif()

project(aubio_vendor C)

add_library(aubio STATIC)

target_compile_definitions(aubio PRIVATE
    HAVE_STDLIB_H=1
    HAVE_STDIO_H=1
    HAVE_MATH_H=1
    HAVE_STRING_H=1
    HAVE_LIMITS_H=1
    HAVE_STDARG_H=1
    HAVE_ERRNO_H=1
    HAVE_C99_VARARGS_MACROS=1
)

file(GLOB_RECURSE AUBIO_SOURCES "${AUBIO_SOURCE_DIR}/src/*.c")
target_sources(aubio PRIVATE ${AUBIO_SOURCES})

target_include_directories(aubio PUBLIC
    "${AUBIO_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}"
)

# Aubio's waf build generates config.h; an empty stub is sufficient for core DSP.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/config.h" "")

find_library(AUBIO_MATH_LIBRARY m)
if(AUBIO_MATH_LIBRARY)
    target_link_libraries(aubio PUBLIC ${AUBIO_MATH_LIBRARY})
endif()

set_target_properties(aubio PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
