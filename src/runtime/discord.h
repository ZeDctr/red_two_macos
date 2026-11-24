#pragma once
// Discord Rich Presence (discord.cpp): the player's Discord profile shows that they are playing
// Red Two, the mission and what they are doing in it ("Battle of Hoth", "Flying"), and for how
// long. Only the Discord app on this computer is talked to (its local IPC endpoint), and only
// while the setting discord_presence is on. Not on Android, nor in automated runs (R2_DISCORD=1
// allows it there).
#include <string>

namespace rt::discord {

// At start, after the settings: the worker thread that keeps the connection (none without an
// application ID in the build, on Android or in automated runs)
void init();

// The launcher is open (gx_bridge.cpp runLauncher): "In the launcher"
void setLauncher(bool open);

// What the game is doing, every frame (hle_game.cpp GameFrameDelta; cheap: atomics)
// FrontEnd: the boot screen, the title and menus, the attract demo; None: neither (loading,
// a mission's results)
enum class Phase : int { None = 0, FrontEnd, Flying, Paused, Cutscene };
void gameFrame(Phase phase);

// The level resource the game reads from the disc (main.cpp discNoteRead: lNN of lvlPackNN.dat):
// 1-17 missions, 18-21 the hangars, 22 the front end
void setLevel(int level);

// The mission a level resource holds ("Battle of Hoth"); "" when it is not one or not known
const char* missionName(int level);

// Red Two's Discord server's invite (CMake R2_DISCORD_INVITE; "" none): the menu's Join button
const char* invite();

// For the menu: what the presence is doing now ("Shown on your Discord profile", "Discord is not
// running on this PC", ...)
std::string statusText();

}  // namespace rt::discord
