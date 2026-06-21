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
