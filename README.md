<p align="center">
  <img src="res/macos_icon.png" width="220" alt="Red Two Plus Icon">
</p>

# Red Two Plus

An enhanced **macOS (Apple Silicon / Metal)** and **Windows (x64, experimental)** edition of **Red Two**, a static recompilation of **Star Wars: Rogue Squadron II – Rogue Leader** (GameCube), built on [gminustwo/red_two](https://github.com/gminustwo/red_two) **v1.4**.

Earlier Red Two Plus enhancements (such as Hold-B-to-Skip, the Video Player fixes, Switch button prompts, automatic Memory Card setup, Audio Commentary pause, front-end mouse navigation, and safepoint audio catch-up) have since been adopted into upstream Red Two v1.4. **Red Two Plus** builds on top of v1.4 with full next-gen controller haptics, front-end & cinema refinements, and prebuilt Apple Silicon macOS releases.

The game's PowerPC code is translated to C++ ahead of time and runs natively via Aurora (Metal on macOS, Direct3D 12 / Vulkan on Windows) and SDL3. **No game data or texture packs are included**: you need your own disc image of the USA release (`GSWE64`, revision 0), for both the prebuilt downloads and your own build.

## Quick Start

1. Download `RedTwo-macOS-Plus-v1.4.1-arm64.zip` (macOS Apple Silicon) or `RedTwo-Windows-Plus-v1.4.1-x64.zip` (Windows x64, experimental) from the [Releases](../../releases) page and unzip it.
2. **First launch (macOS Gatekeeper)**: because the app is ad-hoc signed and not notarized by Apple, macOS will block it on first launch (or say it is "damaged"). Clear the quarantine flag once in Terminal:
   ```bash
   xattr -cr "/path/to/Red Two.app"
   ```
   *(Alternatively, try opening it once, then go to **System Settings → Privacy & Security** and click **Open Anyway**.)*
3. Open `Red Two.app` and select your own USA disc image (`GSWE64` `.rvz`, `.iso`, `.gcm`, or `.wia`) in the launcher — Red Two remembers its path.
4. Optional: copy a Dolphin HD texture pack (`GSWE64` folder) into `~/Library/Application Support/Red Two/textures/` or pick its folder under **F1 → MODS → Texture pack folder**.

## What's Exclusive to Red Two Plus (on top of Red Two v1.4)

- **3-Tier Rumble & PS5 DualSense Haptics** (`F1 → CONTROLS → RUMBLE & HAPTICS`):
  - **Enhanced Dual-Motor Rumble** (Xbox, Switch Pro, PlayStation & standard pads): live flight telemetry drives distinct feedback for laser fire, proton torpedoes/missiles/bombs, shield/hull damage, explosions, targeting computer deploy/stow (cockpit & chase view), and boost/airbrake.
  - **Emulated GameCube Trigger Click (PS5 DualSense)**: uses Sony's official `0x25` Weapon trigger mode (USB & Bluetooth) to recreate a crisp physical **GameCube digital trigger click** at ~80% travel before full boost/airbrake (or progressive throttle/airbrake resistance when clicks are moved to LB/RB), plus Xbox impulse trigger feedback.
  - **PS5 DualSense USB Audio Haptics**: routes 22–110 Hz filtered stereo game audio into the DualSense's left & right voice-coil actuators during flight — and automatically tightens to a 4-pole 75 Hz sub-bass filter while compensating for SFX/BGM ducking when Audio Commentary is speaking.
- **Redesigned Music Hall**: paginated 114-track list with track durations, live elapsed/remaining timers, mouse support, and a glyph-locked progress bar.
- **Art Gallery Captions**: native in-game font title and slide counter for all 27 concept art pieces and renders.
- **Streamlined Main Menu**: *Special Features* and *Rogue Leaders* promoted directly into the Main Menu (*Start*, *Special Features*, *Rogue Leaders*, *Options*, *Quit*).
- **Cinema Letterbox Defaults & Fix**: *Cinematic bars* enabled by default, glyph-locked Video Player / Music Hall progress bars, and Aurora patch `0040` fixing vertically squished `2.10:1` in-mission flyby & turnaround cutscenes (submitted upstream as [PR #65](https://github.com/gminustwo/red_two/pull/65)).
- **Prebuilt macOS `.app` Releases & Custom Icon**: ready-to-run Apple Silicon `.app` bundle (alongside an experimental Windows x64 zip) with a custom weathered app icon.

### Inherited from Upstream Red Two v1.4

Freeze fix after *Strike at the Core* & attract demo, 16 MiB ARAM audio fix, Hoth snow & Kothlis water fixes, `GAME` tab & RetroAchievements browser sign-in, Invert Y axis, real-time console clock, async background disc reading, Hold-B-to-Skip, Video Player fixes, Switch prompts, Audio Commentary pause (`L3` / `C`), front-end mouse navigation, safepoint audio catch-up, automatic Memory Card setup, high internal resolutions (up to 4K), 4x MSAA, 16x AF, widescreen, save states, Dolphin HD texture packs, Gecko/AR codes, cheats, and RetroAchievements.

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
