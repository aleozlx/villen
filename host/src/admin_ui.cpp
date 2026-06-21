#include "admin_ui.hpp"

#include <SDL.h>
#include <SDL_opengl.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"
#include "host.hpp"
#include "imgui.h"
#include "net_util.hpp"
#include "qrcodegen/qrcodegen.hpp"
#include "room.hpp"
#include "ws_server.hpp"

#ifdef __GLIBC__
#include <gnu/libc-version.h>
#endif

namespace villen::admin {
namespace {

// Monotonic millisecond clock for IEngine::onTick (matches main.cpp's headless
// loop). Real-time engines use only deltas, so the epoch is irrelevant.
std::uint64_t nowMs() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// The shell is a top-level view router in the one loop (admin-shell §2): the
// launcher home, a system-info view, and the active engine's view. "Opens a
// window" means switches the active view — under Gamescope there is one surface.
enum class View { Launcher, SysInfo, Engine };

// Captured once at startup (these don't change for the session) and shown in the
// System Info view — the spike/deck probes promoted into a permanent operator
// panel (admin-shell §7).
struct SysInfo {
    std::string videoDriver, glVersion, glRenderer, glVendor;
    std::vector<std::string> controllers;
    std::vector<std::string> ips;
    std::uint16_t port = 0;
    std::string build;
};

// Render a QR code into the current ImGui window using the draw list — no
// texture upload needed for a panel-sized code.
void drawQr(const std::string& text, float modulePx) {
    using qrcodegen::QrCode;
    QrCode qr = QrCode::encodeText(text.c_str(), QrCode::Ecc::LOW);
    const int n = qr.getSize();
    const int quiet = 2;  // mandatory quiet zone
    const float total = (n + 2 * quiet) * modulePx;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(o, ImVec2(o.x + total, o.y + total), IM_COL32_WHITE);
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            if (!qr.getModule(x, y)) continue;
            ImVec2 p(o.x + (x + quiet) * modulePx, o.y + (y + quiet) * modulePx);
            dl->AddRectFilled(p, ImVec2(p.x + modulePx, p.y + modulePx),
                              IM_COL32_BLACK);
        }
    }
    ImGui::Dummy(ImVec2(total, total));  // reserve layout space for the code
}

// Colour a seat by its lifecycle state: green connected, amber disconnected
// (held across a drop), grey open (DESIGN §13 #1).
void seatCell(const char* label, const char* status) {
    ImVec4 col;
    if (std::strcmp(status, "connected") == 0)
        col = ImVec4(0.5f, 0.86f, 0.5f, 1.0f);
    else if (std::strcmp(status, "disconnected") == 0)
        col = ImVec4(0.95f, 0.75f, 0.35f, 1.0f);
    else
        col = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
    ImGui::TextColored(col, "%s: %s", label, status);
}

// Launcher home (admin-shell §2): pick an engine to construct+start it, or open
// System Info, or quit. With no engine running, the gamepad may drive nav here.
void drawLauncher(Host& host, View& view, bool& done) {
    ImGui::TextUnformatted("Villen - launcher");
    ImGui::Spacing();
    ImGui::SeparatorText("Engines");
    for (std::size_t i = 0; i < host.engineCount(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Button(host.engineName(i), ImVec2(240, 56))) {
            host.startEngine(i);
            view = View::Engine;
        }
        ImGui::PopID();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button("System Info", ImVec2(240, 40))) view = View::SysInfo;
    ImGui::SameLine();
    if (ImGui::Button("Quit", ImVec2(240, 40))) done = true;
}

