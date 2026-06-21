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

# The chat pure core (DESIGN-chat.md §14 step 1): conversation state +
# per-model templating, asserted with exact rendered prompts and stop tokens —
# the deterministic part, run in headless LLM-free/GPU-free CI (§13, acceptance §1).
add_executable(chat_tests chat_tests.cpp)
target_link_libraries(chat_tests PRIVATE villen::chat doctest::doctest)

add_test(NAME chat_tests COMMAND chat_tests)
