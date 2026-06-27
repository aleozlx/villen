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

# The pure `filter` morphology reference (DESIGN-filter.md §13 step 1): same GPU-
# free, always-on CI discipline as the chess engine tests above.
add_executable(filter_tests filter_tests.cpp)
target_link_libraries(filter_tests PRIVATE villen::filter doctest::doctest)

add_test(NAME filter_tests COMMAND filter_tests)

# The chat pure core (DESIGN-chat.md §14 step 1): conversation state +
# per-model templating, asserted with exact rendered prompts and stop tokens —
# the deterministic part, run in headless LLM-free/GPU-free CI (§13, acceptance §1).
add_executable(chat_tests chat_tests.cpp)
target_link_libraries(chat_tests PRIVATE villen::chat doctest::doctest)

add_test(NAME chat_tests COMMAND chat_tests)

# The pure `snake` simulation (DESIGN-snake.md §3, §10 acceptance #1): a fixed
# input script over a fixed seed asserts the exact grid — same GPU-free, always-on
# CI discipline as the engine tests above.
add_executable(snake_tests snake_tests.cpp)
target_link_libraries(snake_tests PRIVATE villen::snake doctest::doctest)

add_test(NAME snake_tests COMMAND snake_tests)

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

    # Snake host integration: the real WsServer + Host + SnakeEngine over a
    # loopback WebSocket (DESIGN-snake §10 acceptance #3) — the authoritative
    # clock, seat<->snake mapping, server-side steering, and a shared arena that
    # the pure-engine tests can't exercise. Same SDL2/ImGui-free host build as
    # integration_tests (drawAdmin compiles to an empty body without the macro).
    add_executable(snake_integration_tests
        snake_integration_tests.cpp
        ${VILLEN_HOST_SRC}/ws_server.cpp
        ${VILLEN_HOST_SRC}/net_util.cpp
        ${VILLEN_HOST_SRC}/envelope.cpp
        ${VILLEN_HOST_SRC}/room.cpp
        ${VILLEN_HOST_SRC}/host.cpp
        ${VILLEN_HOST_SRC}/engines/snake/snake_engine.cpp)
    target_include_directories(snake_integration_tests PRIVATE
        ${VILLEN_HOST_SRC} "${CMAKE_SOURCE_DIR}/third_party")
    target_link_libraries(snake_integration_tests PRIVATE
        villen::snake doctest::doctest Threads::Threads)
    add_test(NAME snake_integration_tests COMMAND snake_integration_tests)

    # Chat end-to-end harness (DESIGN-chat.md §18): drives the REAL villen binary in
    # spawn mode against the LLM-free fake llama-server over a player WebSocket — the
    # §3.A transport (subprocess spawn + /health gate, SSE streaming, crash-restart,
    # malformed-payload rejection) that the C++ unit tests can't reach. It needs the
    # built `villen` target, a Python 3 interpreter, and the fake server; if any is
    # missing the test is simply not registered (CI has all three). LLM-free and
    # GPU-free, so it stays in the §13 "CI stays LLM-free" discipline.
    find_package(Python3 COMPONENTS Interpreter QUIET)
    set(VILLEN_FAKE_LLAMA "${CMAKE_SOURCE_DIR}/spike/chat-bench/fake_llama_server.py")
    if(Python3_Interpreter_FOUND AND EXISTS "${VILLEN_FAKE_LLAMA}")
        add_test(NAME chat_e2e COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/chat_e2e.py"
            --villen $<TARGET_FILE:villen>
            --fake-llama "${VILLEN_FAKE_LLAMA}")
        # Spawn + /health + crash-restart respawn (1s backoff) take real wall-clock;
        # bound it generously so a slow CI runner doesn't false-fail.
        set_tests_properties(chat_e2e PROPERTIES TIMEOUT 120)
    else()
        message(STATUS
            "villen: skipping chat_e2e test (need Python3 + spike/chat-bench/fake_llama_server.py)")
    endif()
endif()