// System Info (admin-shell §7): renderer sanity (the real radeonsi, not llvmpipe —
// steamdeck-debugging §4), controllers, LAN address, build/glibc.
void drawSysInfo(const SysInfo& s, View& view) {
    ImGui::TextUnformatted("Villen - system info");
    ImGui::Spacing();
    ImGui::SeparatorText("Renderer");
    ImGui::Text("SDL video driver : %s", s.videoDriver.c_str());
    ImGui::Text("GL_VERSION       : %s", s.glVersion.c_str());
    ImGui::Text("GL_RENDERER      : %s", s.glRenderer.c_str());
    ImGui::Text("GL_VENDOR        : %s", s.glVendor.c_str());

    ImGui::SeparatorText("Controllers");
    if (s.controllers.empty())
        ImGui::TextUnformatted("(none detected)");
    for (const std::string& c : s.controllers) ImGui::BulletText("%s", c.c_str());

    ImGui::SeparatorText("Network");
    if (s.ips.empty())
        ImGui::Text("listening on port %u", s.port);
    for (const std::string& ip : s.ips)
        ImGui::Text("http://%s:%u", ip.c_str(), s.port);

    ImGui::SeparatorText("Build");
    ImGui::TextUnformatted(s.build.c_str());

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button("Back", ImVec2(240, 40))) view = View::Launcher;
}

// Engine view: the shared chrome (Home chip, seat roster + Free, join QR,
// connection count — all read from generic membership) wrapping the active
// engine's own drawAdmin() body (admin-shell §8). Home stops the engine and
// returns to the launcher.
void drawEngineView(Host& host, net::WsServer& ws, const std::string& joinUrl,
                    View& view) {
    // The always-present touchscreen Home chip — never needs the gamepad, so it
    // can't clash with an engine that consumes the pad (admin-shell §6).
    if (ImGui::Button("Home", ImVec2(120, 40))) {
        host.stopEngine();
        view = View::Launcher;
        return;  // engine/room are gone now; draw nothing else this frame
    }
    ImGui::SameLine();
    ImGui::Text("engine: %s", host.engineName(host.activeIndex()));
    ImGui::Separator();
    ImGui::Spacing();

    Room* room = host.room();
    ImGui::SeparatorText("Seats");
    if (room && ImGui::BeginTable("seats", 2,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Seat");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        const SeatRoster& roster = room->roster();
        for (SeatId s = 0; s < static_cast<SeatId>(roster.names.size()); ++s) {
            const char* status = room->seatStatus(s);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            seatCell(roster.names[s].c_str(), status);
            ImGui::TableSetColumnIndex(1);
            // Re-issue a seat (DESIGN §13 #1): release it so someone can rejoin.
            // Disabled when already open. ImGui needs unique ids per button.
            const bool open = std::strcmp(status, "open") == 0;
            ImGui::BeginDisabled(open);
            ImGui::PushID(s);
            if (ImGui::Button("Free")) room->freeSeat(s);
            ImGui::PopID();
            ImGui::EndDisabled();
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Engine");
    if (IEngine* engine = host.active()) engine->drawAdmin();

    ImGui::Spacing();
    ImGui::SeparatorText("Join");
    ImGui::Text("Players open in a browser:");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.5f, 0.7f, 1.0f, 1.0f), "%s", joinUrl.c_str());
    ImGui::Spacing();
    drawQr(joinUrl, 6.0f);

    ImGui::Spacing();
    ImGui::Text("live connections: %zu", ws.connectionCount());
}

void drawShell(Host& host, net::WsServer& ws, const std::string& joinUrl,
               const SysInfo& sys, View& view, bool& done) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("Villen", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    // The default ImGui font is ASCII-only, so keep admin strings ASCII.
    // The Engine view requires an active engine; if it vanished, fall to launcher.
    if (view == View::Engine && !host.running()) view = View::Launcher;

    switch (view) {
        case View::Launcher: drawLauncher(host, view, done); break;
        case View::SysInfo: drawSysInfo(sys, view); break;
        case View::Engine: drawEngineView(host, ws, joinUrl, view); break;
    }

    ImGui::End();
}

// Write the current GL framebuffer to a binary PPM (verification artifact only).
void writePpm(const char* path, int w, int h) {
    std::vector<unsigned char> px(static_cast<std::size_t>(w) * h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; --y)  // GL origin is bottom-left; flip to top
        std::fwrite(px.data() + static_cast<std::size_t>(y) * w * 3, 1, w * 3, f);
    std::fclose(f);
}

std::string glString(GLenum e) {
    const char* s = reinterpret_cast<const char*>(glGetString(e));
    return s ? s : "(null)";
}

SysInfo probeSysInfo(net::WsServer& ws) {
    SysInfo s;
    const char* drv = SDL_GetCurrentVideoDriver();
    s.videoDriver = drv ? drv : "(null)";
    s.glVersion = glString(GL_VERSION);
    s.glRenderer = glString(GL_RENDERER);
    s.glVendor = glString(GL_VENDOR);
    int njoy = SDL_NumJoysticks();
    for (int i = 0; i < njoy; ++i) {
        if (!SDL_IsGameController(i)) continue;
        const char* n = SDL_GameControllerNameForIndex(i);
        s.controllers.push_back(n ? n : "(unnamed)");
    }
    s.ips = villen::net::localIpv4Addresses();
    s.port = ws.port();
    s.build = std::string("built: " __DATE__ " " __TIME__) +
#ifdef __VERSION__
              "\ncompiler: " __VERSION__
#endif
              ;  // NOLINT(whitespace/semicolon)
#ifdef __GLIBC__
    s.build += std::string("\nglibc: ") + gnu_get_libc_version();
#endif
    return s;
}

}  // namespace

