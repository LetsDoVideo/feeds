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
    if(WIN32)
        message(FATAL_ERROR "FEEDS_ZOOM_CLIENT_ID is required at build time. Set via -D or env variable.")
    else()
        # macOS plugin shell: no engine is built and nothing in the plugin reads
        # the client ID, so it isn't required here. The Mac engine port will
        # restore the requirement for the platform that builds an engine.
        set(FEEDS_ZOOM_CLIENT_ID "not-used-by-the-plugin-shell")
        message(STATUS "FEEDS_ZOOM_CLIENT_ID not required: no engine on this platform")
    endif()
else()
    message(STATUS "FEEDS_ZOOM_CLIENT_ID provided via build configuration")
endif()

find_package(libobs REQUIRED)

if(MSVC)
    add_compile_options(/WX-)
endif()

# Core libraries
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)
if(WIN32)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ws2_32 crypt32 winhttp shlwapi)
endif()

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
    src/feeds-card-render.cpp
    src/feeds-chat-popup-source.cpp
    src/feeds-chat-overlay-source.cpp
    src/feeds-lower-third-source.cpp
    src/feeds-iso-recorder.cpp
)

# The engine client: same four entry points either way, different transport.
# Windows launches FeedsEngine.exe over two named pipes; POSIX (macOS) spawns
# FeedsEngine.app over a socketpair. See src/engine-client-posix.cpp.
if(WIN32)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/engine-client.cpp)
else()
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/engine-client-posix.cpp)
endif()

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/common"
)

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    FEEDS_ZOOM_CLIENT_ID="${FEEDS_ZOOM_CLIENT_ID}"
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

# macOS installs the .plugin bundle (and builds the .pkg) from
# set_target_properties_plugin in cmake/macos/helpers.cmake; this Windows-layout
# install would drop a second copy under obs-plugins/64bit.
if(NOT APPLE)
    install(TARGETS ${CMAKE_PROJECT_NAME}
        LIBRARY DESTINATION ${OBS_PLUGIN_DESTINATION}
        RUNTIME DESTINATION ${OBS_PLUGIN_DESTINATION}
    )
endif()

if(MSVC)
    install(FILES $<TARGET_PDB_FILE:${CMAKE_PROJECT_NAME}>
        DESTINATION ${OBS_PLUGIN_DESTINATION}
        OPTIONAL
    )
endif()

