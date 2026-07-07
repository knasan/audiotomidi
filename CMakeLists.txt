cmake_minimum_required(VERSION 3.20)

# Use FetchContent_MakeAvailable instead of deprecated FetchContent_Populate (CMake 3.30+).
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 NEW)
endif()

project(audiotomidi VERSION 0.1.0 LANGUAGES CXX C)

# ---------------------------------------------------------------------------
# Deployment target: MOD Dwarf (aarch64, Cortex-A35 @ 1.3 GHz, 48 kHz, ~128 frames).
# Develop on MOD Desktop (x86_64); cross-compile for hardware with mod-plugin-builder:
#   https://github.com/moddevices/mod-plugin-builder
# Desktop builds are for TTL/plugin logic tests — confirm RT headroom on ARM before release.
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Sized deallocation (C++14) emits _ZGTtnam; with -static-libstdc++ it can remain UND
# and break dlopen on MOD Dwarf. Apply to every C++ target in cross-builds.
if(CMAKE_CROSSCOMPILING)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-sized-deallocation>)
endif()

option(AUDIOTOMIDI_FETCH_AUBIO
       "Fetch and build Aubio when not found via pkg-config" ON)
option(AUDIOTOMIDI_BUILD_TESTS "Build Catch2 unit tests" ON)
option(AUDIOTOMIDI_BUILD_LV2 "Build LV2 plugin bundle" ON)
option(AUDIOTOMIDI_GLIBC_217
       "Link LV2 plugin with GLIBC_2.17 symbol versions (old Dwarf firmware)" OFF)

# --- Aubio -------------------------------------------------------------------

set(AUDIOTOMIDI_AUBIO_TARGET "")

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(AUBIO_PKG QUIET aubio)
endif()

if(AUBIO_PKG_FOUND)
    message(STATUS "Using system Aubio via pkg-config")
    add_library(aubio_external INTERFACE)
    target_include_directories(aubio_external INTERFACE ${AUBIO_PKG_INCLUDE_DIRS})
    target_link_libraries(aubio_external INTERFACE ${AUBIO_PKG_LIBRARIES})
    target_compile_options(aubio_external INTERFACE ${AUBIO_PKG_CFLAGS_OTHER})
    set(AUDIOTOMIDI_AUBIO_TARGET aubio_external)
