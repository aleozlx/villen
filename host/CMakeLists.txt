# The native host binary: WebSocket player server + (from step 7) the in-process
# Dear ImGui admin UI — one executable, one process (DESIGN §2).
#
# Step 2 builds the server and main loop only; SDL2/OpenGL/ImGui are added with
# the admin shell in step 7. JSON is a vendored single header (third_party/).
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
