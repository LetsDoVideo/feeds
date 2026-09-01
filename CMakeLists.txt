cmake_minimum_required(VERSION 3.28...3.30)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)
project(${_name} VERSION ${_version})

set(OBS_PLUGIN_DESTINATION "obs-plugins/64bit")
set(OBS_PLUGIN_DATA_DESTINATION "data/obs-plugins/${CMAKE_PROJECT_NAME}")

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES
        AUTOMOC ON
        AUTOUIC ON
        AUTORCC ON
)

# Zoom OAuth Client ID. Must be supplied at configure time via -D or env
# variable; in CI this comes from a GitHub Actions secret. The client ID is
# not cryptographically secret (PKCE doesn't require one), but keeping it
# out of the public repo reduces the surface for imposter-app attacks — a
# concern raised by Zoom's developer relations team. We fail loudly when
# missing rather than baking a fallback into the source tree.
if(NOT DEFINED FEEDS_ZOOM_CLIENT_ID OR FEEDS_ZOOM_CLIENT_ID STREQUAL "")
    message(FATAL_ERROR "FEEDS_ZOOM_CLIENT_ID is required at build time. Set via -D or env variable.")
endif()
message(STATUS "FEEDS_ZOOM_CLIENT_ID provided via build configuration")

find_package(libobs REQUIRED)

if(MSVC)
    add_compile_options(/WX-)
endif()

# Core libraries
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs ws2_32 crypt32 winhttp shlwapi)

if(ENABLE_FRONTEND_API)
    find_package(obs-frontend-api REQUIRED)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
    # Svg: the Connect chooser renders bundled Tabler glyph SVGs (data/icons)
    # tinted via QSvgRenderer. OBS ships Qt6Svg.dll and the obs-deps Qt6
    # prebuilt includes the Svg dev module, so this resolves in CI.
    find_package(Qt6 COMPONENTS Widgets Core Svg QUIET)
    if(NOT Qt6_FOUND)
        find_package(Qt5 COMPONENTS Widgets Core Svg REQUIRED)
        target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt5::Core Qt5::Widgets Qt5::Svg)
    else()
        target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets Qt6::Svg)
    endif()
endif()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    src/plugin-main.cpp
    src/engine-client.cpp
    src/feeds-card-render.cpp
    src/feeds-chat-popup-source.cpp
    src/feeds-chat-overlay-source.cpp
    src/feeds-lower-third-source.cpp
    src/feeds-iso-recorder.cpp
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/common"
)

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    FEEDS_ZOOM_CLIENT_ID="${FEEDS_ZOOM_CLIENT_ID}"
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

install(TARGETS ${CMAKE_PROJECT_NAME}
    LIBRARY DESTINATION ${OBS_PLUGIN_DESTINATION}
    RUNTIME DESTINATION ${OBS_PLUGIN_DESTINATION}
)

if(MSVC)
    install(FILES $<TARGET_PDB_FILE:${CMAKE_PROJECT_NAME}>
        DESTINATION ${OBS_PLUGIN_DESTINATION}
        OPTIONAL
    )
endif()

# libyuv for the engine's frame scaler. Pulled in via FetchContent rather
# than vcpkg / git submodule so no new tooling lands on the CI runner —
# the obsproject Windows runner already has network access for the
# buildspec.json prebuilt downloads, and FetchContent piggy-backs on
# that. EXCLUDE_FROM_ALL keeps libyuv's tests/tools out of the default
# build target.
#
# GIT_TAG is pinned to upstream 94417b9d (2025-09-26, "Pass rgbconstants
# via struct pointer"). libyuv doesn't publish semver tags. This pin
# predates upstream f2ac6db (2026-04-29, "RAWToNV21 using SME ..."),
# which adds `__attribute__((no_sanitize("cfi-icall")))` to
# source/row_win.cc — a Clang-only attribute MSVC rejects with no
# workaround short of patching libyuv.
#
# Warnings: even on this older pin, MSVC's C4244 trips on libyuv's
# integer-truncation patterns under /WX. Rather than hunt further back
# (older commits have other issues, and any new pin will likely
# re-introduce something), we accept libyuv's warnings as upstream's
# problem. libyuv defines several targets (yuv, yuv_common_objects,
# possibly more depending on build options) so enumerating them with
# set_target_properties is brittle — instead, save / clear /
# CMAKE_COMPILE_WARNING_AS_ERROR around FetchContent_MakeAvailable so
# every target libyuv creates inherits the OFF default. Our own targets
# are defined below, after the restore, so they pick the original
# strict value back up.
include(FetchContent)
FetchContent_Declare(
    libyuv
    GIT_REPOSITORY https://chromium.googlesource.com/libyuv/libyuv.git
    GIT_TAG        94417b9d213364905ce849c25719b819b8dbbaaa
    EXCLUDE_FROM_ALL
)
set(_FEEDS_SAVED_WAE ${CMAKE_COMPILE_WARNING_AS_ERROR})
set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)
FetchContent_MakeAvailable(libyuv)
set(CMAKE_COMPILE_WARNING_AS_ERROR ${_FEEDS_SAVED_WAE})

add_executable(FeedsEngine WIN32
    engine/engine-main.cpp
    engine/engine-oauth.cpp
    engine/engine-sdk.cpp
    engine/engine-api.cpp
    engine/engine-meeting.cpp
    engine/engine-video.cpp
    engine/engine-screenshare.cpp
    engine/engine-frame-scaler.cpp
)
target_include_directories(FeedsEngine PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/zoom-sdk/h"
    "${CMAKE_CURRENT_SOURCE_DIR}/common"
    # libyuv: belt-and-braces. Modern libyuv CMake propagates via
    # target_link_libraries, but older or custom builds may not — this
    # makes <libyuv/scale.h> resolve either way.
    "${libyuv_SOURCE_DIR}/include"
)
target_link_directories(FeedsEngine PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/zoom-sdk/lib")
target_link_libraries(FeedsEngine PRIVATE winhttp crypt32 advapi32 shell32 sdk yuv)
target_compile_definitions(FeedsEngine PRIVATE
    FEEDS_ZOOM_CLIENT_ID="${FEEDS_ZOOM_CLIENT_ID}"
)

# Unit tests — standalone console exes covering the shared IPC helpers
# (>4KB E2P reader reassembly + string-aware JSON parsing). No libobs/Qt/SDK,
# so they build and run in CI as the "parsing-test target" on the roadmap.
option(FEEDS_BUILD_TESTS "Build Feeds unit tests" ON)
if(FEEDS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
