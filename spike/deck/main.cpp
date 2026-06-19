// Villen — Deck smoke spike (THROWAWAY, DESIGN §11 step 6).
//
// Purpose: retire the three Game-Mode-only risks (§11.1) against disposable code,
// BEFORE the real ImGui admin shell (step 7) is written:
//   #1 Gamescope compositing — does an SDL2+GL3 window come up fullscreen/focused?
//   #2 Steam Input → SDL2    — do the Deck's D-pad/buttons reach SDL2 as a gamepad?
//   #4 GL context            — does the GL3 context init cleanly under Gamescope?
//
// It draws a dummy "sessions" table and one gamepad-navigable button, and prints
// what it sees (video driver, GL strings, detected controllers) BOTH on-screen
// (Game Mode has no terminal) and to a logfile we can read over SSH afterwards.
//
// Carries no game/server/network code and no infra details — pure windowing/input.
#include <SDL.h>
#include <SDL_opengl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl2.h"

namespace {
FILE* g_log = nullptr;
template <typename... A>
void logln(const char* fmt, A... a) {
    std::printf(fmt, a...);
    std::printf("\n");
    std::fflush(stdout);
    if (g_log) {
        std::fprintf(g_log, fmt, a...);
        std::fprintf(g_log, "\n");
        std::fflush(g_log);
    }
}
}  // namespace

int main(int argc, char** argv) {
    // Logfile path is overridable so the Steam shortcut can drop it somewhere we
    // can fetch; defaults to the current "Start In" directory.
    std::string logPath = "spike.log";
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--log") == 0 && i + 1 < argc) logPath = argv[++i];
    g_log = std::fopen(logPath.c_str(), "w");
    logln("=== villen deck smoke spike ===");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        logln("FATAL: SDL_Init: %s", SDL_GetError());
        return 1;
    }
    logln("SDL video driver: %s", SDL_GetCurrentVideoDriver());

    // Risk #2: enumerate what SDL sees as game controllers under Steam Input.
    std::string padSummary;
    int njoy = SDL_NumJoysticks();
    int npads = 0;
    for (int i = 0; i < njoy; ++i) {
        if (SDL_IsGameController(i)) {
            const char* n = SDL_GameControllerNameForIndex(i);
            logln("controller[%d]: %s", i, n ? n : "(unnamed)");
            if (!padSummary.empty()) padSummary += ", ";
            padSummary += (n ? n : "(unnamed)");
            ++npads;
        }
    }
    logln("controllers detected: %d", npads);

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    // Risk #1: fullscreen-desktop so Gamescope composites it across the Deck panel.
    auto flags = (SDL_WindowFlags)(SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP |
                                   SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Window* win = SDL_CreateWindow("villen deck spike", SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, 1280, 800, flags);
    if (!win) {
        logln("FATAL: SDL_CreateWindow: %s", SDL_GetError());
        return 1;
    }
    SDL_GLContext gl = SDL_GL_CreateContext(win);  // Risk #4
    if (!gl) {
        logln("FATAL: SDL_GL_CreateContext: %s", SDL_GetError());
        return 1;
    }
    SDL_GL_MakeCurrent(win, gl);
    SDL_GL_SetSwapInterval(1);

    const char* glVer = (const char*)glGetString(GL_VERSION);
    const char* glRen = (const char*)glGetString(GL_RENDERER);
    logln("GL_VERSION: %s", glVer ? glVer : "(null)");
    logln("GL_RENDERER: %s", glRen ? glRen : "(null)");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;   // risk #2: gamepad nav
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(1.6f);  // legible on a 7" handheld
    io.FontGlobalScale = 1.6f;
    ImGui_ImplSDL2_InitForOpenGL(win, gl);
    ImGui_ImplOpenGL3_Init("#version 130");

    logln("window + GL + imgui up; entering loop");
    int buttonPresses = 0;
    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) running = false;
            // Gamepad Start or Back quits (no keyboard in Game Mode).
            if (e.type == SDL_CONTROLLERBUTTONDOWN &&
                (e.cbutton.button == SDL_CONTROLLER_BUTTON_START ||
                 e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK))
                running = false;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_Once);
        ImGui::SetNextWindowSize(ImVec2(900, 640), ImGuiCond_Once);
        ImGui::Begin("Villen Deck smoke spike (throwaway)");
        ImGui::TextWrapped(
            "If you can read this fullscreen and move the highlight with the D-pad, "
            "Gamescope + Steam Input + GL are all good.");
        ImGui::Separator();
        ImGui::Text("SDL video driver : %s", SDL_GetCurrentVideoDriver());
        ImGui::Text("GL_VERSION       : %s", glVer ? glVer : "(null)");
        ImGui::Text("GL_RENDERER      : %s", glRen ? glRen : "(null)");
        ImGui::Text("controllers (%d)  : %s", npads,
                    padSummary.empty() ? "(none seen)" : padSummary.c_str());
        ImGui::Text("ImGui NavActive  : %s", io.NavActive ? "yes" : "no");
        ImGui::Separator();

        // Dummy "sessions" table — same shape the real admin UI will draw (step 7).
        if (ImGui::BeginTable("sessions", 4,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("session");
            ImGui::TableSetupColumn("white");
            ImGui::TableSetupColumn("black");
            ImGui::TableSetupColumn("status");
            ImGui::TableHeadersRow();
            const char* rows[][4] = {
                {"game-1", "connected", "open", "white to move"},
                {"game-2", "connected", "connected", "black to move"},
            };
            for (auto& r : rows) {
                ImGui::TableNextRow();
                for (int c = 0; c < 4; ++c) {
                    ImGui::TableSetColumnIndex(c);
                    ImGui::TextUnformatted(r[c]);
                }
            }
            ImGui::EndTable();
        }
        ImGui::Separator();

        // The one gamepad-navigable button: A (or Enter) should activate it.
        if (ImGui::Button("Press me  (gamepad A)", ImVec2(360, 80))) ++buttonPresses;
        ImGui::SameLine();
        ImGui::Text("presses: %d", buttonPresses);
        ImGui::Spacing();
        if (ImGui::Button("Quit  (or gamepad Start)", ImVec2(360, 80))) running = false;
        ImGui::End();

        ImGui::Render();
        int w, h;
        SDL_GL_GetDrawableSize(win, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(win);
    }

    logln("clean exit after %d button press(es)", buttonPresses);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    if (g_log) std::fclose(g_log);
    return 0;
}