# ---------------------------------------------------------------------------
# macOS engine (increment 1a): FeedsEngine.app, the process that hosts the Zoom
# Meeting SDK. Objective-C++ against ZoomSDK.framework — a rewrite of the
# Windows engine's SDK calls, not a port of those files (the macOS SDK shares no
# API with the C++ one). See engine/mac/feeds-engine-mac.mm.
#
# The SDK is licensed and ~600 MB, so it is NOT in this repo: point
# FEEDS_MAC_ZOOM_SDK_DIR at the directory that holds ZoomSDK.framework and its
# sibling frameworks/bundles (the "ZoomSDK" folder of Zoom's macOS SDK archive),
# by -D or by environment variable. Without it the plugin still builds and
# loads; it simply has no engine, and says so at runtime.
# ---------------------------------------------------------------------------
if(APPLE)
    if(NOT DEFINED FEEDS_MAC_ZOOM_SDK_DIR OR FEEDS_MAC_ZOOM_SDK_DIR STREQUAL "")
        set(FEEDS_MAC_ZOOM_SDK_DIR "$ENV{FEEDS_MAC_ZOOM_SDK_DIR}")
    endif()

    if(FEEDS_MAC_ZOOM_SDK_DIR AND EXISTS "${FEEDS_MAC_ZOOM_SDK_DIR}/ZoomSDK.framework")
        message(STATUS "Feeds: building the macOS engine against ${FEEDS_MAC_ZOOM_SDK_DIR}")
        enable_language(OBJCXX)

        add_executable(
            FeedsEngine MACOSX_BUNDLE
            engine/mac/feeds-engine-mac.mm   # process, IPC, SDK, join, roster
            engine/mac/feeds-mac-login.mm    # OAuth + Zoom REST (the login path)
            engine/mac/feeds-mac-net.mm      # HTTPS, PKCE crypto, Keychain, browser
        )

        set_target_properties(
            FeedsEngine
            PROPERTIES
                OUTPUT_NAME FeedsEngine
                OBJCXX_STANDARD 17
                OBJCXX_STANDARD_REQUIRED TRUE
                MACOSX_BUNDLE_INFO_PLIST
                    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/FeedsEngine-Info.plist.in"
                XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER com.letsdovideo.feeds.engine
                # The SDK lives in the app's own Frameworks at runtime; the build
                # tree resolves it where it was unpacked. BUILD_RPATH is required,
                # not optional: the framework is linked through raw -F/-framework
                # flags rather than an imported target, so CMake can infer no
                # build-tree rpath and the binary would ship with none at all.
                BUILD_WITH_INSTALL_RPATH OFF
                BUILD_RPATH "${FEEDS_MAC_ZOOM_SDK_DIR}"
                INSTALL_RPATH "@executable_path/../Frameworks"
                # See cmake/macos/FeedsEngine.entitlements: the engine loads a
                # framework signed by ZOOM, and library validation refuses any
                # library whose Team ID differs from the process's. Only this
                # binary gets the exception; the plugin (and so OBS) must not.
                XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS
                    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/FeedsEngine.entitlements"
        )

        # Ad-hoc builds additionally drop the hardened runtime for THIS target.
        # The template turns it on for every non-Debug configuration
        # (cmake/macos/xcode.cmake), and the hardened runtime is what switches
        # library validation on in the first place. Belt and braces: the
        # entitlement above is the real fix and is what a notarized build (which
        # must keep the hardened runtime) will rely on, while dropping it here
        # means an ad-hoc CI build does not depend on entitlements being honored
        # for a signature with no team behind it at all.
        if(CODESIGN_IDENTITY STREQUAL "-")
            set_target_properties(
                FeedsEngine
                PROPERTIES XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME NO
            )
            set(_feeds_engine_hardened_args "")
        else()
            set(_feeds_engine_hardened_args "--options;runtime")
        endif()

        target_include_directories(FeedsEngine PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/common"
        )
        target_compile_definitions(FeedsEngine PRIVATE
            FEEDS_ZOOM_CLIENT_ID="${FEEDS_ZOOM_CLIENT_ID}"
        )
        # -F has to be on BOTH lines: the compile line so `#import <ZoomSDK/...>`
        # resolves through the framework's Headers, the link line so
        # `-framework ZoomSDK` finds the binary.
        target_compile_options(FeedsEngine PRIVATE "-F${FEEDS_MAC_ZOOM_SDK_DIR}")
        target_link_libraries(FeedsEngine PRIVATE
            "-F${FEEDS_MAC_ZOOM_SDK_DIR}"
            "-framework ZoomSDK"
            "-framework Cocoa"
            # Keychain (SecItem*) and SecRandomCopyBytes, for the login path.
            # CommonCrypto's SHA-256 needs no framework — it is in libSystem.
            "-framework Security"
        )

        # The SDK runtime must sit in the ENGINE APP's Frameworks directory.
        # ZoomSDK.framework is not self-contained: at auth time it loads sibling
        # bundles through the MAIN BUNDLE's Frameworks path, not through rpath.
        # Without them initSDK still reports success and sdkAuth then fails with
        # no callback at all — so this copy is a runtime requirement, not
        # packaging tidiness. ditto preserves the frameworks' symlinks, which a
        # plain copy would flatten.
        add_custom_command(
            TARGET FeedsEngine
            POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                    "$<TARGET_BUNDLE_DIR:FeedsEngine>/Contents"
            COMMAND ditto "${FEEDS_MAC_ZOOM_SDK_DIR}"
                    "$<TARGET_BUNDLE_DIR:FeedsEngine>/Contents/Frameworks"
            # Re-sign AFTER the SDK lands inside the app. Xcode signed this
            # bundle at the end of its own build phase, i.e. before the copy
            # above, so that signature no longer describes the bundle's
            # contents. Re-signing here also guarantees the entitlement is
            # attached to the artifact that actually ships, rather than
            # depending on when Xcode chose to sign.
            #
            # Zoom's own nested frameworks keep THEIR signatures: this signs the
            # app, not its contents (no --deep), which is exactly the
            # different-Team-ID arrangement the entitlement exists to permit. A
            # notarized build will additionally have to re-sign every nested
            # item with our Developer ID, deepest first; that belongs with the
            # signing/notarization work, not here.
            COMMAND codesign --force --sign "${CODESIGN_IDENTITY}"
                    ${_feeds_engine_hardened_args}
                    --entitlements
                    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/FeedsEngine.entitlements"
                    --timestamp=none
                    "$<TARGET_BUNDLE_DIR:FeedsEngine>"
            COMMENT "Copying the Zoom macOS SDK runtime into FeedsEngine.app and re-signing it"
            VERBATIM
        )

        # Ship the engine inside the plugin bundle, beside the loaded module:
        # <plugin>.plugin/Contents/MacOS/FeedsEngine.app. That is exactly where
        # engine-client-posix.cpp looks for it.
        add_dependencies(${CMAKE_PROJECT_NAME} FeedsEngine)
        add_custom_command(
            TARGET ${CMAKE_PROJECT_NAME}
            POST_BUILD
            COMMAND ditto "$<TARGET_BUNDLE_DIR:FeedsEngine>"
                    "$<TARGET_BUNDLE_DIR:${CMAKE_PROJECT_NAME}>/Contents/MacOS/FeedsEngine.app"
            # Same reason as the engine: the plugin bundle was signed before the
            # engine was copied into it. Deliberately NO entitlements here — the
            # plugin loads no Zoom framework, and library validation must stay
            # on for the bundle OBS loads into its own process.
            COMMAND codesign --force --sign "${CODESIGN_IDENTITY}" --timestamp=none
                    "$<TARGET_BUNDLE_DIR:${CMAKE_PROJECT_NAME}>"
            COMMENT "Staging FeedsEngine.app inside the plugin bundle and re-signing it"
            VERBATIM
        )

        # Belt and braces for the installed artifact (the .pkg is built from the
        # install tree). The POST_BUILD copy above already put the engine inside
        # the built bundle, and installing a bundle target copies it wholesale —
        # but that leaves the engine's presence in the shipped package depending
        # on an implementation detail. Install it explicitly so the packaged
        # plugin cannot silently come out engine-less, which looks completely
        # correct right up until it cannot connect to Zoom.
        install(
            DIRECTORY "$<TARGET_BUNDLE_DIR:FeedsEngine>"
            DESTINATION "${_name}.plugin/Contents/MacOS"
            USE_SOURCE_PERMISSIONS
        )
    else()
        message(
            WARNING
            "Feeds: FEEDS_MAC_ZOOM_SDK_DIR is unset or has no ZoomSDK.framework, "
            "so the macOS engine will NOT be built. The plugin will load and show "
            "its UI, but Zoom features stay unavailable. Set it to the SDK's "
            "ZoomSDK directory to build the engine."
        )
    endif()
endif()

# ---------------------------------------------------------------------------
# FeedsEngine (Zoom Meeting SDK host) and its libyuv dependency: Windows only.
# The macOS plugin shell builds the plugin alone; the Mac engine port comes
# later. The block below is unchanged apart from this WIN32 guard.
# ---------------------------------------------------------------------------
if(WIN32)

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
    engine/engine-speaker.cpp
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

endif()  # WIN32 — FeedsEngine

# Unit tests — standalone console exes covering the shared IPC helpers
# (>4KB E2P reader reassembly + string-aware JSON parsing). No libobs/Qt/SDK,
# so they build and run in CI as the "parsing-test target" on the roadmap.
# Windows only: the E2P reader test drives the Win32 named-pipe reader.
option(FEEDS_BUILD_TESTS "Build Feeds unit tests" ON)
if(FEEDS_BUILD_TESTS AND WIN32)
    enable_testing()
    add_subdirectory(tests)
endif()
