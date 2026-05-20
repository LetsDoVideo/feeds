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
    find_package(Qt6 COMPONENTS Widgets Core QUIET)
    if(NOT Qt6_FOUND)
        find_package(Qt5 COMPONENTS Widgets Core REQUIRED)
        target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt5::Core Qt5::Widgets)
    else()
        target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
    endif()
endif()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    src/plugin-main.cpp
    src/engine-client.cpp
    src/feeds-chat-popup-source.cpp
    src/feeds-chat-overlay-source.cpp
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
# GIT_TAG is pinned to a specific upstream commit (libyuv doesn't
# publish semver tags). Bump deliberately when picking up upstream fixes;
# CI builds against the pinned revision so the same Feeds tag always
# rebuilds against the same libyuv.
include(FetchContent)
FetchContent_Declare(
    libyuv
    GIT_REPOSITORY https://chromium.googlesource.com/libyuv/libyuv.git
    GIT_TAG        9d98aaefe7a5e2710aa6175d44da38892400b381
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(libyuv)

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

add_executable(FeedsLogin WIN32 helper/FeedsAuthHelper.cpp)
