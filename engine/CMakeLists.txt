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
