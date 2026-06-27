# The pure chess engine: no networking, no graphics, no device code.
# Kept as its own target so it can be unit-tested in isolation and so the
# "engine slot" stays swappable for variants/other games later
# (DESIGN-villen.md §9.1).
add_library(villen_engine
    src/types.cpp
    src/move.cpp
    src/position.cpp
)

target_include_directories(villen_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(villen_engine PUBLIC cxx_std_17)
add_library(villen::engine ALIAS villen_engine)

# The pure `filter` morphology reference (DESIGN-filter.md §2): the same pixel
# buffer -> pipeline -> pixel buffer the APU backend accelerates, but in plain
# C++ — no GL, no JPEG, no device code. Built and unit-tested in CI exactly like
# villen_engine; it is the contract the GPU output must match bit-for-bit (§4.3).
add_library(villen_filter
    src/filter/image.cpp
    src/filter/morphology.cpp
    src/filter/pipeline.cpp
    src/filter/presets.cpp
)

target_include_directories(villen_filter PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(villen_filter PUBLIC cxx_std_17)
add_library(villen::filter ALIAS villen_filter)

# The pure chat core (DESIGN-chat.md §2/§4/§13): conversation state + the
# per-model prompt-templating table. No llama, no networking, no GPU — the
# deterministic, CI-tested half of the chat engine, a sibling library to
# villen_engine (the stochastic generation lives behind the inference boundary
# in the host, §3.A). Always built, always testable.
add_library(villen_chat
    src/chat/conversation.cpp
    src/chat/prompt.cpp
    src/chat/sse.cpp
)

target_include_directories(villen_chat PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(villen_chat PUBLIC cxx_std_17)
add_library(villen::chat ALIAS villen_chat)

# The pure `snake` simulation (DESIGN-snake.md §3): grid + snakes + food + a
# deterministic tick, plus the A* AI movers (§7). No SDL, no GL, no sockets — the
# upstream snake2 logic lifted out of its app shell. Always built, always unit-
# tested in headless CI (a fixed seed + input script asserts the exact grid),
# exactly like villen_engine / villen_filter / villen_chat. The lightest engine:
# no GPU, no subprocess, no codec (§9).
add_library(villen_snake
    src/snake/world.cpp
    src/snake/pathfinding.cpp
)

target_include_directories(villen_snake PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(villen_snake PUBLIC cxx_std_17)
add_library(villen::snake ALIAS villen_snake)
