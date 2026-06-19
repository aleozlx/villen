# Throwaway Deck smoke spike (DESIGN §11 step 6) — standalone, NOT part of the
# main villen build. Built on PC, copied to the Deck (no toolchain there).
#
#   cmake -S spike/deck -B build-spike -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-spike
#
# Static libstdc++/libgcc so the PC binary runs against SteamOS's older runtime;
# SDL2 + GL stay dynamic (present on the Deck).
cmake_minimum_required(VERSION 3.16)
project(villen_deck_spike LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(SDL2 REQUIRED)
find_package(OpenGL REQUIRED)

set(IMGUI ${CMAKE_CURRENT_SOURCE_DIR}/imgui)
add_executable(deck_spike
    main.cpp
    ${IMGUI}/imgui.cpp
    ${IMGUI}/imgui_draw.cpp
    ${IMGUI}/imgui_tables.cpp
    ${IMGUI}/imgui_widgets.cpp
    ${IMGUI}/backends/imgui_impl_sdl2.cpp
    ${IMGUI}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(deck_spike PRIVATE ${IMGUI} ${IMGUI}/backends ${SDL2_INCLUDE_DIRS})
target_link_libraries(deck_spike PRIVATE ${SDL2_LIBRARIES} OpenGL::GL ${CMAKE_DL_LIBS})
target_link_options(deck_spike PRIVATE -static-libstdc++ -static-libgcc)
# Pin glibc-2.43-re-versioned math symbols back to the 2.2.5 node so the binary
# loads on the Deck's older glibc (2.41). Force-included into every TU.
target_compile_options(deck_spike PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/glibc_compat.h)
