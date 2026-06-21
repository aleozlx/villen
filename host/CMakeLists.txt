# The native host binary: WebSocket player server + the in-process Dear ImGui
# admin UI — one executable, one process, one thread (DESIGN §2, §5).
#
# The admin UI needs SDL2 + OpenGL and compiles Dear ImGui from the
# third_party/imgui submodule. If those aren't available we still build a fully
# functional server-only host (run with --headless behaviour), so engine/server
# work never depends on a display.

add_executable(villen
    src/main.cpp
    src/ws_server.cpp
    src/net_util.cpp
    src/envelope.cpp
    src/room.cpp
    src/host.cpp
    src/engines/chess/chess_engine.cpp
    src/engines/filter/filter_engine.cpp
    src/engines/filter/stb_impl.cpp
)
# src/ on the include path so engines under src/engines/<name>/ can include the
# framework headers (engine.hpp, room.hpp, …) by their plain names; third_party
# carries the nlohmann + stb single-headers (filter's JPEG codec, DESIGN-filter §12).
target_include_directories(villen PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
# villen::filter is the pure CPU morphology reference the engine runs (and the
# APU backend will later be validated against, §4.3).
target_link_libraries(villen PRIVATE villen::engine villen::filter)
target_compile_features(villen PRIVATE cxx_std_17)

# Dev convenience: serve the browser client straight from the source tree unless
# overridden with --client-dir at runtime.
target_compile_definitions(villen PRIVATE
    VILLEN_DEFAULT_CLIENT_DIR="${CMAKE_SOURCE_DIR}/client")

# --- In-process Dear ImGui admin UI (DESIGN §8, step 7) ----------------------
find_package(SDL2 QUIET)
find_package(OpenGL QUIET)

if(SDL2_FOUND AND OpenGL_FOUND)
    # Dear ImGui from the git submodule at third_party/imgui (DESIGN §8), pinned
    # to v1.91.5 by the recorded gitlink. Compile its core plus the SDL2/OpenGL3
    # backends directly into a small static lib. Submodule over FetchContent so
    # the version pin is an explicit, reviewable gitlink instead of a configure-
    # time network fetch — the Deck never builds, so the only cost is a one-time
    # `git submodule update --init`.
    set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
    if(NOT EXISTS ${IMGUI_DIR}/imgui.cpp)
        message(FATAL_ERROR
            "third_party/imgui is empty — initialise the submodule with:\n"
            "  git submodule update --init third_party/imgui")
    endif()

    add_library(imgui STATIC
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    )
    target_include_directories(imgui PUBLIC
        ${IMGUI_DIR} ${IMGUI_DIR}/backends)
    target_link_libraries(imgui PUBLIC SDL2::SDL2 OpenGL::GL)

    # Vendored QR generator (Nayuki, MIT) for the join-URL panel.
    add_library(qrcodegen STATIC ${CMAKE_SOURCE_DIR}/third_party/qrcodegen/qrcodegen.cpp)
    target_include_directories(qrcodegen PUBLIC ${CMAKE_SOURCE_DIR}/third_party/qrcodegen)

    target_sources(villen PRIVATE src/admin_ui.cpp)
    target_link_libraries(villen PRIVATE imgui qrcodegen SDL2::SDL2 OpenGL::GL)
    target_compile_definitions(villen PRIVATE VILLEN_ADMIN_UI)
else()
    message(STATUS
        "villen: SDL2/OpenGL not found — building server-only host (no admin UI)")
endif()
