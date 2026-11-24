// The game's GX command stream rendered with Aurora (gx_bridge.cpp).
#pragma once
#include <string>

#include "../ctx.h"

namespace rt::gfx {
// window + renderer; registers the GX command sink. disc: the image the game starts with (""
// if none was found); without a usable one the launcher asks for it and disc is its choice.
// discNote: why there is none (shown on the launcher's disc page).
bool init(int argc, char** argv, std::string& disc, const std::string& discNote);
void vblank();                     // once per emulated retrace (keeps the window responsive)
u64 frames();                      // frames presented
std::string stats();               // Aurora's per-frame statistics

// menu.cpp: the settings menu (F1 or the controller's Back/Select button)
void menuEvent(const void* sdlEvent);  // every SDL event (opens/closes the menu)
void menuDraw();                       // inside a frame, before it ends (ImGui)
// menu.cpp: the startup screen while shaders compile and textures prefetch (inside a frame)
void drawShaderProgress(u32 done, u32 total, u64 texDone, u64 texTotal, u64 texBytes);
void applyDisplaySettings();           // resolution, aspect, fullscreen, frame rate -> Aurora/VI
int uberShaderMode();                  // AuroraSetUberShaders' mode: 0 off, 1 hybrid (R2_UBER overrides)
int displayRefreshHz();                // gx_bridge.cpp: the refresh rate of the window's display, rounded (0: unknown)
double retraceHz();                    // gx_bridge.cpp: the retrace rate to play at (rt::frameRateHz)
void menuImGuiInit(const void* windowSize);  // Aurora's ImGui init callback: fonts and style
// menu.cpp: the launcher - the settings menu before the game boots (gx_bridge.cpp runLauncher)
void launcherOpen();
bool launcherFrame();  // one frame, inside an ImGui frame; true: the player started the game
void launcherSetDisc(const std::string& path, const std::string& note);  // the disc found at start (also in game)
std::string launcherDisc();  // the disc chosen (the launcher only starts the game with a usable one)
}  // namespace rt::gfx
