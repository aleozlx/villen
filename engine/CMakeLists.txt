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
