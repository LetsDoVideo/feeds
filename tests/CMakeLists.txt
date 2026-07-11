# Feeds unit tests. Standalone console executables — no libobs / Qt / Zoom SDK —
# so they configure and run anywhere the compiler is available (including CI
# before the plugin/engine link). Registered with ctest.

add_executable(test_e2p_reader test_e2p_reader.cpp)

# Only needs the shared, header-only IPC helpers.
target_include_directories(test_e2p_reader PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../common"
)

# The reassembly headers use std::move / <utility>; nothing beyond C++17, which
# the top-level toolchain already sets. Win32 named-pipe APIs link from the
# default kernel32 import lib.

add_test(NAME e2p_reader COMMAND test_e2p_reader)
