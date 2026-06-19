cmake_minimum_required(VERSION 3.16)
project(villen LANGUAGES CXX)

# -----------------------------------------------------------------------------
# Villen — a portable chess(-variant) server you carry.
#
# The build is split so the load-bearing pieces can be exercised independently:
#   * villen_engine  — pure game rules, always built, always unit-tested.
#   * villen_host     — the single native binary (WS server + in-process ImGui
#                       admin). Optional, because it pulls in SDL2/OpenGL and
#                       fetches uWebSockets; engine-only CI stays lightweight.
#
# See DESIGN-villen.md for the architecture this layout protects.
# -----------------------------------------------------------------------------

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(VILLEN_BUILD_TESTS "Build the engine unit tests" ON)
option(VILLEN_BUILD_HOST  "Build the native host binary (WS server + ImGui admin)" ON)

add_subdirectory(engine)

if(VILLEN_BUILD_HOST AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/host/CMakeLists.txt")
    add_subdirectory(host)
endif()

if(VILLEN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
