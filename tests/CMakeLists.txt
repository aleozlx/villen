# Engine unit tests. doctest is a single-header framework fetched at configure
# time; it compiles fast and keeps the test target self-contained.
#
# Keep this pin at >= v2.4.12: earlier tags declare cmake_minimum_required(<3.5),
# which CMake 4.x rejects at configure ("Compatibility with CMake < 3.5 has been
# removed"). v2.5.x raises doctest's own minimum to 3.14, clearing the threshold
# with margin. We only use the stable core macros (TEST_CASE/CHECK/REQUIRE).
include(FetchContent)
FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.5.2
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(doctest)

add_executable(engine_tests engine_tests.cpp)
target_link_libraries(engine_tests PRIVATE villen::engine doctest::doctest)

add_test(NAME engine_tests COMMAND engine_tests)

# Host integration tests: drive the real WsServer + Host + ChessEngine over a
# loopback WebSocket (see integration_tests.cpp). They only make sense when the
# host is being built, so they share its precondition. They compile the host's
# core sources directly *without* VILLEN_ADMIN_UI — so there is no SDL2/OpenGL/
# ImGui dependency and the test builds in any host configuration (the admin UI is
# never exercised here). ChessEngine::drawAdmin() compiles to an empty body when
# that macro is undefined, so the link is clean.
if(VILLEN_BUILD_HOST AND EXISTS "${CMAKE_SOURCE_DIR}/host/CMakeLists.txt")
    find_package(Threads REQUIRED)
    set(VILLEN_HOST_SRC "${CMAKE_SOURCE_DIR}/host/src")
    add_executable(integration_tests
        integration_tests.cpp
        ${VILLEN_HOST_SRC}/ws_server.cpp
        ${VILLEN_HOST_SRC}/net_util.cpp
        ${VILLEN_HOST_SRC}/envelope.cpp
        ${VILLEN_HOST_SRC}/room.cpp
        ${VILLEN_HOST_SRC}/host.cpp
        ${VILLEN_HOST_SRC}/engines/chess/chess_engine.cpp)
    target_include_directories(integration_tests PRIVATE
        ${VILLEN_HOST_SRC} "${CMAKE_SOURCE_DIR}/third_party")
    target_link_libraries(integration_tests PRIVATE
        villen::engine doctest::doctest Threads::Threads)
    add_test(NAME integration_tests COMMAND integration_tests)
endif()
