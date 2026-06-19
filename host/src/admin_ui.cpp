#include "admin_ui.hpp"

#include <SDL.h>
#include <SDL_opengl.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"
#include "imgui.h"
#include "net_util.hpp"
#include "qrcodegen/qrcodegen.hpp"
#include "session.hpp"
#include "villen/chess/position.hpp"
#include "ws_server.hpp"

namespace villen::admin {
namespace {

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

void drawAdmin(GameServer& game, net::WsServer& ws, const std::string& joinUrl) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("Villen", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    // The default ImGui font is ASCII-only, so keep admin strings ASCII.
    ImGui::TextUnformatted("Villen - host");
    ImGui::Separator();
    ImGui::Spacing();

    const chess::Position& pos = game.position();
    const char* status = chess::statusName(pos.status());
    const char* turn = pos.sideToMove() == chess::Color::White ? "white" : "black";

    ImGui::SeparatorText("Sessions");
    if (ImGui::BeginTable("sessions", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Session");
        ImGui::TableSetupColumn("Status");
        ImGui::TableSetupColumn("Turn");
        ImGui::TableSetupColumn("Seats");
        ImGui::TableHeadersRow();

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(game.sessionName().c_str());
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(status);
        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(turn);
        ImGui::TableSetColumnIndex(3);
        seatCell("W", game.whiteSeatStatus());
        ImGui::SameLine();
        seatCell("B", game.blackSeatStatus());
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Join");
    ImGui::Text("Players open in a browser:");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.5f, 0.7f, 1.0f, 1.0f), "%s", joinUrl.c_str());
    ImGui::Spacing();
    drawQr(joinUrl, 6.0f);

    ImGui::Spacing();
    ImGui::SeparatorText("Admin");
    if (ImGui::Button("New game")) game.reset();

    // Re-issue a seat (DESIGN §13 #1): release a seat — typically one a
    // disconnected player left held — so someone can rejoin it. Disabled when the
    // seat is already open. Gamepad-navigable like every other admin control.
    const bool whiteOpen = std::strcmp(game.whiteSeatStatus(), "open") == 0;
    const bool blackOpen = std::strcmp(game.blackSeatStatus(), "open") == 0;
    ImGui::SameLine();
    ImGui::BeginDisabled(whiteOpen);
    if (ImGui::Button("Free White")) game.freeSeat(chess::Color::White);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(blackOpen);
    if (ImGui::Button("Free Black")) game.freeSeat(chess::Color::Black);
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::Text("live connections: %zu", ws.connectionCount());

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

}  // namespace

bool runAdminLoop(GameServer& game, net::WsServer& ws,
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
    // Gamepad + keyboard navigation: D-pad through the panel, A to act (§8).
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad |
                      ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForOpenGL(win, gl);
    ImGui_ImplOpenGL3_Init("#version 130");

    auto ips = villen::net::localIpv4Addresses();
    std::string host = ips.empty() ? "localhost" : ips[0];
    std::string joinUrl = "http://" + host + ":" + std::to_string(ws.port());

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

        ws.poll(0);  // drain the network on the SAME thread as the UI (§5)

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        drawAdmin(game, ws, joinUrl);
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
