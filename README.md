<p align="center">
  <img src="res/macos_icon.png" width="220" alt="Red Two Plus Icon">
</p>

# Red Two Plus

An enhanced **macOS (Apple Silicon / Metal)** and **Windows (x64, experimental)** edition of **Red Two**, a static recompilation of **Star Wars: Rogue Squadron II – Rogue Leader** (GameCube), built on [gminustwo/red_two](https://github.com/gminustwo/red_two) **v1.3** with additional UI, audio, cinema framing, and quality-of-life enhancements.

The game's PowerPC code is translated to C++ ahead of time and runs natively via Aurora (Metal on macOS, Direct3D 12 / Vulkan on Windows) and SDL3. **No game data or texture packs are included**: you need your own disc image of the USA release (`GSWE64`, revision 0), for both the prebuilt downloads and your own build.

## Quick Start

1. Download `RedTwo-macOS-Plus-v1.3.2-arm64.zip` (macOS Apple Silicon) or `RedTwo-Windows-Plus-v1.3.2-x64.zip` (Windows x64, experimental) from the [Releases](../../releases) page and unzip it.
2. **First launch (macOS Gatekeeper)**: because the app is ad-hoc signed and not notarized by Apple, macOS will block it on first launch (or say it is "damaged"). Clear the quarantine flag once in Terminal:
   ```bash
   xattr -cr "/path/to/Red Two.app"
   ```
   *(Alternatively, try opening it once, then go to **System Settings → Privacy & Security** and click **Open Anyway**.)*
3. Open `Red Two.app` and select your own USA disc image (`GSWE64` `.rvz`, `.iso`, `.gcm`, or `.wia`) in the launcher — Red Two remembers its path.
4. Optional: copy a Dolphin HD texture pack (`GSWE64` folder) into `~/Library/Application Support/Red Two/textures/` or pick its folder under **F1 → MODS → Texture pack folder**.

## Red Two Plus Enhancements (on top of Red Two v1.3)

- **Native Apple Silicon (ARM64 / Metal) & Experimental Windows (x64 / D3D12 & Vulkan)** builds via Aurora + SDL3, with custom weathered app icon and macOS Game Mode support.
- **Authentic Cinema Letterbox Framing**: cutscenes, intro, crawl, and credits keep their native cinema aspect ratio with top/bottom letterbox bars and no off-screen side pop-ins across all widescreen resolutions, while *Select Mission*, *Select Craft*, and front-end menus stay in clean unstretched widescreen.
- **Enhanced Video Player** (*Making Of* & *Teaser*): fixed high-FPS (90/120/144 Hz) playback speed, 4:3 aspect lock, ±5s seeking (`Left`/`Right`), auto-hiding progress bar with locked timestamps, mouse support, and **native in-game font UI** for the pause menu.
- **Redesigned Music Hall**: clean paginated track list for all 114 tracks with track durations and live elapsed/remaining playback timers.
- **Audio Commentary Controls & Overlay**: press **L3** or **C** to pause/resume active commentary streams during gameplay, with game-styled HUD typography.
- **Art Gallery Captions**: native in-game font title and slide counter for all 27 concept art pieces and renders.
- **Nintendo Switch Button Prompts & Select Craft C-Stick Fix**: adds dedicated **Nintendo Switch** (Pro Controller / Joy-Con) auto-detection and on-screen button prompts (`Switch`, alongside `Xbox`, `PlayStation`, and `GameCube`), plus fixes the previously unreplaced GameCube C-stick icon on the *Select Craft* screen with a clean generic Right Analog Stick icon.
- **Streamlined Menu & Mouse Navigation**: *Special Features* and *Rogue Leaders* promoted directly into the Main Menu (*Start*, *Special Features*, *Rogue Leaders*, *Options* with *Passcodes*, *Quit*), with mouse hover/click support across front-end menus.
- **Faster Boot & Cutscene Skip**: eliminated multi-second black-screen delays before the 3D intro with *Fast loading*, extended *Skip intro logos* to cover initial splash screens, and game-styled `HOLD TO SKIP` prompt.
- **Smooth Cutscene & Credits Audio**: immediate safepoint AI/MusyX catch-up prevents choppy/stuttering music both with and without V-Sync (including on ProMotion displays).
- **Automatic Memory Card Setup**: formats the virtual Memory Card and creates the initial save file on first launch without prompting.
- **All upstream Red Two v1.3 features**: Hoth snow & Kothlis water texture fix, real-time console clock (Tatooine & Imperial Academy Heist day/night), async background disc reading with read-ahead, smooth stick response curve & per-stick settings, V-Sync, Discord Rich Presence, starter shader list, high internal resolutions (up to 4K), 4x MSAA, 16x anisotropic filtering, widescreen, MusyX audio DSP emulation, save states, Dolphin HD texture packs with diagnostics, Gecko/AR codes, cheats, passcode unlocks, and RetroAchievements.

### Feature Overview Video

[![Red Two Plus Feature Overview](https://img.youtube.com/vi/_wnlr6NRlJg/hqdefault.jpg)](https://youtu.be/_wnlr6NRlJg)

## Controls & Files

- **In-game menu**: press **F1** *(or **Fn + F1** on Mac keyboards)* or **Back / Select / Minus** on your controller.
- **Fullscreen**: toggle with **Fn + F**, **Option + Return**, or **F11** *(or in the Display menu)*.
- **Save states**: **Back + RB** saves, **Back + LB** loads, **Back + D-pad Left/Right** switches slots (keyboard: **F5 / F8 / F6 / F7**).
- **User files**: settings, memory card (`saves/GSWE64_A.raw`), save states, logs, shader cache, and texture packs live in `~/Library/Application Support/Red Two/`.

## Building on macOS (Apple Silicon)

Requirements: Xcode Command Line Tools (`clang`), Homebrew (`cmake`, `ninja`, `sdl3`, `libpng`, `freetype`, `zstd`, `xxhash`, `fmt`, `abseil`).

```bash
# 1. Build: recompiles the game straight from your disc image (RVZ, WIA, ISO or GCM)
tools/build_macos.sh --disc "<your Rogue Leader.rvz>"

# 2. Run
build/macos/red_two --disc "<your Rogue Leader.rvz>"
```

## Credits

Based on [gminustwo/red_two](https://github.com/gminustwo/red_two) and [Aurora](https://github.com/dolphin-emu/aurora). See the upstream repository for technical details on the static recompilation architecture and Windows/Linux builds.

The macOS port and quality-of-life enhancements in this fork were developed with the assistance of Google Gemini.

## Legal

Red Two is released under the [MIT License](LICENSE). It covers this repository's own code
only, not the game's code that a build (and the prebuilt downloads) contain in recompiled form.

This repository contains no code or assets from the game. Star Wars and Rogue Leader are
trademarks of Lucasfilm Ltd.; GameCube is a trademark of Nintendo. This project is not
affiliated with or endorsed by them.