bool runAdminLoop(Host& host, net::WsServer& ws,
                  const volatile std::sig_atomic_t* running, int screenshotDelayMs,
                  const char* screenshotPath) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "villen: SDL video unavailable (%s)\n", SDL_GetError());
        return false;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_Window* win = SDL_CreateWindow(
        "Villen — host", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 800,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) {
        std::fprintf(stderr, "villen: window creation failed (%s)\n", SDL_GetError());
        SDL_Quit();
        return false;
    }

    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl) {
        std::fprintf(stderr, "villen: GL context failed (%s)\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return false;
    }
    SDL_GL_MakeCurrent(win, gl);
    SDL_GL_SetSwapInterval(1);  // vsync paces the loop ~60 Hz

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // no imgui.ini on the Deck
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForOpenGL(win, gl);
    ImGui_ImplOpenGL3_Init("#version 130");

    const SysInfo sys = probeSysInfo(ws);
    auto ips = villen::net::localIpv4Addresses();
    std::string hostIp = ips.empty() ? "localhost" : ips[0];
    std::string joinUrl = "http://" + hostIp + ":" + std::to_string(ws.port());

    // Boot into the engine if one is already active (--engine / kiosk); otherwise
    // start at the launcher (admin-shell §4).
    View view = host.running() ? View::Engine : View::Launcher;
    bool done = false;
    const Uint32 start = SDL_GetTicks();
    while (!done && (!running || *running)) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) done = true;
            if (e.type == SDL_WINDOWEVENT &&
                e.window.event == SDL_WINDOWEVENT_CLOSE &&
                e.window.windowID == SDL_GetWindowID(win))
                done = true;
        }

        // Gamepad ownership is modal (admin-shell §6): the pad drives ImGui nav in
        // the launcher / system-info views, but the moment an engine is in session
        // it belongs to the engine (snake's players, etc.). The shell stays
        // touch-reachable via the Home chip, which never needs the pad.
        if (view == View::Engine)
            io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
        else
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

        ws.poll(0);     // drain the network on the SAME thread as the UI (§5)
        host.tick(nowMs());  // advance real-time engines (filter, snake) per frame

        // A real-time engine may have made its own GL context current this tick
        // (filter's surfaceless-EGL compute context, §6). Re-bind the admin SDL
        // context before ImGui renders, so the two contexts share one thread
        // cleanly.
        SDL_GL_MakeCurrent(win, gl);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        drawShell(host, ws, joinUrl, sys, view, done);
        ImGui::Render();

        int w, h;
        SDL_GL_GetDrawableSize(win, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.10f, 0.10f, 0.11f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // For verification: capture once the requested settle time has elapsed
        // (long enough for a test client to connect and move).
        if (screenshotPath && screenshotDelayMs >= 0 &&
            static_cast<int>(SDL_GetTicks() - start) >= screenshotDelayMs) {
            writePpm(screenshotPath, w, h);
            done = true;
        }
        SDL_GL_SwapWindow(win);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return true;
}

}  // namespace villen::admin
