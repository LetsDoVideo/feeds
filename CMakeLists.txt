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

# Zoom OAuth Client ID. Injected at build time from a CMake variable (which
# in CI comes from a GitHub Actions secret). Falls back to the production
# client ID for local builds and forks where the secret isn't defined.
# The client ID is not cryptographically secret (PKCE doesn't require one),
# but keeping it out of the public repo reduces the surface for imposter-
# app attacks — a concern raised by Zoom's developer relations team.
if(NOT DEFINED FEEDS_ZOOM_CLIENT_ID OR FEEDS_ZOOM_CLIENT_ID STREQUAL "")
    set(FEEDS_ZOOM_CLIENT_ID "JlP6KfRqTt6r0t67FcDuqQ")
    message(STATUS "FEEDS_ZOOM_CLIENT_ID not provided; using default production ID")
else()
    message(STATUS "FEEDS_ZOOM_CLIENT_ID provided via build configuration")
endif()

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

add_executable(FeedsEngine WIN32
    engine/engine-main.cpp
    engine/engine-oauth.cpp
    engine/engine-sdk.cpp
    engine/engine-api.cpp
    engine/engine-meeting.cpp
)
target_include_directories(FeedsEngine PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/zoom-sdk/h")
target_link_directories(FeedsEngine PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/zoom-sdk/lib")
target_link_libraries(FeedsEngine PRIVATE winhttp crypt32 advapi32 shell32 sdk)
target_compile_definitions(FeedsEngine PRIVATE
    FEEDS_ZOOM_CLIENT_ID="${FEEDS_ZOOM_CLIENT_ID}"
)

add_executable(FeedsLogin WIN32 helper/FeedsAuthHelper.cpp)