elseif(AUDIOTOMIDI_FETCH_AUBIO)
    message(STATUS "Aubio not found; fetching from GitHub (0.4.9)")
    include(FetchContent)
    FetchContent_Declare(
        aubio_upstream
        GIT_REPOSITORY https://github.com/aubio/aubio.git
        GIT_TAG        0.4.9
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(aubio_upstream)
    set(AUBIO_SOURCE_DIR "${aubio_upstream_SOURCE_DIR}" CACHE INTERNAL "")
    add_subdirectory(cmake/aubio "${CMAKE_BINARY_DIR}/aubio-build")
    set(AUDIOTOMIDI_AUBIO_TARGET aubio)
else()
    message(FATAL_ERROR
        "Aubio not found. Install libaubio-dev or set AUDIOTOMIDI_FETCH_AUBIO=ON")
endif()

# --- DSP library -------------------------------------------------------------

add_library(audiotomidi_dsp STATIC
    src/dsp/analyser.cpp
    src/dsp/logger.cpp
)

target_include_directories(audiotomidi_dsp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

target_link_libraries(audiotomidi_dsp PUBLIC ${AUDIOTOMIDI_AUBIO_TARGET})

target_compile_definitions(audiotomidi_dsp PRIVATE AUBIO_UNSTABLE=1)

set_target_properties(audiotomidi_dsp PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    target_compile_options(audiotomidi_dsp PRIVATE -O2)
endif()

# --- Tests -------------------------------------------------------------------

if(AUDIOTOMIDI_BUILD_TESTS)
    find_package(Threads REQUIRED)
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.7.1
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(Catch2)

    add_executable(audiotomidi_tests
        tests/test_types.cpp
        tests/test_analyser.cpp
        tests/test_ringbuffer.cpp
        tests/test_logger.cpp
    )

    target_link_libraries(audiotomidi_tests
        PRIVATE
            audiotomidi_dsp
            Catch2::Catch2WithMain
            Threads::Threads
    )

    include(CTest)
    include(Catch)
    catch_discover_tests(audiotomidi_tests)
endif()

# --- LV2 plugin --------------------------------------------------------------

if(AUDIOTOMIDI_BUILD_LV2)
    find_package(Threads REQUIRED)
    set(AUDIOTOMIDI_LV2_INCLUDE_DIRS "")

    if(PkgConfig_FOUND)
        pkg_check_modules(LV2_PKG QUIET lv2)
    endif()

    if(LV2_PKG_FOUND)
        message(STATUS "Using system LV2 via pkg-config")
        list(APPEND AUDIOTOMIDI_LV2_INCLUDE_DIRS ${LV2_PKG_INCLUDE_DIRS})
    else()
        message(STATUS "LV2 not found; fetching headers from GitLab (v1.18.8)")
        include(FetchContent)
        FetchContent_Declare(
            lv2_upstream
            GIT_REPOSITORY https://gitlab.com/lv2/lv2.git
            GIT_TAG        v1.18.8
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(lv2_upstream)
        list(APPEND AUDIOTOMIDI_LV2_INCLUDE_DIRS
             "${lv2_upstream_SOURCE_DIR}/include")
    endif()

    set(AUDIOTOMIDI_LV2_BUNDLE_DIR "${CMAKE_BINARY_DIR}/audiotomidi.lv2")

    add_library(audiotomidi_lv2 MODULE
        src/lv2/plugin.cpp
    )

    target_include_directories(audiotomidi_lv2
        PRIVATE
            ${AUDIOTOMIDI_LV2_INCLUDE_DIRS}
            "${CMAKE_CURRENT_SOURCE_DIR}/src/lv2"
    )

    target_link_libraries(audiotomidi_lv2 PRIVATE audiotomidi_dsp Threads::Threads)

    target_compile_options(audiotomidi_lv2 PRIVATE
        -fvisibility=hidden
        -Wall
        -Wextra
    )

    if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        target_compile_options(audiotomidi_lv2 PRIVATE -O2)
    endif()

    # MOD Dwarf: embed full libstdc++/libgcc so no unresolved C++ runtime symbols remain.
    if(CMAKE_CROSSCOMPILING)
        target_link_options(audiotomidi_lv2 PRIVATE
            -static-libgcc
            "-Wl,--whole-archive"
            "-lstdc++"
            "-Wl,--no-whole-archive"
        )
        if(AUDIOTOMIDI_GLIBC_217)
            message(STATUS "Linking audiotomidi_lv2 with GLIBC_2.17 version script")
            target_link_options(audiotomidi_lv2 PRIVATE
                "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/glibc-2.17.ver"
            )
        endif()
    endif()

    set_target_properties(audiotomidi_lv2 PROPERTIES
        PREFIX ""
        OUTPUT_NAME "audiotomidi"
        LIBRARY_OUTPUT_DIRECTORY "${AUDIOTOMIDI_LV2_BUNDLE_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${AUDIOTOMIDI_LV2_BUNDLE_DIR}"
    )

    add_custom_command(TARGET audiotomidi_lv2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/lv2/manifest.ttl"
            "${CMAKE_CURRENT_SOURCE_DIR}/lv2/audiotomidi.ttl"
            "${CMAKE_CURRENT_SOURCE_DIR}/lv2/modgui.ttl"
            "${AUDIOTOMIDI_LV2_BUNDLE_DIR}/"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/lv2/modgui"
            "${AUDIOTOMIDI_LV2_BUNDLE_DIR}/modgui"
        COMMENT "Copying LV2 TTL manifests and MOD GUI into bundle"
    )
endif()
