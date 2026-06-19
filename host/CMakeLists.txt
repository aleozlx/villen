# The native host binary: WebSocket player server + the in-process Dear ImGui
# admin UI — one executable, one process, one thread (DESIGN §2, §5).
#
# The admin UI needs SDL2 + OpenGL and fetches Dear ImGui. If those aren't
# available we still build a fully functional server-only host (run with
# --headless behaviour), so engine/server work never depends on a display.

add_executable(villen
    src/main.cpp
    src/ws_server.cpp
    src/net_util.cpp
    src/protocol.cpp
    src/session.cpp
)
target_include_directories(villen PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(villen PRIVATE villen::engine)
target_compile_features(villen PRIVATE cxx_std_17)

# Dev convenience: serve the browser client straight from the source tree unless
# overridden with --client-dir at runtime.
target_compile_definitions(villen PRIVATE
    VILLEN_DEFAULT_CLIENT_DIR="${CMAKE_SOURCE_DIR}/client")

# --- In-process Dear ImGui admin UI (DESIGN §8, step 7) ----------------------
find_package(SDL2 QUIET)
find_package(OpenGL QUIET)

if(SDL2_FOUND AND OpenGL_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.91.5
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(imgui)

    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    )
    target_include_directories(imgui PUBLIC
        ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
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
