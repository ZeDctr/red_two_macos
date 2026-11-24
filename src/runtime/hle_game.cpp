// Game-side hooks (hooks.txt) for the port's options: frame rate, skips, cheats (settings.h).
#include "funcs.h"
#include "runtime.h"
#include "discord.h"
#include "gyro.h"
#include "savestate.h"
#include "settings.h"
#include "hw/gx_fifo.h"
#include "../common/disc_image.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace rt {

// GameFrameDelta (0x7FD46244): the seconds the frame advances the game by, stored at 0x7FDE8288
// by the per-frame update (0x7FC02CF0) of every main loop. The game computes
// 1 / (59.94 - lost) with `lost` the frames per second it measured missing (0x7FE0C6A4,
// from its retrace callback 0x7FD46A10, capped at 29.97): 1/59.94 s when it keeps up. At a
// higher retrace rate (settings fps) it would keep stepping 1/59.94 s per frame and run fast,
// so there the delta is the real time since the previous frame.
static void frameDelta(Ctx& c) {
    f_7FD46244_orig(c);
    const double hz = viRate();
    if (hz < 60.5) return;  // 59.94 Hz: the game's own measure
    // Real time rather than retraces: those can fire late when the host is busy, which would
    // run the game a few percent slow. Capped like the game's own measure (1/29.97 s), e.g.
    // after a load; no real lower bound, or short frames (two updates in one, as loading
    // screens' 0x7FC03BC4 can do) would add time and run the game fast.
    static u64 lastTb = 0;
    const u64 tb = tbNow();
    const double dt = lastTb ? double(tb - lastTb) / double(kTbClock) : 1.0 / hz;
    lastTb = tb;
    c.f[1] = c.ps1[1] = rsingle(std::clamp(dt, 0.00025, 1.0 / 29.97));
}

// MovieUpdate (0x7FD8300C): advances a Factor 5 .movie player by dt (f1). Cutscene and level
// movies pass GameFrameDelta (0x7FDE8288), but the documentary / teaser player (0x7FD71564)
// passes a hardcoded 1/60 s constant, so at retrace rates above 59.94 Hz the video runs fast
// and outruns its audio. Replace that fixed step with real elapsed time, and allow +/-5 s
// seeking (Left/Right) + an on-screen progress bar and timestamp overlay.
namespace {
struct MovieIndex {
    std::mutex mu;
    u32 discStart = 0;
    bool building = false;
    bool ready = false;
    std::vector<std::pair<u32, u32>> frames;  // (fileOffset, frameSize) per frame
} s_movieIdx;

struct MovieOverlayState {
    std::mutex mu;
    u64 lastActiveTb = 0;
    bool paused = false;
    int pauseSel = 0;
    double curSec = 0.0;
    double totalSec = 0.0;
    u64 lastActionTb = 0;
} s_movieOverlay;

static bool s_moviePaused = false;
static std::atomic<int> s_movieMouseHover{-1};
static std::atomic<bool> s_movieMouseClick{false};
static std::atomic<u64> s_fullscreenMovieTb{0};

void ensureMovieIndex(u32 discStart, u32 totalFrames, u32 firstOff, u32 firstSize) {
    {
        std::lock_guard lk(s_movieIdx.mu);
        if (s_movieIdx.discStart == discStart && (s_movieIdx.ready || s_movieIdx.building)) return;
        s_movieIdx.discStart = discStart;
        s_movieIdx.building = true;
        s_movieIdx.ready = false;
        s_movieIdx.frames.clear();
    }
    const std::string path = discPath();
    std::thread([path, discStart, totalFrames, firstOff, firstSize] {
        std::vector<std::pair<u32, u32>> idx;
        std::string err;
        auto img = disc::open(path, err);
        if (img && totalFrames > 0 && totalFrames < 1000000) {
            idx.reserve(totalFrames);
            u32 off = firstOff, sz = firstSize;
            for (u32 i = 0; i < totalFrames && sz >= 32; i++) {
                idx.emplace_back(off, sz);
                u8 hdr[4];
                if (!img->read(u64(discStart) + u64(off), hdr, 4)) break;
                const u32 nextSz = u32(hdr[0]) << 24 | u32(hdr[1]) << 16 | u32(hdr[2]) << 8 | hdr[3];
                off += sz;
                sz = nextSz;
            }
        }
        std::lock_guard lk(s_movieIdx.mu);
        if (s_movieIdx.discStart == discStart) {
            s_movieIdx.frames = std::move(idx);
            s_movieIdx.ready = !s_movieIdx.frames.empty();
            s_movieIdx.building = false;
        }
    }).detach();
}
}  // namespace

bool movieFullscreenActive() {
    const u64 t = s_fullscreenMovieTb.load(std::memory_order_relaxed);
    return t != 0 && (tbNow() - t) < kTbClock / 4;
}

MovieOverlay movieOverlay() {
    std::lock_guard lk(s_movieOverlay.mu);
    MovieOverlay out;
    const u64 now = tbNow();
    if (s_movieOverlay.lastActiveTb && now - s_movieOverlay.lastActiveTb < kTbClock / 4) {
        out.active = true;
        out.paused = s_movieOverlay.paused;
        out.pauseSel = s_movieOverlay.pauseSel;
        out.curSec = s_movieOverlay.curSec;
        out.totalSec = s_movieOverlay.totalSec;
        out.lastActionTb = s_movieOverlay.lastActionTb;
    }
    return out;
}

// RAII guest stack frame for HLE hooks that call recompiled guest functions.
// Never pass a local copy of Ctx to a guest function (a safepoint/interrupt inside poll() saves
// and resumes the fiber's actual Ctx); instead allocate a 32-byte guest frame on c.r[1] so the
// callee's `stw r0, 4(r1)` does not overwrite the caller's saved LR.
struct GuestFrame {
    Ctx& c;
    u32 sp, lr;
    explicit GuestFrame(Ctx& c) : c(c), sp(c.r[1]), lr(c.lr) {
        wr32(sp - 0x20u, sp);
        c.r[1] = sp - 0x20u;
    }
    ~GuestFrame() {
        c.r[1] = sp;
        c.lr = lr;
    }
};

static u32 s_movieMenuObj = 0;
static bool s_movieShowBar = false;

// Native in-game font UI for the documentary/teaser movie player (pause menu + progress timestamps).
// Uses slots 10..14 of menuObj so no ImGui text or launcher boxes are shown in-game.
static void movieUpdateNativeUi(Ctx& c, u32 menuObj, bool paused, int pauseSel, bool showBar, double curSec, double totalSec) {
    if (menuObj < 0x7FC00000u || menuObj >= 0x81800000u) return;
    static u32 s_initMenuObj = 0;
    static u8 s_initPage = 0xFFu;
    static int s_lastEl = -1, s_lastRem = -1, s_lastSel = -1;
    static bool s_lastPaused = false, s_lastBar = false;

    const u8 page = rd8(menuObj + 0x3Eu);
    const u32 slotBase = rd32(menuObj + 0x3B80u);
    if (slotBase < 0x7FC00000u) return;
    s_movieMenuObj = menuObj;
    s_movieShowBar = showBar;

    const u32 savedR3 = c.r[3];
    const double savedF1 = c.f[1];
    const double savedPs1 = c.ps1[1];
    GuestFrame frame(c);
    auto writeStr = [](u32 addr, const char* s) {
        u32 k = 0;
        do { wr8(addr + k, u8(s[k])); } while (s[k++] != '\0');
    };
    static constexpr u32 kMvPauseAddr = 0x8F000400u;
    static constexpr u32 kMvContAddr  = 0x8F000410u;
    static constexpr u32 kMvBackAddr  = 0x8F000428u;
    static constexpr u32 kMvElAddr    = 0x8F000440u;
    static constexpr u32 kMvRemAddr   = 0x8F000458u;

    const bool needInit = (s_initMenuObj != menuObj || s_initPage != page || rd8(menuObj + 0x3BA4u) < 15u);
    if (needInit) {
        s_initMenuObj = menuObj;
        s_initPage = page;
        s_lastEl = s_lastRem = s_lastSel = -1;
        s_lastPaused = !paused;
        s_lastBar = !showBar;

        writeStr(kMvPauseAddr, "PAUSED");
        writeStr(kMvContAddr, "CONTINUE");
        writeStr(kMvBackAddr, "BACK");
        writeStr(kMvElAddr, "0:00");
        writeStr(kMvRemAddr, "-0:00");

        c.r[3] = 0x7FDE3428u;
        f_7FD58DD8(c);
        const u32 tmplStr = c.r[3];

        extern void f_7FD3E9D4(Ctx& c);
        auto initSlot = [&](u32 idx, double x, double y, double scale, u32 strAddr, u8 r, u8 g, u8 b) {
            const u32 slot = slotBase + idx * 108u;
            c.r[3] = tmplStr;
            c.r[4] = slot;
            c.r[5] = 0u;
            c.f[1] = c.ps1[1] = rsingle(x);
            c.f[2] = c.ps1[2] = rsingle(y);
            f_7FD58EF0(c);
            wr32(rd32(menuObj + 0x4508u) + idx * 4u, 0xFFFFFFFFu);
            c.r[3] = slot;
            c.r[4] = strAddr;
            f_7FD3E418(c);
            c.r[3] = slot;
            c.r[4] = 2u;  // centered
            f_7FD3E9C0(c);
            c.r[3] = slot;
            c.f[1] = c.ps1[1] = rsingle(scale);
            f_7FD3EA1C(c);
            c.r[3] = slot;
            c.r[4] = 1u;  // enable native drop shadow (same as in-game pause menu)
            c.f[1] = c.ps1[1] = rsingle(2.0);
            c.f[2] = c.ps1[2] = rsingle(2.0);
            c.f[3] = c.ps1[3] = rsingle(0.75);
            f_7FD3E9D4(c);
            c.r[3] = slot;
            c.r[4] = 0u;
            c.r[5] = r;
            c.r[6] = g;
            c.r[7] = b;
            c.r[8] = 0xFFu;
            f_7FD3EA00(c);
            c.r[3] = slot;
            c.r[4] = 0u;
            f_7FD3E9B8(c);
        };

        initSlot(10u, 320.0, 178.0, 0.85, kMvPauseAddr, 0xFFu, 0xE0u, 0x60u);
        initSlot(11u, 320.0, 230.0, 0.62, kMvContAddr,  0xFFu, 0xFFu, 0xFFu);
        initSlot(12u, 320.0, 264.0, 0.62, kMvBackAddr,  0xFFu, 0xFFu, 0xFFu);
        initSlot(13u, 165.0, 418.0, 0.50, kMvElAddr,    0xFFu, 0xFFu, 0xFFu);
        initSlot(14u, 468.0, 418.0, 0.50, kMvRemAddr,   0xFFu, 0xFFu, 0xFFu);

        if (rd8(menuObj + 0x3BA4u) < 15u) {
            wr8(menuObj + 0x3BA4u, 15u);
            wr8(menuObj + 0x3BA5u + page, 15u);
        }
    }

    if (paused != s_lastPaused) {
        s_lastPaused = paused;
        s_lastSel = -1;
        for (u32 idx = 10u; idx <= 12u; idx++) {
            c.r[3] = slotBase + idx * 108u;
            c.r[4] = paused ? 1u : 0u;
            f_7FD3E9B8(c);
        }
    }
    if (paused) {
        // Smooth yellow <-> red pulsing glow on the selected item, matching the in-game pause menu.
        const double t = 0.5 + 0.5 * std::sin((double(tbNow()) / double(kTbClock)) * 5.2);
        const u8 selR = 0xFFu;
        const u8 selG = u8(std::lround(42.0 + t * (232.0 - 42.0)));
        const u8 selB = u8(std::lround(20.0 + t * (48.0 - 20.0)));
        for (int i = 0; i < 2; i++) {
            const u32 slot = slotBase + u32(11 + i) * 108u;
            c.r[3] = slot;
            c.r[4] = 0u;
            if (i == pauseSel) {
                c.r[5] = selR; c.r[6] = selG; c.r[7] = selB; c.r[8] = 0xFFu;
            } else if (pauseSel != s_lastSel) {
                c.r[5] = 0xFFu; c.r[6] = 0xFFu; c.r[7] = 0xFFu; c.r[8] = 0xFFu;
            } else {
                continue;
            }
            f_7FD3EA00(c);
        }
        s_lastSel = pauseSel;
    }
    if (showBar != s_lastBar) {
        s_lastBar = showBar;
        for (u32 idx = 13u; idx <= 14u; idx++) {
            c.r[3] = slotBase + idx * 108u;
            c.r[4] = showBar ? 1u : 0u;
            f_7FD3E9B8(c);
        }
    }
    if (showBar) {
        const double el = (totalSec > 0.0) ? std::clamp(curSec, 0.0, totalSec) : std::max(0.0, curSec);
        const double rem = (totalSec > 0.0) ? std::max(0.0, totalSec - el) : 0.0;
        const int elSec = int(el + 0.5);
        const int remSec = int(rem + 0.5);
        if (elSec != s_lastEl) {
            s_lastEl = elSec;
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d:%02d", elSec / 60, elSec % 60);
            writeStr(kMvElAddr, buf);
            c.r[3] = slotBase + 13u * 108u;
            c.r[4] = kMvElAddr;
            f_7FD3E418(c);
        }
        if (remSec != s_lastRem) {
            s_lastRem = remSec;
            char buf[16];
            std::snprintf(buf, sizeof(buf), "-%d:%02d", remSec / 60, remSec % 60);
            writeStr(kMvRemAddr, buf);
            c.r[3] = slotBase + 14u * 108u;
            c.r[4] = kMvRemAddr;
            f_7FD3E418(c);
        }
    }
    c.r[3] = savedR3;
    c.f[1] = savedF1;
    c.ps1[1] = savedPs1;
}

void hle_MovieUpdate(Ctx& c) {
    // Fullscreen movie player (documentary / teaser at 0x7FD71568). Note: 0x7FD86250 is the 3D
    // hologram/quad movie player (e.g. SELECT MISSION briefing carousel), so it keeps widescreen.
    if (c.lr == 0x7FD71568u) {
        const u32 p = c.r[3];
        const u32 hdr = p ? rd32(p + 0x70u) : 0;
        const bool exiting = rd8(c.r[13] + 0xFFFFB590u) != 0;
        if (hdr && rd8(p + 0xE9u) == 0 && !exiting) {
            s_fullscreenMovieTb.store(tbNow(), std::memory_order_relaxed);
            const double fps = s2d(rd32(hdr + 0x04u));
            const u32 totalFrames = rd32(hdr + 0x0Cu);
            const u32 curFrame = rd32(p + 0xE4u);
            const u32 discStart = rd32(p + 0x38u);
            const u32 firstSize = rd32(hdr + 0x14u);
            const u32 firstOff = rd32(hdr + 0x18u);
            const u64 now = tbNow();

            static u32 s_lastPlayer = 0;
            static int s_pendingFrames = 0;
            static int s_lastDir = 0;
            static u64 s_nextRepeatTb = 0;
            static int s_pauseSel = 0;
            static int s_lastVert = 0;
            static bool s_startWas = false;
            static u64 s_lastDtTb = 0;

            if (p != s_lastPlayer) {
                s_lastPlayer = p;
                s_moviePaused = false;
                s_pauseSel = 0;
                s_pendingFrames = 0;
                s_lastDir = 0;
                s_lastVert = 0;
                s_lastDtTb = 0;
                std::lock_guard lk(s_movieOverlay.mu);
                s_movieOverlay.paused = false;
                s_movieOverlay.pauseSel = 0;
                s_movieOverlay.lastActionTb = 0;
            }

            if (discStart && totalFrames > 0 && fps > 1.0) {
                ensureMovieIndex(discStart, totalFrames, firstOff, firstSize);

                // Intercept the game's exit button bits (r31 + 0x6699, bits 0x10 / 0x20) so a
                // button press opens our pause menu instead of immediately aborting playback.
                const u32 menuObj = c.r[31];
                const u8 rawBtn = menuObj ? rd8(menuObj + 0x6699u) : 0;
                if (menuObj) wr8(menuObj + 0x6699u, rawBtn & ~0x30u);

                bool confirmPressed = (rawBtn & 0x30u) != 0;
                const PadState pad = padState(0);
                const bool startNow = !gfx::menuOpen() && (pad.buttons & 0x1000u) != 0;
                if (startNow && !s_startWas) confirmPressed = true;
                s_startWas = startNow;

                if (gfx::menuOpen()) confirmPressed = false;

                if (!s_moviePaused && confirmPressed) {
                    s_moviePaused = true;
                    s_pauseSel = 0;  // "Continue" selected by default
                    setAudioSpeed(1.0, true);
                    const double curSec = double(curFrame) / fps;
                    const double totalSec = double(totalFrames) / fps;
                    {
                        std::lock_guard lk(s_movieOverlay.mu);
                        s_movieOverlay.lastActiveTb = now;
                        s_movieOverlay.paused = true;
                        s_movieOverlay.pauseSel = 0;
                        s_movieOverlay.curSec = curSec;
                        s_movieOverlay.totalSec = totalSec;
                        s_movieOverlay.lastActionTb = now;
                    }
                    if (menuObj) movieUpdateNativeUi(c, menuObj, true, 0, true, curSec, totalSec);
                    return;
                }

                if (s_moviePaused) {
                    if (!gfx::menuOpen()) {
                        const bool up = (pad.buttons & 0x0008u) != 0 || pad.stickY > 188;
                        const bool down = (pad.buttons & 0x0004u) != 0 || pad.stickY < 68;
                        const int vert = down ? 1 : up ? -1 : 0;
                        if (vert != 0 && vert != s_lastVert) {
                            s_pauseSel = (vert > 0) ? 1 : 0;
                        }
                        s_lastVert = vert;

                        const int mHover = s_movieMouseHover.exchange(-1, std::memory_order_relaxed);
                        if (mHover >= 0) s_pauseSel = mHover;
                        if (s_movieMouseClick.exchange(false, std::memory_order_relaxed)) confirmPressed = true;

                        if (confirmPressed) {
                            if (s_pauseSel == 0) {
                                // Fortsetzen
                                s_moviePaused = false;
                                s_lastDtTb = now;
                                setAudioSpeed(1.0, false);
                                std::lock_guard lk(s_movieOverlay.mu);
                                s_movieOverlay.paused = false;
                                s_movieOverlay.lastActionTb = now;
                            } else {
                                // End: hand the exit bit back to the game's native exit path
                                s_moviePaused = false;
                                s_movieShowBar = false;
                                setAudioSpeed(1.0, false);
                                if (menuObj) {
                                    movieUpdateNativeUi(c, menuObj, false, 0, false, 0.0, 0.0);
                                    wr8(menuObj + 0x6699u, rawBtn | 0x20u);
                                }
                                s_movieMenuObj = 0;
                                {
                                    std::lock_guard lk(s_movieOverlay.mu);
                                    s_movieOverlay.paused = false;
                                    s_movieOverlay.lastActiveTb = 0;
                                }
                                s_fullscreenMovieTb.store(0, std::memory_order_relaxed);
                                f_7FD8300C_orig(c);
                                return;
                            }
                        }
                    }
                    if (s_moviePaused) {
                        setAudioSpeed(1.0, true);
                        const double curSec = double(curFrame) / fps;
                        const double totalSec = double(totalFrames) / fps;
                        {
                            std::lock_guard lk(s_movieOverlay.mu);
                            s_movieOverlay.lastActiveTb = now;
                            s_movieOverlay.paused = true;
                            s_movieOverlay.pauseSel = s_pauseSel;
                            s_movieOverlay.curSec = curSec;
                            s_movieOverlay.totalSec = totalSec;
                        }
                        if (menuObj) movieUpdateNativeUi(c, menuObj, true, s_pauseSel, true, curSec, totalSec);
                        return;
                    }
                }

                u64 lastAct = 0;
                {
                    std::lock_guard lk(s_movieOverlay.mu);
                    s_movieOverlay.lastActiveTb = now;
                    s_movieOverlay.paused = false;
                    s_movieOverlay.curSec = double(curFrame) / fps;
                    s_movieOverlay.totalSec = double(totalFrames) / fps;
                    lastAct = s_movieOverlay.lastActionTb;
                }
                if (menuObj) {
                    const bool showBar = lastAct != 0 && (double(now - lastAct) / double(kTbClock)) < 2.0;
                    movieUpdateNativeUi(c, menuObj, false, s_pauseSel, showBar, double(curFrame) / fps, double(totalFrames) / fps);
                }

                // Left / Right (D-pad, stick or arrow keys / A / D): +/- 5 seconds with auto-repeat
                if (!gfx::menuOpen()) {
                    const bool left = (pad.buttons & 0x0001u) != 0 || pad.stickX < 68;
                    const bool right = (pad.buttons & 0x0002u) != 0 || pad.stickX > 188;
                    const int dir = right ? 1 : left ? -1 : 0;
                    if (dir != 0) {
                        const bool trigger = (dir != s_lastDir) || (now >= s_nextRepeatTb);
                        if (trigger) {
                            const int stepFrames = std::max(1, int(std::lround(5.0 * fps)));
                            s_pendingFrames += dir * stepFrames;
                            s_nextRepeatTb = now + (dir != s_lastDir ? (kTbClock * 40 / 100) : (kTbClock * 25 / 100));
                            std::lock_guard lk(s_movieOverlay.mu);
                            s_movieOverlay.lastActionTb = now;
                        }
                    } else {
                        s_nextRepeatTb = 0;
                    }
                    s_lastDir = dir;
                }

                if (s_pendingFrames != 0) {
                    std::pair<u32, u32> loc{0, 0};
                    u32 target = 0;
                    bool haveTarget = false;
                    {
                        std::lock_guard lk(s_movieIdx.mu);
                        if (s_movieIdx.discStart == discStart && s_movieIdx.ready && !s_movieIdx.frames.empty()) {
                            const int t = std::clamp(int(curFrame) + s_pendingFrames, 0, int(s_movieIdx.frames.size()) - 1);
                            target = u32(t);
                            loc = s_movieIdx.frames[target];
                            haveTarget = true;
                        }
                    }
                    if (haveTarget) {
                        if (rd8(p + 0x44u) != 0) {
                            wr8(p + 0x45u, 0);
                        } else {
                            wr32(p + 0x48u, loc.first);   // nextReadOffset
                            wr32(p + 0x4Cu, loc.second);  // nextReadSize
                            wr32(p + 0x50u, target);      // readFrameIdx
                            wr32(p + 0xE4u, target);      // currentFrame
                            wr32(p + 0x5Cu, 0);           // videoReadIdx
                            wr32(p + 0x60u, 0);           // audioReadIdx
                            wr32(p + 0x64u, 0);           // writeIdx
                            wr32(p + 0xDCu, 0);           // accum = 0.0f
                            wr8(p + 0x45u, 1);            // resume streaming
                            s_pendingFrames = 0;
                            std::lock_guard lk(s_movieOverlay.mu);
                            s_movieOverlay.curSec = double(target) / fps;
                            s_movieOverlay.lastActionTb = now;
                        }
                    }
                }
            }
            if (viRate() > 60.5) {
                const double dt = s_lastDtTb ? double(now - s_lastDtTb) / double(kTbClock) : 1.0 / viRate();
                s_lastDtTb = now;
                const double step = dt > 0.25 ? 1.0 / viRate() : std::clamp(dt, 0.00025, 1.0 / 29.97);
                c.f[1] = c.ps1[1] = rsingle(step);
            }
        } else {
            if (s_moviePaused) {
                s_moviePaused = false;
                setAudioSpeed(1.0, false);
            }
            s_movieShowBar = false;
            s_movieMenuObj = 0;
        }
    }
    f_7FD8300C_orig(c);
}

// MovieEndDraw (0x7FD839A0): called right after the full-screen video quad in the documentary /
// teaser player (0x7FD710A8, screen 0x13, return address 0x7FD712A4). Screen 0x13 jumps past
// the front-end menu's text-layer pass (0x7FD6B46C -> f_7FD3F324), so invoke the exact same
// f_7FD3F324(0, 0, dt) pass here on top of the video frame to render our native-font pause menu
// and timestamps (slots 10..14) in the original game font.
void hle_MovieEndDraw(Ctx& c) {
    extern void f_7FD839A0_orig(Ctx& c);
    extern void f_7FD3EA50(Ctx& c);
    extern void f_7FD3E9B8(Ctx& c);
    extern void f_7FD3F324(Ctx& c);
    const u32 callerLr = c.lr;
    f_7FD839A0_orig(c);
    if (callerLr != 0x7FD712A4u) return;
    if (!s_moviePaused && !s_movieShowBar) return;
    const u32 menuObj = s_movieMenuObj;
    if (menuObj < 0x7FC00000u || menuObj >= 0x81800000u) return;
    if (rd32(0x80096200u) == 0 || rd32(0x80096220u) == 0) return;

    GuestFrame frame(c);
    const u8 page = rd8(menuObj + 0x3Eu) & 1u;
    const u32 otherBase = menuObj + 0x70u + u32(page ^ 1u) * 7560u;
    u8 savedOther[70];
    for (int i = 0; i < 70; i++) {
        const u32 slot = otherBase + u32(i) * 108u;
        c.r[3] = slot;
        f_7FD3EA50(c);
        savedOther[i] = u8(c.r[3] & 0xFFu);
        if (savedOther[i]) {
            c.r[3] = slot;
            c.r[4] = 0u;
            f_7FD3E9B8(c);
        }
    }

    c.f[1] = c.ps1[1] = s2d(rd32(0x7FDE8288u));
    c.r[3] = 0u;
    c.r[4] = 0u;
    f_7FD3F324(c);

    for (int i = 0; i < 70; i++) {
        if (savedOther[i]) {
            const u32 slot = otherBase + u32(i) * 108u;
            c.r[3] = slot;
            c.r[4] = savedOther[i];
            f_7FD3E9B8(c);
        }
    }
}

// CardCheckSlot (0x7FC06CA8): on Slot A, enable the game's built-in autoCreate flag (r4 = 1)
// so a freshly formatted card gets its initial Rogue Leader save file silently in the background
// instead of prompting on first boot.
void hle_CardCheckSlot(Ctx& c) {
    if (c.r[3] == 0) c.r[4] = 1;
    f_7FC06CA8_orig(c);
}

// Front-end menu customization:
// Promote Special Features (0x12) and Rogue Leaders (0x07) from Options (0x06) into the Main Menu
// (0x04) in the order: Start, Special Features, Rogue Leaders, Options, Quit. Passcodes stays in Options.
void hle_MenuSetScreen(Ctx& c) {
    const u32 menuObj = c.r[3];
    u32 target = c.r[4] & 0xFFu;
    u32 initSel = c.r[5] & 0xFFu;
    if (target == 0x06u) {
        // Every guest caller targeting Options (0x06) is a sub-screen returning to its old Options slot
        // (hle_MainMenuUpdate calls f_7FD6BCF0_orig directly when entering Options from the Main Menu).
        const u32 oldSel = initSel;
        if (oldSel == 5u || oldSel == 0u) {
            // Special Features (5), Rogue Leaders (0) -> back to Main Menu (0x04)
            if (rd8(0x7FE1C4A0u) != 0 && rd8(c.r[13] + 0xFFFFB550u) != 0) {
                GuestFrame gf(c);
                c.r[3] = 0x7FE1C4A0u;
                f_7FD57658(c);
            }
            target = 0x04u;
            initSel = oldSel == 5u ? 1u : 2u;
        } else if (oldSel == 1u) {
            initSel = 0u;  // Passcodes
        } else if (oldSel == 2u) {
            initSel = 1u;  // Game Settings
        } else if (oldSel == 3u) {
            initSel = 2u;  // Video Settings
        } else if (oldSel == 4u) {
            initSel = 3u;  // Sound Settings
        }
    }
    c.r[3] = menuObj;
    c.r[4] = target;
    c.r[5] = initSel;
    f_7FD6BCF0_orig(c);
}

void hle_MainMenuSetup(Ctx& c) {
    GuestFrame gf(c);
    const u32 menuObj = c.r[3];
    wr8(menuObj + 0x32u, 1);
    wr8(menuObj + 0x35u, 0);
    c.r[3] = 0x58u;
    c.r[4] = 0x26u;
    c.r[5] = 0x74u;
    c.r[6] = 0u;
    c.f[1] = c.ps1[1] = s2d(rd32(c.r[2] + 0xFFFFCD50u));
    f_7FD4CE78(c);

    wr8(menuObj + 0x24u, 5);  // 5 items: Start, Special Features, Rogue Leaders, Options, Quit
    wr32(menuObj + 0x2Cu, rd32(c.r[2] + 0xFFFFCE08u));  // attract demo idle timer

    static constexpr u32 kMainStrings[4] = {
        0x7FDE3428u,  // template for START (same layout/baseline as Options/Quit)
        0x7FDE34E0u,  // FrntEnd_opts_main_specialFeatures
        0x7FDE3444u,  // FrntEnd_opts_main_rogueLeaders
        0x7FDE3428u,  // FrntEnd_StartScrn_options
    };
    static constexpr u32 kStartStringAddr = 0x8F000070u;
    static constexpr char kStartLabel[] = "START";
    for (u32 k = 0; k < sizeof(kStartLabel); k++) {
        wr8(kStartStringAddr + k, u8(kStartLabel[k]));
    }
    static constexpr u32 kQuitStringAddr = 0x8F000080u;
    static constexpr char kQuitLabel[] = "QUIT";
    for (u32 k = 0; k < sizeof(kQuitLabel); k++) {
        wr8(kQuitStringAddr + k, u8(kQuitLabel[k]));
    }

    const double x = s2d(rd32(c.r[2] + 0xFFFFCDF4u));      // 160.0
    constexpr double startY = 74.0;
    constexpr double stepY = 29.0;  // matches the vertical spacing of the Options submenu rows
    constexpr double scale = 0.70;  // matches the font size of the Options submenu items

    for (int i = 0; i < 5; i++) {
        c.r[3] = kMainStrings[i < 4 ? i : 3];
        f_7FD58DD8(c);
        const u32 strPtr = c.r[3];
        const u8 idx = rd8(menuObj + 0x3BA4u);
        const u32 slot = rd32(menuObj + 0x3B80u) + u32(idx) * 108u;
        c.r[3] = strPtr;
        c.r[4] = slot;
        c.r[5] = 0u;
        c.f[1] = c.ps1[1] = rsingle(x);
        c.f[2] = c.ps1[2] = rsingle(startY + double(i) * stepY);
        f_7FD58EF0(c);
        if (i == 0) {
            c.r[3] = slot;
            c.r[4] = kStartStringAddr;
            f_7FD3E418(c);
        } else if (i == 4) {
            c.r[3] = slot;
            c.r[4] = kQuitStringAddr;
            f_7FD3E418(c);
        }
        wr32(rd32(menuObj + 0x4508u) + u32(idx) * 4u, 0xFFFFFFFFu);
        const u32 emb = rd32(menuObj + 0x6520u) + u32(idx) * 44u;
        wr16(emb + 2u, 0xFFFFu);
        wr32(emb + 0x20u, 0u);  // clear any leftover X/Y offset from previous screens
        wr32(emb + 0x24u, 0u);
        wr32(emb + 0x28u, slot);  // bind emblem explicitly to this menu item's text slot
        wr8(menuObj + 0x3BA4u, idx + 1u);
    }

    const u8 count = rd8(menuObj + 0x3BA4u);
    for (u8 i = 0; i < count; i++) {
        const u32 slot = rd32(menuObj + 0x3B80u) + u32(i) * 108u;
        c.r[3] = slot;
        c.r[4] = 1u;
        f_7FD3E9B8(c);
        c.r[3] = slot;
        c.f[1] = c.ps1[1] = rsingle(scale);
        f_7FD3EA1C(c);
    }

    c.r[3] = menuObj;
    f_7FD6AF54(c);

    c.r[3] = menuObj;
    c.r[4] = 2u;
    f_7FD76500(c);

    c.r[3] = menuObj;
    c.r[4] = 5u;
    c.r[5] = 10u;
    c.r[6] = 18u;
    c.r[7] = 0u;
    f_7FD6AD9C(c);

    c.r[3] = 0u;
    f_7FD30B8C(c);

    c.r[3] = menuObj;
    c.r[4] = 0x15u;
    f_7FD7695C(c);

    wr8(c.r[13] + 0xFFFFB552u, 0xFFu);
}

void hle_MainMenuUpdate(Ctx& c) {
    const u32 menuObj = c.r[3];
    const u8 btn = rd8(menuObj + 0x6699u);
    const s8 sel = s8(rd8(menuObj + 0x23u));
    const bool accept = (btn & 0x10u) != 0 || ((btn & 0x40u) != 0 && sel == 0);
    if (!accept) {
        f_7FD6C23C_orig(c);  // handles attract demo idle timer
        return;
    }
    GuestFrame gf(c);
    wr32(menuObj + 0x2Cu, rd32(c.r[2] + 0xFFFFCE08u));
    c.r[3] = 0x1E1u;
    c.r[4] = 0x40u;
    f_7FD5123C(c);

    c.r[3] = menuObj + 0x6718u;
    c.r[4] = u32(s32(sel));
    c.r[5] = 1u;
    c.r[6] = rd8(menuObj + 0x3Eu);
    f_7FD77158(c);

    if (sel == 4) {
        logf("menu", "quit to desktop (main menu)");
        quitToDesktop();
    }

    static constexpr u32 kTargets[4] = {0x17u, 0x12u, 0x07u, 0x06u};
    const u32 target = (sel >= 0 && sel < 4) ? kTargets[sel] : 0x17u;
    c.r[3] = menuObj;
    c.r[4] = target;
    c.r[5] = 0u;
    f_7FD6BCF0_orig(c);
}

void hle_OptionsMenuSetup(Ctx& c) {
    GuestFrame gf(c);
    const u32 menuObj = c.r[3];
    c.r[3] = menuObj;
    c.r[4] = 6u;
    c.r[5] = 11u;
    c.r[6] = 24u;
    c.r[7] = 0u;
    f_7FD6AD9C(c);

    wr8(menuObj + 0x32u, 1);
    wr8(menuObj + 0x35u, 0);

    static constexpr u32 kOptStrings[5] = {
        0x7FDE3464u,  // FrntEnd_opts_main_passcodes
        0x7FDE3480u,  // FrntEnd_opts_main_gameSettings
        0x7FDE34A0u,  // FrntEnd_opts_main_videoSettings
        0x7FDE34C0u,  // FrntEnd_opts_main_soundSettings
        0x7FDE3504u,  // FrntEnd_opts_main_back
    };
    static constexpr u32 kOptRows[5] = {6u, 8u, 10u, 12u, 14u};
    const double scale = s2d(rd32(c.r[2] + 0xFFFFCD50u));

    for (int i = 0; i < 5; i++) {
        c.r[3] = kOptStrings[i];
        f_7FD58DD8(c);
        const u32 strPtr = c.r[3];
        const u8 idx = rd8(menuObj + 0x3BA4u);
        const u32 slot = rd32(menuObj + 0x3B80u) + u32(idx) * 108u;
        c.r[3] = strPtr;
        c.r[4] = slot;
        c.r[5] = 1u;
        c.f[1] = c.ps1[1] = rsingle(scale);
        c.f[2] = c.ps1[2] = rsingle(scale);
        f_7FD58EF0(c);
        wr32(rd32(menuObj + 0x4508u) + u32(idx) * 4u, kOptRows[i]);
        c.r[3] = rd32(menuObj + 0x6520u) + u32(idx) * 44u;
        c.r[4] = kOptRows[i] + 1u;
        f_7FD77960(c);
        wr8(menuObj + 0x3BA4u, idx + 1u);
    }

    // OPTIONS title header (gold)
    {
        c.r[3] = 0x7FDE351Cu;  // FrntEnd_opts_main_options
        f_7FD58DD8(c);
        const u32 strPtr = c.r[3];
        const u8 idx = rd8(menuObj + 0x3BA4u);
        const u32 slot = rd32(menuObj + 0x3B80u) + u32(idx) * 108u;
        c.r[3] = strPtr;
        c.r[4] = slot;
        c.r[5] = 1u;
        c.f[1] = c.ps1[1] = rsingle(scale);
        c.f[2] = c.ps1[2] = rsingle(scale);
        f_7FD58EF0(c);
        wr32(rd32(menuObj + 0x4508u) + u32(idx) * 4u, 3u);
        c.r[3] = slot;
        c.r[4] = 0u;
        c.r[5] = 0xEBu;
        c.r[6] = 0xD7u;
        c.r[7] = 0x7Du;
        c.r[8] = 0xFFu;
        f_7FD3EA00(c);
        wr8(menuObj + 0x3BA4u, idx + 1u);
    }

    const u8 count = rd8(menuObj + 0x3BA4u);
    for (u8 i = 0; i < count; i++) {
        c.r[3] = rd32(menuObj + 0x3B80u) + u32(i) * 108u;
        c.r[4] = 1u;
        f_7FD3E9B8(c);
    }

    wr8(menuObj + 0x24u, 5);  // 5 selectable items
    if (rd8(menuObj + 0x23u) >= 5u) wr8(menuObj + 0x23u, 0u);
    c.r[3] = menuObj;
    f_7FD6AF54(c);

    c.r[3] = menuObj;
    c.r[4] = 2u;
    f_7FD76500(c);

    c.r[3] = menuObj;
    c.r[4] = 0x15u;
    f_7FD7695C(c);
}

void hle_OptionsMenuUpdate(Ctx& c) {
    const u32 menuObj = c.r[3];
    const u8 btn = rd8(menuObj + 0x6699u);
    const s8 sel = s8(rd8(menuObj + 0x23u));
    const bool back = (btn & 0x20u) != 0 || ((btn & 0x10u) != 0 && sel == 4);
    if (back) {
        GuestFrame gf(c);
        c.r[3] = menuObj + 0x6718u;
        c.r[4] = 4u;
        c.r[5] = 1u;
        c.r[6] = rd8(menuObj + 0x3Eu);
        f_7FD77158(c);

        c.r[3] = 0x1E2u;
        c.r[4] = 0x40u;
        f_7FD5123C(c);

        if (rd8(0x7FE1C4A0u) != 0 && rd8(c.r[13] + 0xFFFFB550u) != 0) {
            c.r[3] = 0x7FE1C4A0u;
            f_7FD57658(c);
        }
        c.r[3] = menuObj;
        c.r[4] = 0x04u;
        c.r[5] = 3u;  // highlight Options in Main Menu
        f_7FD6BCF0(c);
        return;
    }
    if ((btn & 0x10u) == 0) return;
    GuestFrame gf(c);
    c.r[3] = 0x1E1u;
    c.r[4] = 0x40u;
    f_7FD5123C(c);

    c.r[3] = menuObj;
    c.r[4] = 0x15u;
    f_7FD7695C(c);

    c.r[3] = menuObj + 0x6718u;
    c.r[4] = u32(s32(sel));
    c.r[5] = 1u;
    c.r[6] = rd8(menuObj + 0x3Eu);
    f_7FD77158(c);

    static constexpr u32 kOptTargets[4] = {0x08u, 0x09u, 0x0Bu, 0x0Fu};
    if (sel >= 0 && sel < 4) {
        c.r[3] = menuObj;
        c.r[4] = kOptTargets[sel];
        c.r[5] = 0u;
        f_7FD6BCF0(c);
    }
}

// --- Redesigned Music Hall (screen 0x14, mode 0) ---------------------------------------------
// Replaces the original artwork-cycling Music Hall with a clean starfield background and a
// paginated standard menu list of all 114 tracks (9 tracks + Previous Page + Next Page + Back
// = 12 selectable rows per page). Because the front-end's built-in emblem array only holds 9
// entries, we drive selection highlighting and emblem 0 directly without calling f_7FD6AF54.
namespace {
constexpr int kMusicHallTrackCount = 114;
constexpr int kMusicHallPerPage = 11;
constexpr int kMusicHallPageCount = (kMusicHallTrackCount + kMusicHallPerPage - 1) / kMusicHallPerPage;  // 11
constexpr u32 kMusicHallTableAddr = 0x80060C38u;  // 16-byte entries: +0 h78, +2 h7a, +4 utf16Ptr, +10 b82

int s_musicHallPage = 0;
int s_musicHallItemCount = 0;
int s_musicHallTitleSlot = -1;
int s_musicHallElapsedSlot = -1;
int s_musicHallRemainSlot = -1;
int s_musicHallLastShownEl = -1;
int s_musicHallLastShownRem = -1;
int s_musicHallPlaying = -1;
int s_musicHallPending = -1;
bool s_musicHallAudioInit = false;
bool s_musicHallAudioReady = false;
u64 s_musicHallStartTb = 0;
std::atomic<bool> s_musicHallOverlayActive{false};
std::atomic<double> s_musicHallOverlayElapsed{0.0};
std::atomic<double> s_musicHallOverlayTotal{0.0};

int s_artTitleSlot = -1;
int s_artIndexSlot = -1;
int s_artLastPic = -1;
}  // namespace

// Forward-declared helper for Art Gallery titles (defined below)
const char* artGalleryTitleForIndex(int idx, int& outTotal);

namespace {

double musicHallTrackDuration(int trackIdx) {
    if (trackIdx < 0 || trackIdx >= kMusicHallTrackCount) return 0.0;
    const u32 entry = kMusicHallTableAddr + u32(trackIdx) * 16u;
    const double dur = s2d(rd32(entry + 12u));
    // Track 5 ("Defending A Train") has 0.0 in the original table; actual length is 1:48 (108 s).
    if (dur <= 0.0 && trackIdx == 5) return 108.0;
    return dur > 0.0 ? dur : 0.0;
}

void formatTrackDuration(double sec, char* out, size_t maxLen) {
    const int total = (sec > 0.0) ? int(sec + 0.5) : 0;
    const int m = total / 60;
    const int s = total % 60;
    std::snprintf(out, maxLen, "%d:%02d", m, s);
}

void writeGuestAscii(u32 addr, const char* s) {
    u32 k = 0;
    do {
        wr8(addr + k, u8(s[k]));
    } while (s[k++] != '\0');
}

void readGuestUtf16Ascii(u32 utf16Addr, char* out, size_t maxLen) {
    size_t n = 0;
    while (n + 1 < maxLen) {
        const u16 ch = rd16(utf16Addr + u32(n) * 2u);
        if (ch == 0) break;
        out[n++] = (ch < 128u) ? char(ch) : '?';
    }
    out[n] = '\0';
}

void musicHallApplyHighlight(Ctx& c, u32 menuObj) {
    const int sel = int(s8(rd8(menuObj + 0x23u)));
    const u32 slotBase = rd32(menuObj + 0x3B80u);
    const double t = 0.5 + 0.5 * std::sin((double(tbNow()) / double(kTbClock)) * 5.2);
    const u8 selG = u8(std::lround(42.0 + t * (232.0 - 42.0)));
    const u8 selB = u8(std::lround(20.0 + t * (48.0 - 20.0)));
    for (int i = 0; i < s_musicHallItemCount; i++) {
        const u32 slot = slotBase + u32(i) * 108u;
        c.r[3] = slot;
        c.r[4] = 0u;
        if (i == sel) {
            c.r[5] = 0xFFu;
            c.r[6] = selG;
            c.r[7] = selB;
            c.r[8] = 0xFFu;
        } else {
            c.r[5] = 0xFFu;
            c.r[6] = 0xFFu;
            c.r[7] = 0xFFu;
            c.r[8] = 0xFFu;
        }
        f_7FD3EA00(c);
    }
    // Point emblem 0 at the selected text slot (embBase + 0x24 is the built-in Y-up offset)
    const u32 embBase = rd32(menuObj + 0x6520u);
    if (sel >= 0 && sel < s_musicHallItemCount) {
        wr8(embBase + 0u, 1u);
        wr16(embBase + 2u, 0xFFFFu);
        wr32(embBase + 0x24u, d2s(rsingle(-4.0)));
        wr32(embBase + 0x28u, slotBase + u32(sel) * 108u);
        c.r[3] = embBase;
        f_7FD78084(c);
    } else {
        wr8(embBase + 0u, 0u);
    }
    for (u32 i = 1u; i < 9u; i++) {
        wr8(embBase + i * 44u, 0u);
    }
}

void musicHallPopulatePage(Ctx& c, u32 menuObj, int page, int initSel) {
    if (page < 0) page = kMusicHallPageCount - 1;
    if (page >= kMusicHallPageCount) page = 0;
    s_musicHallPage = page;

    const int startTrack = page * kMusicHallPerPage;
    const int tracksOnPage = std::min(kMusicHallPerPage, kMusicHallTrackCount - startTrack);
    const int itemCount = tracksOnPage + 3;  // tracks + PREVIOUS PAGE + NEXT PAGE + BACK
    s_musicHallItemCount = itemCount;

    // Hide all 70 text slots first
    for (u32 i = 0; i < 70u; i++) {
        c.r[3] = rd32(menuObj + 0x3B80u) + i * 108u;
        c.r[4] = 0u;
        f_7FD3E9B8(c);
    }
    // Reset slot counter; keep menuObj + 0x24 = 0 so f_7FD6A290 does not call f_7FD6AF54
    // (which assumes <= 9 selectable items in the emblem array). We handle Up/Down & visuals here.
    wr8(menuObj + 0x3BA4u, 0u);
    wr8(menuObj + 0x25u, 0u);
    wr8(menuObj + 0x24u, 0u);
    if (initSel < 0) initSel = 0;
    if (initSel >= itemCount) initSel = itemCount - 1;
    wr8(menuObj + 0x23u, u8(initSel));

    // Template string pointer for f_7FD58EF0 initialization
    c.r[3] = 0x7FDE3428u;  // FrntEnd_StartScrn_options
    f_7FD58DD8(c);
    const u32 tmplStr = c.r[3];

    // Helper to add a 2D text slot
    auto addSlot = [&](double x, double y, double scale, u32 align) -> u32 {
        const u8 idx = rd8(menuObj + 0x3BA4u);
        const u32 slot = rd32(menuObj + 0x3B80u) + u32(idx) * 108u;
        c.r[3] = tmplStr;
        c.r[4] = slot;
        c.r[5] = 0u;
        c.f[1] = c.ps1[1] = rsingle(x);
        c.f[2] = c.ps1[2] = rsingle(y);
        f_7FD58EF0(c);
        wr32(rd32(menuObj + 0x4508u) + u32(idx) * 4u, 0xFFFFFFFFu);
        c.r[3] = slot;
        c.r[4] = align;
        f_7FD3E9C0(c);
        c.r[3] = slot;
        c.f[1] = c.ps1[1] = rsingle(scale);
        f_7FD3EA1C(c);
        c.r[3] = slot;
        c.r[4] = 1u;
        f_7FD3E9B8(c);
        wr8(menuObj + 0x3BA4u, idx + 1u);
        return slot;
    };

    constexpr double listX = 110.0;
    constexpr double startY = 84.0;
    constexpr double stepY = 21.0;
    constexpr double itemScale = 0.50;

    // 1) Track rows (0 .. tracksOnPage - 1) and measure each title's right edge
    double trackRightX[kMusicHallPerPage] = {};
    constexpr u32 kBoxScratch = 0x8F0000A0u;
    for (int i = 0; i < tracksOnPage; i++) {
        const int trackIdx = startTrack + i;
        const u32 entry = kMusicHallTableAddr + u32(trackIdx) * 16u;
        const u32 utf16Ptr = rd32(entry + 4u);
        const u32 slot = addSlot(listX, startY + double(i) * stepY, itemScale, 0u);
        c.r[3] = slot;
        c.r[4] = utf16Ptr;
        f_7FD3E454(c);
        c.r[3] = slot;
        c.r[4] = kBoxScratch + 0u;
        c.r[5] = kBoxScratch + 4u;
        c.r[6] = kBoxScratch + 8u;
        c.r[7] = kBoxScratch + 12u;
        f_7FD3EB1C(c);
        trackRightX[i] = std::max(s2d(rd32(kBoxScratch + 0u)), s2d(rd32(kBoxScratch + 4u)));
    }

    // 2) Navigation rows: PREVIOUS PAGE, NEXT PAGE, BACK
    static constexpr u32 kPrevStrAddr = 0x8F000100u;
    static constexpr u32 kNextStrAddr = 0x8F000120u;
    static constexpr u32 kBackStrAddr = 0x8F000140u;
    writeGuestAscii(kPrevStrAddr, "PREVIOUS PAGE");
    writeGuestAscii(kNextStrAddr, "NEXT PAGE");
    writeGuestAscii(kBackStrAddr, "BACK");

    const double navBaseY = startY + double(kMusicHallPerPage) * stepY + 10.0;
    const u32 navAddrs[3] = {kPrevStrAddr, kNextStrAddr, kBackStrAddr};
    for (int j = 0; j < 3; j++) {
        const u32 slot = addSlot(listX, navBaseY + double(j) * stepY, itemScale, 0u);
        c.r[3] = slot;
        c.r[4] = navAddrs[j];
        f_7FD3E418(c);
    }

    // 2b) Track durations "M:SS" in soft gold right after each track title (non-selectable)
    static constexpr u32 kDurStrBase = 0x8F000240u;  // 12 * 16 = 192 bytes
    for (int i = 0; i < tracksOnPage; i++) {
        const int trackIdx = startTrack + i;
        const double dur = musicHallTrackDuration(trackIdx);
        char durBuf[16];
        formatTrackDuration(dur, durBuf, sizeof(durBuf));
        const u32 durAddr = kDurStrBase + u32(i) * 16u;
        writeGuestAscii(durAddr, durBuf);
        const u32 slot = addSlot(trackRightX[i] + 8.0, startY + double(i) * stepY, itemScale, 0u);
        c.r[3] = slot;
        c.r[4] = durAddr;
        f_7FD3E418(c);
        c.r[3] = slot;
        c.r[4] = 0u;
        c.r[5] = 0xEBu;
        c.r[6] = 0xD7u;
        c.r[7] = 0x7Du;
        c.r[8] = 0xFFu;
        f_7FD3EA00(c);
    }

    // 3) Header: "MUSIC HALL" in gold + "X / 10" in white (no "PAGE" word)
    static constexpr u32 kHeaderStrAddr = 0x8F000160u;
    static constexpr u32 kPageNumStrAddr = 0x8F000190u;
    writeGuestAscii(kHeaderStrAddr, "MUSIC HALL");
    {
        const u32 slot = addSlot(275.0, 48.0, 0.72, 2u);
        c.r[3] = slot;
        c.r[4] = kHeaderStrAddr;
        f_7FD3E418(c);
        c.r[3] = slot;
        c.r[4] = 0u;
        c.r[5] = 0xEBu;
        c.r[6] = 0xD7u;
        c.r[7] = 0x7Du;
        c.r[8] = 0xFFu;
        f_7FD3EA00(c);
    }
    {
        char pageBuf[32];
        std::snprintf(pageBuf, sizeof(pageBuf), "%d /%d", page + 1, kMusicHallPageCount);
        writeGuestAscii(kPageNumStrAddr, pageBuf);
        const u32 slot = addSlot(395.0, 48.0, 0.72, 0u);
        c.r[3] = slot;
        c.r[4] = kPageNumStrAddr;
        f_7FD3E418(c);
        c.r[3] = slot;
        c.r[4] = 0u;
        c.r[5] = 0xFFu;
        c.r[6] = 0xFFu;
        c.r[7] = 0xFFu;
        c.r[8] = 0xFFu;
        f_7FD3EA00(c);
    }

    // 4) Currently playing track title (centered, gold, without "NOW PLAYING:" prefix)
    static constexpr u32 kNowPlayingStrAddr = 0x8F0001C0u;
    char npBuf[96];
    if (s_musicHallPlaying >= 0 && s_musicHallPlaying < kMusicHallTrackCount) {
        readGuestUtf16Ascii(rd32(kMusicHallTableAddr + u32(s_musicHallPlaying) * 16u + 4u), npBuf, sizeof(npBuf));
    } else {
        std::snprintf(npBuf, sizeof(npBuf), "SELECT A TRACK TO PLAY");
    }
    writeGuestAscii(kNowPlayingStrAddr, npBuf);
    {
        s_musicHallTitleSlot = int(rd8(menuObj + 0x3BA4u));
        const u32 slot = addSlot(320.0, 394.0, 0.52, 2u);
        c.r[3] = slot;
        c.r[4] = kNowPlayingStrAddr;
        f_7FD3E418(c);
        c.r[3] = slot;
        c.r[4] = 0u;
        c.r[5] = 0xEBu;
        c.r[6] = 0xD7u;
        c.r[7] = 0x7Du;
        c.r[8] = 0xFFu;
        f_7FD3EA00(c);
    }

    // 5) Elapsed & remaining time labels in native menu font directly below the track title
    static constexpr u32 kElapsedStrAddr = 0x8F000300u;
    static constexpr u32 kRemainStrAddr = 0x8F000320u;
    writeGuestAscii(kElapsedStrAddr, "0:00");
    writeGuestAscii(kRemainStrAddr, "-0:00");
    {
        s_musicHallElapsedSlot = int(rd8(menuObj + 0x3BA4u));
        const u32 slot = addSlot(165.0, 418.0, 0.50, 2u);  // centered left of bar
        c.r[3] = slot;
        c.r[4] = kElapsedStrAddr;
        f_7FD3E418(c);
        c.r[3] = slot;
        c.r[4] = 0u;
        c.r[5] = 0xFFu;
        c.r[6] = 0xFFu;
        c.r[7] = 0xFFu;
        c.r[8] = 0xFFu;
        f_7FD3EA00(c);
        c.r[3] = slot;
        c.r[4] = (s_musicHallPlaying >= 0) ? 1u : 0u;
        f_7FD3E9B8(c);
    }
    {
        s_musicHallRemainSlot = int(rd8(menuObj + 0x3BA4u));
        const u32 slot = addSlot(468.0, 418.0, 0.50, 2u);  // centered right of bar
        c.r[3] = slot;
        c.r[4] = kRemainStrAddr;
        f_7FD3E418(c);
        c.r[3] = slot;
        c.r[4] = 0u;
        c.r[5] = 0xFFu;
        c.r[6] = 0xFFu;
        c.r[7] = 0xFFu;
        c.r[8] = 0xFFu;
        f_7FD3EA00(c);
        c.r[3] = slot;
        c.r[4] = (s_musicHallPlaying >= 0) ? 1u : 0u;
        f_7FD3E9B8(c);
    }
    s_musicHallLastShownEl = -1;
    s_musicHallLastShownRem = -1;

    // Sync per-page slot count so f_7FD6B144 renders all slots
    wr8(menuObj + 0x3BA5u + rd8(menuObj + 0x3Eu), rd8(menuObj + 0x3BA4u));

    // Apply selection highlight & emblem 0
    musicHallApplyHighlight(c, menuObj);
}

void musicHallPlayTrack(Ctx& c, u32 menuObj, int trackIdx) {
    if (trackIdx < 0 || trackIdx >= kMusicHallTrackCount) return;
    const u32 entry = kMusicHallTableAddr + u32(trackIdx) * 16u;
    c.r[3] = rd16(entry + 2u);   // songId (+0x7A from 0x80060BC0)
    c.r[4] = rd16(entry + 0u);   // arg2 (+0x78)
    c.r[5] = rd8(entry + 10u);   // arg3 (+0x82)
    c.r[6] = 0x12Cu;
    c.f[1] = c.ps1[1] = 0.0;
    f_7FD4CE78(c);
    s_musicHallPlaying = trackIdx;
    s_musicHallStartTb = tbNow();
    const double total = musicHallTrackDuration(trackIdx);
    s_musicHallOverlayTotal.store(total, std::memory_order_relaxed);
    s_musicHallOverlayElapsed.store(0.0, std::memory_order_relaxed);
    s_musicHallOverlayActive.store(true, std::memory_order_relaxed);

    // Update centered track title footer (without "NOW PLAYING:" prefix)
    if (s_musicHallTitleSlot >= 0) {
        static constexpr u32 kNowPlayingStrAddr = 0x8F0001C0u;
        char npBuf[96];
        readGuestUtf16Ascii(rd32(entry + 4u), npBuf, sizeof(npBuf));
        writeGuestAscii(kNowPlayingStrAddr, npBuf);
        const u32 slot = rd32(menuObj + 0x3B80u) + u32(s_musicHallTitleSlot) * 108u;
        c.r[3] = slot;
        c.r[4] = kNowPlayingStrAddr;
        f_7FD3E418(c);
    }
}
}  // namespace

MusicHallOverlay musicHallOverlay() {
    MusicHallOverlay o;
    o.active = s_musicHallOverlayActive.load(std::memory_order_relaxed);
    o.elapsedSec = s_musicHallOverlayElapsed.load(std::memory_order_relaxed);
    o.totalSec = s_musicHallOverlayTotal.load(std::memory_order_relaxed);
    return o;
}

void artGallerySyncNativeCaption(Ctx& c, u32 menuObj, bool force) {
    const int pic = int(rd8(menuObj + 0x6794u + 0x3E8u));
    if (!force && pic == s_artLastPic) return;
    s_artLastPic = pic;
    int total = 27;
    const char* title = artGalleryTitleForIndex(pic, total);
    static constexpr u32 kArtTitleStrAddr = 0x8F000340u;
    static constexpr u32 kArtIdxStrAddr = 0x8F0003C0u;
    constexpr u32 kBoxScratch = 0x8F0000A0u;
    writeGuestAscii(kArtTitleStrAddr, title ? title : "");
    char idxBuf[24];
    std::snprintf(idxBuf, sizeof(idxBuf), "%d /%d", pic + 1, total);
    writeGuestAscii(kArtIdxStrAddr, idxBuf);
    double titleRightX = 320.0;
    if (s_artTitleSlot >= 0) {
        const u32 slot = rd32(menuObj + 0x3B80u) + u32(s_artTitleSlot) * 108u;
        c.r[3] = slot;
        c.r[4] = kArtTitleStrAddr;
        f_7FD3E418(c);
        c.r[3] = slot;
        c.r[4] = kBoxScratch + 0u;
        c.r[5] = kBoxScratch + 4u;
        c.r[6] = kBoxScratch + 8u;
        c.r[7] = kBoxScratch + 12u;
        f_7FD3EB1C(c);
        titleRightX = std::max(s2d(rd32(kBoxScratch + 0u)), s2d(rd32(kBoxScratch + 4u)));
    }
    if (s_artIndexSlot >= 0) {
        const u32 slot = rd32(menuObj + 0x3B80u) + u32(s_artIndexSlot) * 108u;
        c.r[3] = slot;
        c.r[4] = kArtIdxStrAddr;
        f_7FD3E418(c);
        c.r[3] = slot;
        c.f[1] = c.ps1[1] = rsingle(titleRightX + 12.0);
        c.f[2] = c.ps1[2] = rsingle(454.0);
        f_7FD3E9A4(c);
    }
}

void hle_MusicHallSetup(Ctx& c) {
    const u32 menuObj = c.r[3];
    // Mode 1 is Art Gallery — run original setup, then add native-font caption & index slots.
    if (rd32(menuObj + 0x04u) != 0u) {
        s_musicHallOverlayActive.store(false, std::memory_order_relaxed);
        f_7FD71670_orig(c);
        GuestFrame gf(c);
        c.r[3] = 0x7FDE3428u;
        f_7FD58DD8(c);
        const u32 tmplStr = c.r[3];
        auto addArtSlot = [&](double x, double y, double scale, u32 align, u8 r, u8 g, u8 b) -> int {
            const u8 idx = rd8(menuObj + 0x3BA4u);
            const u32 slot = rd32(menuObj + 0x3B80u) + u32(idx) * 108u;
            c.r[3] = tmplStr;
            c.r[4] = slot;
            c.r[5] = 0u;
            c.f[1] = c.ps1[1] = rsingle(x);
            c.f[2] = c.ps1[2] = rsingle(y);
            f_7FD58EF0(c);
            wr32(rd32(menuObj + 0x4508u) + u32(idx) * 4u, 0xFFFFFFFFu);
            c.r[3] = slot;
            c.r[4] = align;
            f_7FD3E9C0(c);
            c.r[3] = slot;
            c.f[1] = c.ps1[1] = rsingle(scale);
            f_7FD3EA1C(c);
            c.r[3] = slot;
            c.r[4] = 0u;
            c.r[5] = r;
            c.r[6] = g;
            c.r[7] = b;
            c.r[8] = 0xFFu;
            f_7FD3EA00(c);
            c.r[3] = slot;
            c.r[4] = 1u;
            f_7FD3E9B8(c);
            wr8(menuObj + 0x3BA4u, idx + 1u);
            return int(idx);
        };
        s_artTitleSlot = addArtSlot(298.0, 454.0, 0.56, 2u, 0xEBu, 0xD7u, 0x7Du);
        s_artIndexSlot = addArtSlot(400.0, 454.0, 0.52, 0u, 0xFFu, 0xFFu, 0xFFu);
        wr8(menuObj + 0x3BA5u + rd8(menuObj + 0x3Eu), rd8(menuObj + 0x3BA4u));
        s_artLastPic = -1;
        artGallerySyncNativeCaption(c, menuObj, true);
        return;
    }

    GuestFrame gf(c);
    // Ensure Art Gallery artwork renderer is OFF
    wr8(menuObj + 0x6794u + 0x3E7u, 0u);
    // Pure starfield background: disable 3D scene & background image fade
    wr8(menuObj + 0x6579u, 0u);
    wr32(menuObj + 0x657Cu, 0u);
    wr8(menuObj + 0x6090u, 2u);
    wr8(menuObj + 0x6091u, 3u);

    s_musicHallPage = 0;
    s_musicHallPlaying = -1;
    s_musicHallPending = -1;
    s_musicHallAudioInit = false;
    s_musicHallAudioReady = false;
    s_musicHallOverlayActive.store(false, std::memory_order_relaxed);
    s_musicHallOverlayElapsed.store(0.0, std::memory_order_relaxed);
    s_musicHallOverlayTotal.store(0.0, std::memory_order_relaxed);

    musicHallPopulatePage(c, menuObj, 0, 0);

    // Same camera/starfield transition call as original screen 0x14 (no 3D hangar models)
    c.r[3] = menuObj;
    c.r[4] = 0x1Eu;
    c.r[5] = 0x0Au;
    c.r[6] = 0x18u;
    c.r[7] = 0u;
    f_7FD6AD9C(c);

    f_7FD30A38(c);
}

void hle_MusicHallUpdate(Ctx& c) {
    const u32 menuObj = c.r[3];
    // Mode 1 is Art Gallery — run original update, then sync native-font caption & index.
    if (rd32(menuObj + 0x04u) != 0u) {
        f_7FD71A14_orig(c);
        if (rd8(menuObj + 0x6794u + 0x3E7u) == 1u) {
            GuestFrame gf(c);
            artGallerySyncNativeCaption(c, menuObj, false);
        }
        return;
    }

    GuestFrame gf(c);
    f_7FD30AC0(c);
    const bool ready = (c.r[3] & 0xFFu) != 0u;

    // Initialize Music Hall audio bank once the screen transition finishes (same as original)
    if (ready && !s_musicHallAudioInit && !s_musicHallAudioReady) {
        f_7FD4AB34(c);
        s_musicHallAudioInit = true;
    }
    if (s_musicHallAudioInit && !s_musicHallAudioReady) {
        f_7FD59A84(c);
        if (rd8(c.r[3]) != 0u) {
            s_musicHallAudioReady = true;
            if (s_musicHallPending >= 0) {
                const int trk = s_musicHallPending;
                s_musicHallPending = -1;
                musicHallPlayTrack(c, menuObj, trk);
            }
        }
    }

    // Update playback progress bar timer & native time text slots
    if (s_musicHallPlaying >= 0 && s_musicHallPlaying < kMusicHallTrackCount) {
        const u32 entry = kMusicHallTableAddr + u32(s_musicHallPlaying) * 16u;
        const double total = musicHallTrackDuration(s_musicHallPlaying);
        const bool loops = rd8(entry + 8u) != 0u;
        f_7FD4D0C0(c);
        double elapsed = c.f[1];
        if (elapsed <= 0.0 && s_musicHallStartTb != 0) {
            elapsed = double(tbNow() - s_musicHallStartTb) / double(kTbClock);
        }
        if (total > 0.0) {
            elapsed = loops ? std::fmod(elapsed, total) : std::clamp(elapsed, 0.0, total);
        }
        const double rem = (total > 0.0) ? std::max(0.0, total - elapsed) : 0.0;
        s_musicHallOverlayElapsed.store(elapsed, std::memory_order_relaxed);
        s_musicHallOverlayTotal.store(total > 0.0 ? total : 0.0, std::memory_order_relaxed);
        s_musicHallOverlayActive.store(true, std::memory_order_relaxed);

        const int elSec = int(elapsed + 0.5);
        const int remSec = int(rem + 0.5);
        if (elSec != s_musicHallLastShownEl && s_musicHallElapsedSlot >= 0) {
            s_musicHallLastShownEl = elSec;
            static constexpr u32 kElapsedStrAddr = 0x8F000300u;
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d:%02d", elSec / 60, elSec % 60);
            writeGuestAscii(kElapsedStrAddr, buf);
            const u32 slot = rd32(menuObj + 0x3B80u) + u32(s_musicHallElapsedSlot) * 108u;
            c.r[3] = slot;
            c.r[4] = kElapsedStrAddr;
            f_7FD3E418(c);
            c.r[3] = slot;
            c.r[4] = 1u;
            f_7FD3E9B8(c);
        }
        if (remSec != s_musicHallLastShownRem && s_musicHallRemainSlot >= 0) {
            s_musicHallLastShownRem = remSec;
            static constexpr u32 kRemainStrAddr = 0x8F000320u;
            char buf[16];
            std::snprintf(buf, sizeof(buf), "-%d:%02d", remSec / 60, remSec % 60);
            writeGuestAscii(kRemainStrAddr, buf);
            const u32 slot = rd32(menuObj + 0x3B80u) + u32(s_musicHallRemainSlot) * 108u;
            c.r[3] = slot;
            c.r[4] = kRemainStrAddr;
            f_7FD3E418(c);
            c.r[3] = slot;
            c.r[4] = 1u;
            f_7FD3E9B8(c);
        }
    }

    const u8 btn = rd8(menuObj + 0x6699u);
    s8 sel = s8(rd8(menuObj + 0x23u));
    if (sel >= 0 && sel < s_musicHallItemCount) {
        const double t = 0.5 + 0.5 * std::sin((double(tbNow()) / double(kTbClock)) * 5.2);
        const u32 slot = rd32(menuObj + 0x3B80u) + u32(sel) * 108u;
        c.r[3] = slot;
        c.r[4] = 0u;
        c.r[5] = 0xFFu;
        c.r[6] = u8(std::lround(42.0 + t * (232.0 - 42.0)));
        c.r[7] = u8(std::lround(20.0 + t * (48.0 - 20.0)));
        c.r[8] = 0xFFu;
        f_7FD3EA00(c);
    }
    const int startTrack = s_musicHallPage * kMusicHallPerPage;
    const int tracksOnPage = std::min(kMusicHallPerPage, kMusicHallTrackCount - startTrack);
    const int prevIdx = tracksOnPage;
    const int nextIdx = tracksOnPage + 1;
    const int backIdx = tracksOnPage + 2;

    // Handle Up / Down navigation directly (supports >9 selectable rows safely)
    if ((btn & 0x01u) != 0 && s_musicHallItemCount > 0) {  // UP
        if (sel > 0) {
            sel--;
            wr8(menuObj + 0x23u, u8(sel));
            c.r[3] = 0x1E5u;
            c.r[4] = 0x40u;
            f_7FD5123C(c);
            musicHallApplyHighlight(c, menuObj);
        }
        return;
    }
    if ((btn & 0x02u) != 0 && s_musicHallItemCount > 0) {  // DOWN
        if (sel + 1 < s_musicHallItemCount) {
            sel++;
            wr8(menuObj + 0x23u, u8(sel));
            c.r[3] = 0x1E5u;
            c.r[4] = 0x40u;
            f_7FD5123C(c);
            musicHallApplyHighlight(c, menuObj);
        }
        return;
    }

    const bool back = ((btn & 0x20u) != 0 && ready) || ((btn & 0x10u) != 0 && sel == backIdx);
    if (back) {
        s_musicHallOverlayActive.store(false, std::memory_order_relaxed);
        wr8(menuObj + 0x32u, 1u);
        wr8(menuObj + 0x35u, 0u);
        c.r[3] = menuObj;
        c.r[4] = 0x12u;  // Special Features
        c.r[5] = 0u;
        f_7FD6BCF0(c);

        c.r[3] = 0x1E2u;
        c.r[4] = 0x40u;
        f_7FD5123C(c);

        if (s_musicHallAudioInit) {
            f_7FD4ADD0(c);
        }
        // Restore menu theme
        c.r[3] = 0x58u;
        c.r[4] = 0x26u;
        c.r[5] = 0x74u;
        c.r[6] = 0u;
        c.f[1] = c.ps1[1] = 0.0;
        f_7FD4CE78(c);
        return;
    }

    // Left / Right flips pages directly
    if ((btn & 0x04u) != 0) {  // LEFT
        c.r[3] = 0x1E5u;
        c.r[4] = 0x40u;
        f_7FD5123C(c);
        musicHallPopulatePage(c, menuObj, s_musicHallPage - 1, 0);
        return;
    }
    if ((btn & 0x08u) != 0) {  // RIGHT
        c.r[3] = 0x1E5u;
        c.r[4] = 0x40u;
        f_7FD5123C(c);
        musicHallPopulatePage(c, menuObj, s_musicHallPage + 1, 0);
        return;
    }

    if ((btn & 0x10u) == 0) return;  // A / Left-click

    c.r[3] = 0x1E1u;
    c.r[4] = 0x40u;
    f_7FD5123C(c);

    if (sel == prevIdx) {
        musicHallPopulatePage(c, menuObj, s_musicHallPage - 1, 0);
        return;
    }
    if (sel == nextIdx) {
        musicHallPopulatePage(c, menuObj, s_musicHallPage + 1, 0);
        return;
    }
    if (sel >= 0 && sel < tracksOnPage) {
        const int trackIdx = startTrack + int(sel);
        if (s_musicHallAudioReady) {
            musicHallPlayTrack(c, menuObj, trackIdx);
        } else {
            s_musicHallPending = trackIdx;
        }
    }
}

// The player's craft (0x7FDEFE14 in missions), or 0 when that pointer holds something else:
// a craft's vtable (+0x80) has CraftDestroy (0x7FC3E528).
static u32 s_craftLast = 0;  // playerCraft's cache (a state load clears it)
static bool s_craftLastIsCraft = false;
static u32 playerCraft() {
    u32& last = s_craftLast;
    bool& lastIsCraft = s_craftLastIsCraft;
    const u32 p = rd32(0x7FDEFE14);
    if (p != last) {
        last = p;
        lastIsCraft = false;
        const u32 vt = p ? rd32(p + 0x80) : 0;
        if (vt >= 0x7F000000 && vt < 0x81800000)
            for (u32 o = 0; o < 0x400 && !lastIsCraft; o += 4) lastIsCraft = rd32(vt + o) == 0x7FC3E528;
    }
    return lastIsCraft ? p : 0;
}

// R2_GYRO_LOG: twice a second the player's craft's turn rate - the angle of its rotation (3x3 at
// +0x84) since the last line, whichever way the matrix is stored (trace of R1 R0^T) - against what
// the gyro sent (gyro.cpp logs that): to measure the turn rate a stick deflection gives
static void gyroProbe(u32 craft, bool flying) {
    static const bool log = envFlag("R2_GYRO_LOG");
    if (!log) return;
    static float last[9];
    static u64 lastTb = 0;
    const u64 tb = tbNow();
    if (!craft || !flying) {
        lastTb = 0;
        return;
    }
    if (lastTb && tb - lastTb < kTbClock / 2) return;
    float m[9];
    for (int i = 0; i < 9; i++) {
        const u32 w = rd32(craft + 0x84 + u32(i) * 4);
        memcpy(&m[i], &w, 4);
    }
    if (lastTb) {
        double tr = 0;
        for (int i = 0; i < 9; i++) tr += double(m[i]) * double(last[i]);
        const double ang = std::acos(std::clamp((tr - 1.0) / 2.0, -1.0, 1.0)) * 57.29577951;
        const double dt = double(tb - lastTb) / double(kTbClock);
        // (rows: right, up, forward - heading atan2(m6, m8), pitch asin(m7); y is height)
        logf("gyro", "craft turns %.1f deg/s; m %.3f %.3f %.3f / %.3f %.3f %.3f / %.3f %.3f %.3f", ang / dt, m[0], m[1],
             m[2], m[3], m[4], m[5], m[6], m[7], m[8]);
    }
    memcpy(last, m, sizeof m);
    lastTb = tb;
}

// Skippable cutscenes (settings): the game has no skip for the crawl and most cutscenes, so a
// press of B in a letterboxed cinematic (the VI shows 304 lines there, 450 in flight) runs the
// game flat out until play resumes: retraces at kSkipHz, kSkipStep game seconds per frame
// (8x a 60 fps frame: what cinematics were tested at), nothing drawn (gx_bridge.cpp), the
// audio clock (AI DMA -> MusyX: sequencer, voices, streams) as fast and silent, disc reads
// faster so streams keep up. It ends once the player's craft flies (state +0x370 = 0) in a
// frame that is not letterboxed: between the crawl and the mission intro the game shows full
// frames for a moment. Outside missions, after 3 s of full frames. And after kSkipMaxSeconds in
// any case (issue #38): a skipped scene that leads into a letterboxed screen waiting for a button
// never shows full frames, and the skip ran on at 600 Hz behind "Skipping..." until the player
// closed the game. Skips take 0.7-4.5 s (a crawl and a mission intro ~3 s; #38's log).
constexpr double kSkipStep = 8.0 / 60.0;
constexpr int kSkipHz = 600;
constexpr double kSkipHoldSecs = 2.0;
constexpr double kSkipMaxSeconds = 10.0;
static bool s_skip = false;
static bool s_bootSkipping = false;
static std::atomic<bool> s_skipDraws{false};
static bool s_nativeSceneSkip = false;
static double s_nativeSkipFallbackSecs = 0.0;
bool cinematicSkip() { return s_skipDraws.load(std::memory_order_relaxed); }
void bootSkipInit() {
    if (!settings::get().skipLogos) return;
    s_bootSkipping = true;
    viSetRate(kSkipHz);
    presentPaced(false);
    setDvdBoost(50.0);
}

namespace {
struct CutsceneSkipState {
    std::mutex mu;
    bool active = false;
    bool holding = false;
    float progress = 0.f;
    u64 lastHintTb = 0;
} s_skipUi;
}  // namespace

CutsceneSkipOverlay cutsceneSkipOverlay() {
    std::lock_guard lk(s_skipUi.mu);
    CutsceneSkipOverlay out;
    if (!s_skipUi.active) return out;
    if (s_skipUi.holding) {
        out.visible = true;
        out.holding = true;
        out.progress = s_skipUi.progress;
        out.alpha = 1.f;
        return out;
    }
    if (s_skipUi.lastHintTb) {
        const double age = double(tbNow() - s_skipUi.lastHintTb) / double(kTbClock);
        if (age < 2.2) {
            out.visible = true;
            out.holding = false;
            out.progress = 0.f;
            out.alpha = age < 1.7 ? 1.f : float((2.2 - age) / 0.5);
        }
    }
    return out;
}

// The game has a skip of its own for scenes seen before (this session): SceneSkipAllowed
// (0x7FD56FD8; scene id r3) returns A pressed if SceneSeen (0x7FD56E5C) says so, scenes 13 and 23
// always, and in game mode 5 (the attract demo) as its exit. With skippable cutscenes on, we
// suppress that instant tap-skip outside the attract demo so cutscenes only skip via hold-B.
// For Credits (13), Ending Credits (14) and Audio Test (23), holding B triggers the game's
// native scene exit instead of fast-forwarding so it returns straight to the menu with no
// multi-second "SKIPPING" screen.
static bool standaloneScene() {
    const u32 scene = rd32(0x7FDE822C);
    const u8 screen = rd8(0x800962EF);
    return scene == 13 || scene == 14 || scene == 23 || screen == 0x23 || screen == 0x24;
}

void hle_SceneSkipAllowed(Ctx& c) {
    if (s_nativeSceneSkip) {
        s_nativeSceneSkip = false;
        s_nativeSkipFallbackSecs = 0.0;
        c.r[3] = 1;
        return;
    }
    f_7FD56FD8_orig(c);
    if (standaloneScene()) return;
    if (settings::get().skipCutscenes && (c.r[3] & 0xFF) && rd8(0x800962EF) != 5) {
        c.r[3] = 0;
        std::lock_guard lk(s_skipUi.mu);
        s_skipUi.lastHintTb = tbNow();
    }
}

// Skip intro logos (settings): the boot screen (mode 5, screen 22 at 0x7FDE8254/0x7FDE822C, see
// the top-level loop 0x7FC00030) shows the legal notice, checks the memory card, then plays the
// 3D LucasArts intro and the logos: 16x and silent, still drawn (the card dialogs may need an
// answer), its logic (card checks, loads) as it is.
static bool bootScreen() {
    const u32 mode = rd32(0x7FDE8254);
    return (mode == 5 && rd32(0x7FDE822C) == 22) || (mode == 6 && rd8(0x800962EF) == 0x15);
}

static void keepHealth();
static void keepAmmo();
static void craftDiffProbe();
static void resultsTest(Ctx& c);
static void unlocksFrame();
static void commentaryUpdate(Ctx& c);
static void artGalleryUpdate(Ctx& c);
static void mouseMenuUpdate(Ctx& c);

void hle_GameFrameDelta(Ctx& c) {
    frameDelta(c);
    const settings::Settings& s = settings::get();
    static u64 lastTb = 0;  // real time since the previous frame
    const u64 tb = tbNow();
    const double real = lastTb ? std::max(double(tb - lastTb) / double(kTbClock), 0.0001) : 1.0 / 60.0;
    lastTb = tb;

    const bool cinematic = viXfbLines() < 400;
    const PadState pad = padState(0);
    const bool b = !gfx::menuOpen() && (pad.buttons & 0x0200) != 0;  // PAD_BUTTON_B
    const bool anyBtn = !gfx::menuOpen() && (pad.buttons & 0x1F00) != 0;  // A/B/X/Y/Start
    static bool anyWas = false;
    const bool anyPressed = anyBtn && !anyWas;
    anyWas = anyBtn;

    // Inside f_7FC03074 (boot splash / loading loop, caller 0x7FC03BDC via f_7FC03BC4):
    // - On mode 5 / scene 22 (the black loading screen right before the 3D intro sequence), the game
    //   sets artificial multi-second countdown timers at 0x7FDE85C8 and 0x7FDE8660 even after SSD
    //   disc loading has already finished. With Fast loading (or Skip intro logos), expire them
    //   immediately (keeping 0x7FDE8660 at 0.001f so f_7FD4CFDC(0x7D0) still triggers on this frame).
    // - On mode 0 (the initial legal/Dolby/Nintendo splash screen), expire 0x7FDE85C8 immediately
    //   when Skip intro logos is enabled.
    const bool inBootSplash = (c.lr == 0x7FC03BDCu && rd32(0x7FDE8254u) == 0u);
    if (c.lr == 0x7FC03BDCu) {
        const u32 m = rd32(0x7FDE8254u);
        const u32 sc = rd32(0x7FDE8258u);
        if ((m == 5u && sc == 22u && (s.fastDisc || s.skipLogos)) || (m == 0u && s.skipLogos)) {
            if (s2d(rd32(0x7FDE85C8u)) > 0.0) wr32(0x7FDE85C8u, 0u);
            if (s2d(rd32(0x7FDE8660u)) > 0.001) wr32(0x7FDE8660u, d2s(rsingle(0.001)));
        }
    }

    static double skipHold = 0.0;
    static bool s_skipBootOnce = false;
    const bool inBoot = bootScreen() || inBootSplash;
    const bool inFrontEndMenu = rd32(0x7FDE8254) == 6;  // front-end menus (incl. letterboxed Sound Settings Audio Test)
    const bool inStandaloneScene = standaloneScene();
    if (!inBoot) s_skipBootOnce = false;
    if (!cinematic) {
        s_nativeSceneSkip = false;
        s_nativeSkipFallbackSecs = 0.0;
    }
    const bool canSkipBoot = inBoot && !s.skipLogos && !s_skipBootOnce;
    const bool canSkip = !s_skip && !s_nativeSceneSkip && !inStandaloneScene && s.skipCutscenes &&
                         ((!inFrontEndMenu && cinematic) || canSkipBoot);
    if (canSkip && b) {
        skipHold += real;
    } else {
        skipHold = 0.0;
    }
    {
        std::lock_guard lk(s_skipUi.mu);
        s_skipUi.active = canSkip;
        s_skipUi.holding = canSkip && b;
        s_skipUi.progress = canSkip ? float(std::clamp(skipHold / kSkipHoldSecs, 0.0, 1.0)) : 0.f;
        if (canSkip && anyPressed) s_skipUi.lastHintTb = tb;
        if (!canSkip) s_skipUi.lastHintTb = 0;
    }
    if (canSkip && skipHold >= kSkipHoldSecs) {
        skipHold = 0.0;
        if (canSkipBoot) {
            s_skipBootOnce = true;
            logf("skip", "skipping intro logos (held B)");
        } else {
            s_nativeSceneSkip = true;
            s_nativeSkipFallbackSecs = inStandaloneScene ? 0.0 : 0.25;
            logf("skip", "triggering native scene skip (held B)");
        }
    }
    if (s_nativeSceneSkip && s_nativeSkipFallbackSecs > 0.0) {
        s_nativeSkipFallbackSecs -= real;
        if (s_nativeSkipFallbackSecs <= 0.0) {
            s_nativeSceneSkip = false;
            s_nativeSkipFallbackSecs = 0.0;
            if (!inFrontEndMenu && cinematic) {
                s_skip = true;
                logf("skip", "native skip unused by scene; falling back to fast-forward");
                viSetRate(kSkipHz);
                presentPaced(false);  // (V-Sync would hold the skip to the display's refresh)
                setDvdBoost(50.0);
            }
        }
    }
    const bool inPreIntroBlackLoad = (c.lr == 0x7FC03BDCu && rd32(0x7FDE8254u) == 5u && rd32(0x7FDE8258u) == 22u && s.fastDisc);
    const bool wantBootSkip = ((s.skipLogos || s_skipBootOnce) && inBoot) || inPreIntroBlackLoad;
    if (wantBootSkip != s_bootSkipping) {
        s_bootSkipping = wantBootSkip;
        if (s_bootSkipping) {
            viSetRate(kSkipHz);
            presentPaced(false);
            setDvdBoost(50.0);
        } else if (!s_skip) {
            viSetRateHz(frameRateHz());
            presentPaced(true);
            setDvdBoost(1.0);
        }
    }
    static double fullFor = 0;  // real seconds of frames that are not letterboxed while skipping
    fullFor = s_skip && !cinematic ? fullFor + real : 0;
    static double skipFor = 0;  // real seconds since the skip started
    skipFor = s_skip ? skipFor + real : 0;
    const u32 craft = playerCraft();
    static int flyingFrames = 0;  // (the craft passes through state 0 while it is set up)
    flyingFrames = !cinematic && craft && rd32(craft + 0x370) == 0 ? flyingFrames + 1 : 0;
    const bool skipTooLong = s_skip && skipFor > kSkipMaxSeconds;
    if (s_skip && (!s.skipCutscenes || inFrontEndMenu || flyingFrames >= 10 || fullFor > 3.0 || skipTooLong)) {
        s_skip = false;
        if (skipTooLong)
            logf("skip", "stopped after %.0f s: still %s, no flying craft (a screen waiting for a button?); "
                         "the game goes on at normal speed",
                 skipFor, cinematic ? "letterboxed" : "in full frames");
        else
            logf("skip", "back to play after %.1f s", skipFor);
        viSetRateHz(frameRateHz());
        presentPaced(true);
        setDvdBoost(1.0);
    }
    s_skipDraws.store(s_skip || (s_bootSkipping && !inFrontEndMenu), std::memory_order_relaxed);
    if (s_skip || s_bootSkipping) {
        c.f[1] = c.ps1[1] = rsingle(kSkipStep);
        setAudioSpeed(std::min(kSkipStep / real, 64.0), true);
    } else if (s_moviePaused) {
        setAudioSpeed(1.0, true);
    } else {
        setAudioSpeed(1.0, false);
    }
    keepHealth();
    keepAmmo();
    // R2_MISSION_WIN=<seconds>[:<n>[:<r>]] (tests: a mission's end without flying it, e.g. Strike at
    // the Core's finale, issues #33 / #50): once the player's craft has flown that long, what the
    // mission script's end-mission action (0x2F, ScriptRun's table entry 0x7FC96128) does with a
    // record {2F, 01, r, n}: game object 0x7FDE8000 +0x271 = (r == 0), +0x274 = n + 1, +0x21A = 1.
    // r = 0 is a failure (+0x274: the reason, "FrntEnd_missionFailed_level10_<n+1>"; Strike at the
    // Core's 2F 01 00 00 = "The Millennium Falcon was destroyed"), r = 1 a win.
    if (static const char* win = getenv("R2_MISSION_WIN"); win && *win) {
        static double flown = 0;
        static bool done = false;
        flown = craft && !cinematic && rd32(craft + 0x370) == 0 ? flown + real : flown;
        if (!done && flown >= atof(win)) {
            done = true;
            u32 n = 0, r = 0;
            if (const char* c1 = strchr(win, ':')) {
                n = u32(atoi(c1 + 1));
                if (const char* c2 = strchr(c1 + 1, ':')) r = u32(atoi(c2 + 1));
            }
            constexpr u32 kGame = 0x7FDE8000;
            if (rd8(kGame + 0x21A) == 0) {
                wr8(kGame + 0x271, r == 0 ? 1 : 0);
                wr32(kGame + 0x274, n + 1);
                wr8(kGame + 0x21A, 1);
                logf("test", "R2_MISSION_WIN: end-mission action {2F 01 %02X %02X} after %.0f s of flight", r, n, flown);
            }
        }
    }
    craftDiffProbe();
    resultsTest(c);
    unlocksFrame();
    commentaryUpdate(c);
    artGalleryUpdate(c);
    mouseMenuUpdate(c);
    // widescreen: the flight HUD goes to the screen edges (not the menus: their layout is 4:3).
    // Nor the pause screen, drawn while the craft exists: its objectives' check marks and dots
    // sit left of the centered text and were moved out to the left (by the 4:3 margin, 240 px
    // at 1080p). GameState 0x7FDE8668 (setter 0x7FC03CD8): 0 in play, 2 paused (0x7FCD95D4,
    // back to 0 at 0x7FCD96B8), 3 in front-end loops; found diffing dumps flying / paused.
    const bool inPlay = rd32(0x7FDE8668) == 0;
    // gyro aiming steers only the craft in flight: menus, the pause screen and cutscenes read the
    // main stick too
    gyro::setFlight(flyingFrames > 0 && inPlay && !s_skip);
    gyroProbe(craft, flyingFrames > 0);
    gfx::setHudAnchoring(s.hudEdges && s.aspect != settings::Aspect::R4_3 && playerCraft() && !cinematic && inPlay);
    // menu screens only (no player craft; not the boot screen's intro and logos, whose 2D draws
    // also look like a run of panels, nor letterboxed cinematics): the background's panels
    // repeat into the sides
    gfx::setMenuTiling(s.tileMenus && !playerCraft() && !bootScreen() && !cinematic);
    // Discord Rich Presence (discord.cpp names the mission from the level the game loaded): the
    // front end (the boot screen, GameState 3 in its loops, game mode 5 - byte r13-19121 =
    // 0x800962EF, tested by the attract demo's exit 0x7FD79648), paused (GameState 2 with the
    // craft: checked first, the pause screen may be letterboxed), a letterboxed cinematic (the
    // crawl, briefings, cutscenes; also while the port skips one), flying (the player's craft
    // exists), else none (loading, the results).
    {
        using discord::Phase;
        const u32 gameState = rd32(0x7FDE8668);
        Phase phase = Phase::None;
        if (bootScreen() || rd8(0x800962EF) == 5) phase = Phase::FrontEnd;
        else if (craft && gameState == 2 && !s_skip) phase = Phase::Paused;
        else if (cinematic || s_skip) phase = Phase::Cutscene;
        else if (gameState == 3) phase = Phase::FrontEnd;
        else if (craft) phase = Phase::Flying;
        discord::gameFrame(phase);
    }
    // R2_DT_LOG: every 10 s, frames and the game time they advanced against the wall clock
    static const bool dtLog = envFlag("R2_DT_LOG");
    if (!dtLog) return;
    static u64 logTb = 0, frames = 0;
    static double simulated = 0;
    if (!logTb) logTb = tb;
    frames++;
    simulated += c.f[1];
    if (tb - logTb >= 10 * kTbClock) {
        logf("dt", "%llu frames, %.2f s game time in %.2f s", (unsigned long long)frames, simulated,
             double(tb - logTb) / kTbClock);
        logTb = tb;
        frames = 0;
        simulated = 0;
    }
}

// Widescreen at the source. Aurora widens perspective projections on a wider target, but the
// game culls with its own 4:3 view volume: the world camera (0x7FCDA190) passes 4/3 from its
// small-data constant to the camera setup 0x7FC14CEC, which keeps the aspect (camera +0x12C),
// derives the horizontal field of view (+0x130) and builds the projection from them
// (0x7FD43668 -> MTXPerspective). Each camera function has its own copy of the constant; the
// ones below take the display aspect, so the game renders and culls for it. Aurora leaves
// projections that are already wide alone (Aurora patch 0009).
static std::atomic<float> s_displayAspect{4.f / 3.f};
// Aspects written into the constants before (the window was resized, or the setting changed):
// camera objects copied the default when they were created and keep passing the old value.
static std::atomic<float> s_oldAspects[8];
static std::atomic<u32> s_oldAspectCount{0};
constexpr u32 k43 = 0x3FAAAAAB;  // 4/3
static u32 s_aspectWritten = k43;  // the value setGameAspect last put into the constants

static bool oldGameAspect(double aspect) {
    const u32 n = std::min<u32>(s_oldAspectCount.load(std::memory_order_relaxed), 8);
    for (u32 i = 0; i < n; i++)
        if (std::abs(aspect - double(s_oldAspects[i].load(std::memory_order_relaxed))) < 0.0005) return true;
    return false;
}

void setGameAspect(double aspect) {
    const float previous = s_displayAspect.exchange(float(aspect), std::memory_order_relaxed);
    if (previous != float(aspect) && std::abs(previous - 4.0 / 3.0) > 0.0005 && !oldGameAspect(previous)) {
        const u32 i = s_oldAspectCount.fetch_add(1, std::memory_order_relaxed);
        s_oldAspects[i % 8].store(previous, std::memory_order_relaxed);
    }
    // (The flight camera culls with the camera-object default. HUD cameras built from it, such
    // as the radar globe's, aim off-center; Aurora keeps that offset in the 4:3 HUD layout.
    // 0x8009935C, the HUD's own MTXPerspective calls, stays 4:3.)
    static const u32 kCameraAspect[] = {
        0x80096C64,  // 0x7FC150B0: camera object defaults (+0x274)
        0x80099444,  // 0x7FCD3C38: camera object (+0x58)
        0x800994F4,  // 0x7FCD771C
        0x80099548,  // 0x7FCDA190: the world camera
    };
    u32& written = s_aspectWritten;
    const float f = float(aspect);
    u32 bits;
    memcpy(&bits, &f, 4);
    if (bits == written) return;
    for (u32 a : kCameraAspect) {
        const u32 cur = rd32(a);
        if (cur == k43 || cur == written) wr32(a, bits);
        else logf("wide", "camera aspect at %08X is %08X, left alone", a, cur);
    }
    written = bits;
    logf("wide", "game camera aspect %.4f", aspect);
}

// CameraSetup (0x7FC14CEC; view r3, matrix r4, aspect f1, vertical fov f2 in degrees) derives a
// camera's culling volume - a pyramid from the camera to the far plane's corners (+0x138..+0x168,
// side planes after them; horizontal fov +0x130) - from the aspect and fov its camera object
// holds (+0x274/+0x270), and keeps them (+0x12C, +0x134) for the projection 0x7FC153C0 builds
// next. Not every camera takes the patched constants above: the LucasArts intro's has its own
// 1.3333, letterboxed cinematics and the hangar use 2.105 (4/3 * 480/304) and 4/3. Aurora shows
// such a projection opened up to fill the screen (patch 0009, widescreen_scale): with k = display
// aspect / (4/3) and v = 480 / the XFB's lines in a letterboxed frame (1 otherwise), r = k / v
// opens it r times wider if r > 1, 1/r times taller if r < 1.
//
// So the culling volume is computed for that opened view, and outside free flight (intro,
// menus, the hangar, cinematics) kSceneMargin times wider and taller still for objects (the
// frustum planes): objects whose bounds sit just outside the edge while part of them shows (a
// trooper's feet below the screen) no longer drop out. The margin stays small and the
// horizontal fov (+0x130, which e.g. the terrain grid walk 0x7FC269C4 sizes its area by) stays
// the screen's: a 3x margin overflowed the game's per-frame buffers (heap chunk lists freed
// with bad pointers in the logos and the menus). In missions the flight camera (already at the
// display aspect: setGameAspect) and the rest take kTerrainMargin, the horizontal fov included
// (the terrain, below).
// The projection: a full frame opened sideways gets the game's aspect widened r times with the
// game's fov (4:3 -> the display's aspect; a cinematic camera's 2.105 drawn inside a full
// 450-line frame, like the take-off, -> 2.105 r), which Aurora shows as it is (the same
// picture; anything else the game tests against the projection sees the whole screen);
// otherwise (taller, letterboxed) the game's own aspect and fov, which Aurora opens. The
// camera's own copy of the projection (+0xA4) follows: the game places the engine glows with
// it. R2_GAME_CULLING=1 keeps the game's own volumes, R2_CULL_MARGIN=<f> sets the margin (1 =
// exactly the screen), R2_CAM_LOG=1 logs the camera setups.
constexpr double kSceneMargin = 1.2;
constexpr double kTerrainMargin = 1.2;

// CameraHFovAtan (0x7FC8DE3C; only caller CameraSetup, 0x7FC14D30): atan of f1 clamped to
// [-1, 1]. CameraSetup takes the horizontal fov (+0x130) as 2 * this(aspect * tan(fovY / 2)), so
// it never passes 90 degrees, and the far-plane corners and side planes derived from it neither:
// plenty at 4:3 (the cockpit's 55-degree view is 69.5 wide), but the cockpit at 2.13:1 shows 96
// degrees (106 with the margin) and the Death Star's terrain (TerrainDraw 0x7FCBCCC4 lays out
// the tiles inside the hull of the camera and the far corners seen from above) left the sides
// out in a sawtooth, at 32:9 all but a wedge in the middle. While hle_CameraSetup sets up a widened volume in a mission the full atan
// (0x8005348C, the SDK's) is taken, up to 70 degrees (140 wide): the terrain span array (4x its
// rows, hle_HeapAlloc) holds a footprint twice the far corners' distance across, which stays
// under ~250 of its 284 rows there. The heightmap terrain (Hoth) sizes its own table the same
// way and overflowed it in the cockpit at 21:9: hle_TerrainRows.
static bool s_wideVolume = false;
constexpr double kMaxHalfTan = 2.7474774;  // tan(70 degrees)

void hle_CameraHFovAtan(Ctx& c) {
    static const bool keepCap = envFlag("R2_HFOV_CAP");  // tests: the game's 90-degree cap
    if (s_wideVolume && !keepCap && std::abs(c.f[1]) > 1.0) {
        c.f[1] = std::clamp(c.f[1], -kMaxHalfTan, kMaxHalfTan);
        f_8005348C(c);
        return;
    }
    f_7FC8DE3C_orig(c);
}

// MTXPerspective (SDK 0x8002FEA4; fovY f1, aspect f2, near f3, far f4). The HUD's cameras (the
// radar globe: 0x7FCD22C4, 0x7FCD4F60, 0x7FCD50F4) build their projections here, not through
// CameraSetup, from the aspect their objects copied when the mission started: after a window
// resize they kept the old window's (2.37 at 1280x1024: the globe drew squeezed). An aspect
// setGameAspect wrote before is a widened 4:3 one, like in hle_CameraSetup: today's instead.
void hle_MTXPerspective(Ctx& c) {
    const double aspect = c.f[2];
    const double display = s_displayAspect.load(std::memory_order_relaxed);
    if (std::abs(aspect - display) >= 0.0005 && oldGameAspect(aspect)) c.f[2] = c.ps1[2] = rsingle(display);
    f_8002FEA4_orig(c);
}

void hle_CameraSetup(Ctx& c) {
    double aspect = c.f[1];
    const double fov = c.f[2];
    const double display = s_displayAspect.load(std::memory_order_relaxed);
    // A camera object made before the window was resized (or the aspect setting changed) still
    // holds the aspect it copied then: it is a widened 4:3 camera like the rest, so it takes
    // today's. (It kept the old one: the flight camera's culling was opened from it and the
    // radar globe drew squeezed.)
    if (std::abs(aspect - display) >= 0.0005 && oldGameAspect(aspect)) {
        aspect = display;
        c.f[1] = c.ps1[1] = rsingle(display);
    }
    // R2_CAM_LOG=1: each camera setup (view, caller, aspect, fov) when it changes
    static const bool camLog = envFlag("R2_CAM_LOG");
    if (camLog) {
        struct Seen {
            u32 view, lr;
            float aspect, fov;
        };
        static std::vector<Seen> seen;
        const Seen s{c.r[3], c.lr, float(aspect), float(fov)};
        Seen* e = nullptr;
        for (Seen& x : seen)
            if (x.view == s.view && x.lr == s.lr) e = &x;
        if (!e || e->aspect != s.aspect || e->fov != s.fov) {
            if (e) *e = s;
            else if (seen.size() < 256) seen.push_back(s);
            logf("cam", "setup view %08X from %08X: aspect %.4f fov %.2f (display %.4f, xfb %u lines)", s.view, s.lr,
                 aspect, fov, display, viXfbLines());
        }
    }
    static const bool gameCulling = envFlag("R2_GAME_CULLING");
    if (gameCulling || display < 4.0 / 3.0 + 0.01 || !(aspect > 0.5 && aspect < 8.0) || !(fov > 1.0 && fov < 170.0)) {
        f_7FC14CEC_orig(c);
        return;
    }
    const u32 view = c.r[3];
    const u32 matrix = c.r[4];
    const u32 craft = playerCraft();
    // already at the display's aspect (setGameAspect: the flight and HUD cameras): not opened
    const bool widened = std::abs(aspect - display) < 0.002;
    const u32 lines = viXfbLines();
    const double k = widened ? 1.0 : display * 0.75;
    const double v = !widened && lines && lines < 408 ? 480.0 / lines : 1.0, r = k / v;
    static const double sceneMargin = [] {
        const char* e = getenv("R2_CULL_MARGIN");
        return e ? std::clamp(atof(e), 1.0, 20.0) : kSceneMargin;
    }();
    static const double terrainMargin = [] {
        const char* e = getenv("R2_TERRAIN_MARGIN");
        return e ? std::clamp(atof(e), 1.0, 3.0) : kTerrainMargin;
    }();
    // In a letterboxed frame every camera culls for at least the letterboxed picture: the hangar
    // (ship select) renders at 2.105 but sets the camera up once more per frame at 4:3, and that
    // volume decides what is there (the bay's sides popped in as the camera panned).
    const double base = v > 1.0 ? std::max(aspect, 4.0 / 3.0 * v) : aspect;
    const double margin = craft ? terrainMargin : sceneMargin;
    // in missions the horizontal fov may pass 90 degrees (hle_CameraHFovAtan)
    struct WideVolume {
        explicit WideVolume(bool on) { s_wideVolume = on; }
        ~WideVolume() { s_wideVolume = false; }
    } wideVolume(craft != 0);
    constexpr double kRad = 3.14159265358979323846 / 180.0;
    auto setup = [&](double m) {  // the volume opened for the screen, m times more
        const double wider = std::max(1.0, r) * m, taller = std::max(1.0, 1.0 / r) * m;
        c.r[3] = view;  // (the call keeps the non-volatile registers; arguments again)
        c.r[4] = matrix;
        c.f[1] = c.ps1[1] = rsingle(base * wider / taller);
        c.f[2] = c.ps1[2] = rsingle(std::min(2.0 * std::atan(std::tan(fov * 0.5 * kRad) * taller) / kRad, 170.0));
        f_7FC14CEC_orig(c);
    };
    if (widened && margin == 1.0) {
        f_7FC14CEC_orig(c);
        return;
    }
    // Outside missions the horizontal fov stays the screen's (see above); in missions it takes
    // the margin too: the terrain (the Death Star's surface) is laid out from the volume, whole
    // tiles inside it, and the tiles at the sides dropped out in a sawtooth just inside the edges
    // (and past 90 degrees everywhere outside that: hle_CameraHFovAtan).
    u32 hfov = 0;
    if (margin != 1.0 && !craft) {
        setup(1.0);
        hfov = rd32(view + 0x130);
    }
    setup(margin);
    if (hfov) wr32(view + 0x130, hfov);
    // The projection. A full frame opened sideways: the game's view widened r times (4:3 ->
    // the display's aspect; a cinematic camera's 2.105 inside a full frame -> 2.105 r), which
    // Aurora is told to show as it is. Otherwise (letterboxed, or a display taller than the
    // camera) the game's own, which Aurora opens.
    const bool sideways = v == 1.0 && r >= 1.0;
    const double projAspect = sideways ? aspect * r : aspect;
    if (sideways) gfx::noteWidenedAspect(float(projAspect));
    wr32(view + 0x12C, d2s(projAspect));
    wr32(view + 0x134, d2s(fov));
    // The camera keeps its own copy of the projection (+0xA4, and +0xE4 derived from it; built
    // by 0x7FC14B8C from the aspect, fov and near/far): the game projects points with it, e.g.
    // to place the engine glows. Built for the culling volume above, it put them closer to the
    // center than the ships' engines; rebuilt here for the projection that is shown.
    c.r[3] = view;
    c.r[4] = view + 0xA4;
    c.r[5] = view + 0xE4;
    f_7FC14B8C(c);
}

// The terrain's span array. TerrainSetup (0x7FD89210) allocates it at a mission's start with
// one 4-byte (left, right) pair per grid row, as many rows as the view distance spans for the
// game's own 4:3 view; every frame TerrainSpans (0x7FCBAA0C) rasterizes the view footprint onto
// the terrain grid and writes one pair per row it covers, with no bound. Widened views (the
// display's aspect, the terrain margin above) cover more rows: the pairs ran past the array
// into the next heap block's header (the tile records, 0x80095FA4), and the game crashed in
// HeapFree when the mission ended (or later). That one allocation (this return address) gets
// four times the room, enough for 32:9 with the margin.
// The heightmap terrain's visible-tile list (HeightTerrainLists 0x7FD8AA18, this return address)
// gets four times its 0xC000 bytes too: hle_HeightTerrainTiles.
constexpr u32 kTileListRet = 0x7FD8AA58u, kTileListBytes = 0xC000, kTileListScale = 4;
static u32 s_tileListRecords = kTileListBytes / 12;
static u32 s_heightRowsExtra = 0;  // rows hle_TerrainRows added for HeightTerrainSetup's block

// The enlarged tables live outside the game's heap. Taken from it (MEM1, 22 MiB, mission data
// filling most of it), the extra room moved every block allocated after them, and the heap ran
// short where it never does on hardware: the user's Revenge on Yavin (bonus row) stopped ~5
// minutes in with "*** OUT OF MEMORY (size 530352, free 1019424)! ***" from the loader thread
// (0x7FD5B1BC, a level resource read in 256 KiB pieces), which then read the disc into the null
// pointer it got (fault at 0x0002F880). Their state, 22 s into that mission: 962 KB free in 23
// pieces, the largest 485 KB, and the 4x visible-tile list alone took 147 KB more than the game
// planned. So each table takes its original size from the game's heap, as on hardware (the heap's
// layout and history stay the game's own), and the game gets an enlarged block of the port's
// (kPortArena, past the end of MEM1, saved in states) whose header copies the original's;
// HeapFree of it frees the original. One slot per table: a mission sets each up once.
struct PortTable {
    const char* name;
    u32 lr;                // HeapAlloc's return address in the table's setup
    u32 offset, room;      // the slot in the port arena (data at kPortArena + offset)
    u32 block = 0, size = 0;  // the game's own (original-size) block, the enlarged size
};
static PortTable s_portTables[] = {
    {"terrain span array", 0x7FD893E8u, 0x00100, 0x0FF00},   // TerrainSetup (the Death Star)
    {"heightmap terrain rows", 0x7FD8A3C8u, 0x10100, 0x2FF00},  // HeightTerrainSetup (~95 KB in Yavin)
    {"heightmap visible-tile list", kTileListRet, 0x40100, kTileListBytes * kTileListScale},  // HeightTerrainLists
};
static_assert(0x40100 + kTileListBytes * kTileListScale <= kPortArenaSize);

void hle_HeapAlloc(Ctx& c) {
    const u32 lr = c.lr;
    u32 original = 0, enlarged = 0;
    if (lr == 0x7FD893E8u) [[unlikely]] {
        original = c.r[3], enlarged = c.r[3] * 4;
        logf("wide", "terrain span array: room for %u rows instead of %u", enlarged / 4, original / 4);
    } else if (lr == kTileListRet && c.r[3] == kTileListBytes) [[unlikely]] {
        original = c.r[3], enlarged = c.r[3] * kTileListScale;
        s_tileListRecords = enlarged / 12;
        logf("wide", "heightmap terrain: room for %u visible tiles instead of %u", s_tileListRecords, original / 12);
    } else if (lr == 0x7FD8A3C8u && s_heightRowsExtra && c.r[3] > s_heightRowsExtra * 6) [[unlikely]] {
        // rows * 6 + n * 86 bytes: the table (4 bytes a row), a 2-byte array by rows, records
        enlarged = c.r[3], original = c.r[3] - s_heightRowsExtra * 6;
        s_heightRowsExtra = 0;
    }
    PortTable* t = nullptr;
    for (auto& e : s_portTables)
        if (enlarged && e.lr == lr) t = &e;
    if (!t || enlarged > t->room) [[likely]] {
        if (t) logf("wide", "%s: %u bytes do not fit the port's %u: taken from the game's heap", t->name, enlarged, t->room);
        if (enlarged) c.r[3] = enlarged;
        f_800070C4_orig(c);
        return;
    }
    const u32 flags = c.r[4];
    c.r[3] = original;
    f_800070C4_orig(c);  // the game's own block, as on hardware
    const u32 block = c.r[3];
    if (!block) return;  // out of memory: the game's own handling
    if (t->block) logf("wide", "%s: the previous one (game block %08X) was never freed", t->name, t->block);
    const u32 p = kPortArena + t->offset;
    for (u32 o = 0; o < 0x20; o += 4) wr32(p - 0x20 + o, rd32(block - 0x20 + o));  // its header
    if (flags & 0x100) memset(g_base + p, 0, enlarged);  // HeapAlloc's clear flag
    t->block = block, t->size = enlarged;
    c.r[3] = p;
    logf("wide", "%s: %u bytes at %08X (the game's %u-byte block at %08X stays as on hardware)", t->name, enlarged, p,
         original, block);
}

// The game's heap extension. Vengeance on Kothlis (the user's session, 16:10, ~5 minutes into
// the mission) stopped with "*** OUT OF MEMORY (size 4096, free 3136)! ***" twice, then a
// store through the null pointer it got (fatal at address 0, guest lr 0x7FC0ED1C): the MEM1
// heap had 772 KB free in 9 pieces in the user's save state two minutes before, 3 KB then.
// HeapAllocFrom (0x80006CD0: the game's heap record r3 - MEM1 0x80068088, VM 0x800680A4 -,
// size r4 rounded to 32, report r5), HeapAlloc's only way into OSAllocFromHeap (0x80028AC4),
// prints that line when the allocation fails with report set (OSCheckHeap's free count), calls
// 0x800074F8(0, 3) and returns 0, which HeapAlloc's callers do not check. Callers that can live
// with a failure pass flag 0x40000 (report 0): they still get 0, as on hardware. The others now
// get a block of the extension: 8 MiB right after MEM1 (kMem1Ext, runtime.h), as if the
// console had 32 MiB, but only once the game's own heap has no piece that fits - until then
// its layout and history are the game's own, as on hardware. Each block has the SDK's 32-byte
// cell header (prev and next 0, the cell's size), behind which HeapAlloc writes its fields as
// for any block (size, flags, heap record: the MEM1 one); HeapFree gives them back here
// instead of to OSFreeToHeap (they are in none of the SDK's lists). First fit by address, like
// OSAlloc. Logged as "heap" lines with HeapAlloc's caller; kept in save states (version 4).
constexpr u32 kHeapExtBase = 0x80000000u + kMem1Size, kHeapExtEnd = 0x80000000u + kMem1Mapped;
constexpr u32 kMem1HeapRecord = 0x80068088u;
static std::map<u32, u32> s_extFree, s_extUsed;  // cell address -> cell size (header included)
static u32 s_extInUse = 0, s_extPeak = 0;
struct ExtCell {
    u32 cell, size;
};  // (save states)

static u32 heapExtAlloc(u32 size) {
    if (s_extFree.empty() && s_extUsed.empty()) s_extFree[kHeapExtBase] = kMem1Ext;  // first use
    if (size > kMem1Ext) return 0;
    const u32 need = (size + 0x20 + 0x1F) & ~0x1Fu;
    for (auto it = s_extFree.begin(); it != s_extFree.end(); ++it) {
        if (it->second < need) continue;
        const u32 cell = it->first, left = it->second - need;
        u32 take = need;
        s_extFree.erase(it);
        if (left >= 0x40) s_extFree[cell + need] = left;  // (OSAlloc's smallest cell)
        else take += left;
        s_extUsed[cell] = take;
        wr32(cell, 0), wr32(cell + 4, 0), wr32(cell + 8, take);
        s_extInUse += take;
        return cell + 0x20;
    }
    return 0;
}

static void heapExtFree(u32 p) {
    const auto used = s_extUsed.find(p - 0x20);
    if (used == s_extUsed.end()) {
        logf("heap", "HeapFree of %08X in the heap extension: not a block of it (ignored)", p);
        return;
    }
    u32 cell = used->first, size = used->second;
    s_extUsed.erase(used);
    s_extInUse -= size;
    if (size >= 0x40000)  // (level data, at a mission's end)
        logf("heap", "heap extension: %u KiB at %08X freed (%u KiB in use)", size >> 10, p, s_extInUse >> 10);
    auto next = s_extFree.lower_bound(cell);
    if (next != s_extFree.end() && cell + size == next->first) {
        size += next->second;
        next = s_extFree.erase(next);
    }
    if (next != s_extFree.begin()) {
        if (auto prev = std::prev(next); prev->first + prev->second == cell) {
            prev->second += size;
            cell = 0;
        }
    }
    if (cell) s_extFree[cell] = size;
    if (!s_extInUse) logf("heap", "heap extension empty again (the game's heap has room)");
}

void hle_HeapAllocFrom(Ctx& c) {
    const u32 heap = c.r[3], size = c.r[4], report = c.r[5];
    if (heap != kMem1HeapRecord || !report) {
        f_80006CD0_orig(c);
        return;
    }
    // R2_HEAP_EXT_TEST=<n> (tests): every n-th of these goes to the extension with the heap not
    // full - blocks there read from the disc, drawn from, run (code modules) and freed
    static const u32 testEvery = [] {
        const char* e = getenv("R2_HEAP_EXT_TEST");
        return e ? u32(strtoul(e, nullptr, 10)) : 0u;
    }();
    static u32 testCount = 0;
    if (!testEvery || ++testCount % testEvery) {
        c.r[5] = 0;  // no report yet: the extension may take it
        f_80006CD0_orig(c);
        if (c.r[3]) return;
    }
    // HeapAlloc's frame is r1 (stwu -0x30): its caller's return address was saved at +0x34
    const u32 caller = rd32(c.r[1] + 0x34);
    const u32 before = s_extInUse;
    if (const u32 p = heapExtAlloc(size)) {
        // the first block since it was empty, then every new 256 KiB of peak use
        if (!before || s_extInUse >= s_extPeak + 0x40000) {
            logf("heap", "MEM1 heap full: %u bytes at %08X from the heap extension for %08X (%u KiB of %u in use)", size, p,
                 caller, s_extInUse >> 10, kMem1Ext >> 10);
            s_extPeak = std::max(s_extPeak, s_extInUse);
        }
        c.r[3] = p;
        return;
    }
    logf("heap", "MEM1 heap full and the heap extension too (%u bytes for %08X, %u KiB in use)", size, caller,
         s_extInUse >> 10);
    c.r[3] = heap, c.r[4] = size, c.r[5] = report;
    f_80006CD0_orig(c);  // the game's own report, and 0
}

// HeapFree (0x80007270, ptr r3): a port table's block frees the game's own; a block of the heap
// extension goes back to it.
void hle_HeapFree(Ctx& c) {
    const u32 p = c.r[3];
    if (p >= kHeapExtBase && p < kHeapExtEnd) [[unlikely]] {
        heapExtFree(p);
        return;
    }
    if (p >= kPortArena && p < kPortArena + kPortArenaSize) [[unlikely]] {
        PortTable* t = nullptr;
        for (auto& e : s_portTables)
            if (p == kPortArena + e.offset) t = &e;
        if (!t || !t->block) {
            logf("wide", "HeapFree of %08X in the port arena: not a block of it (ignored)", p);
            return;
        }
        logf("wide", "%s freed (the game's block %08X)", t->name, t->block);
        c.r[3] = t->block;
        t->block = 0, t->size = 0;
    }
    f_80007270_orig(c);
}

// The heightmap terrain (Hoth and the other ground missions; state 0x7FDF5E60) has the same
// kind of span table, sized by TerrainRows (0x7FC92B7C: the footprint of a view 41 degrees to
// each side, at the mission's view distance) - but HeightTerrainSetup (0x7FD8A240) allocates it
// in one block with arrays laid out after it by that row count (+0x248 = table + rows * 4, then
// 86-byte records), so its row count itself gets four times the rows. The widened cockpit view
// (21:9: 112 degrees with the margin, past the game's 90-degree cap: hle_CameraHFovAtan) spans
// more rows than the game's count: HeightTerrainSpans (0x7FCBD92C, no bound) wrote past the
// table into those arrays, and the walk over the heightmap (0x7FCBE5F0) then met an inverted
// span (left > right): a row ends only when the column equals its end, so it slid down from
// MEM1 through the whole VM, every page it touched taking one of the pager's 1536 ARAM slots:
// "FATAL VM ERROR: Out of ARAM memory" seconds into a fight (or a call through a pointer the
// overflow had overwritten: 0x4D657461, "Meta"). The Death Star's TerrainSetup (0x7FD893BC)
// keeps its count (its table is a block of its own: hle_HeapAlloc).
void hle_TerrainRows(Ctx& c) {
    const u32 lr = c.lr;
    f_7FC92B7C_orig(c);
    if (lr == 0x7FD8A308u) {
        logf("wide", "heightmap terrain: room for %u rows instead of %u", c.r[3] * 4, c.r[3]);
        s_heightRowsExtra = c.r[3] * 3;  // its block comes from the port (hle_HeapAlloc)
        c.r[3] *= 4;
    }
}

// HeightTerrainSpans (0x7FCBD92C): uses rows 0..[+0x23C] of the span table ([+0x240] rows).
// Logs the most used so far (every 8 more) and, once, a footprint the table cannot hold.
static u32 s_heightRowsMax = 0;
void hle_HeightTerrainSpans(Ctx& c) {
    f_7FCBD92C_orig(c);
    constexpr u32 kState = 0x7FDF5E60;
    const u32 used = rd32(kState + 0x23C) + 1, rows = rd32(kState + 0x240);
    if (used >= s_heightRowsMax + 8 || (used > rows && s_heightRowsMax <= rows)) {
        logf("wide", "heightmap terrain: %u of %u span rows used%s", used, rows,
             used > rows ? " - PAST THE TABLE (memory after it overwritten)" : "");
        s_heightRowsMax = used;
    }
}

// HeightTerrainTiles (0x7FCC0A00): every tile inside the spans goes into the +0xC0 list (12-byte
// records, u16 indices from 1, chained per material through the buckets at +0xBC: u16 head, tail,
// 6 bytes each, 384 of them, cleared each frame), with no capacity check, and is then drawn
// through the jump table 0x7FDCB5A8 by its record's level byte. The game's 0xC000 bytes hold
// 4096; the widened cockpit at 21:9 saw more, the list ran on into the next heap block, and a
// record read back as garbage called 0x4D657461 or 0x30000000 (0x7FCC09E0). hle_HeapAlloc gives
// it four times the room (16384 records; the indices stay u16). The largest bucket tail after the
// call is the number of records used: the most so far is logged (every 512 more), and once a
// count the list cannot hold.
static u32 s_tilesMax = 0;
void hle_HeightTerrainTiles(Ctx& c) {
    f_7FCC0A00_orig(c);
    constexpr u32 kState = 0x7FDF5E60;
    const u32 buckets = rd32(kState + 0xBC);
    if (!buckets) return;
    u32 used = 0;
    for (u32 i = 0; i < 0x900 / 6; i++) used = std::max(used, rd16(buckets + i * 6 + 2));
    if (used >= s_tilesMax + 512 || (used > s_tileListRecords && s_tilesMax <= s_tileListRecords)) {
        logf("wide", "heightmap terrain: %u of %u visible tile records used%s", used, s_tileListRecords,
             used > s_tileListRecords ? " - PAST THE LIST (memory after it overwritten)" : "");
        s_tilesMax = used;
    }
}

// AreaTriggerUpdate (0x7FC23008; object r3, position r4) runs after an object with a trigger list
// moves (the player's craft after each step, 0x7FC36168). The list (+0x34) holds object indices
// (low half, from 1, into [[0x7FDECF10] + 0x54]; bit 31 = inside; 0xFFFFFFFF ends it). An area
// trigger is a level record read in place: half 0x001F, byte +2 the shape (6 box, 7 cylinder of
// radius +0x1C), id +4, min corner +0x10 and size +0x28 (x, y, z); +0x3C an event bit and
// +0x40/+0x44 a counter threshold that must hold first, +0x48 nonzero = height ignored, +0x4C the
// actions on entering (event bit, value, counter, amount, script offset from the record), +0x60
// those on leaving. Prisons of the Maw lays two gates across the approach to the prison's force
// field, x -2000..3000, y 4300..7300, 100 deep: 0x82 at z 4900 (counter 21: two voice lines) and
// 0x7C at z -100 (counter 15: the force field lines, then "You should switch to ion cannons").
// The user flew over 0x7C at y ~7500 and never heard them; asked for "a wall the player flies
// through", both get the game's own ignore-height flag (that mission's 0x7D has it): full height,
// the width and depth as they were.
static void mawGateWalls(u32 list) {
    struct Gate { u32 id, minZ, counter; };
    static constexpr Gate kGates[] = {{0x7C, 0xC2C80000u /* -100 */, 0x0004000Fu},
                                      {0x82, 0x45992000u /* 4900 */, 0x00040015u}};
    u32 table = 0;
    for (u32 i = 0; i < 4096; i++) {
        const u32 w = rd32(list + i * 4);
        if (w == 0xFFFFFFFFu) break;
        const u32 index = w & 0xFFFF;
        if (!index) continue;
        if (!table) table = rd32(rd32(0x7FDECF10) + 0x54);  // read as the game does, for a list with entries
        const u32 rec = rd32(table + (index - 1) * 4);
        if (rd32(rec) != 0x001F0600u || rd32(rec + 0x48) != 0) continue;
        const u32 id = rd32(rec + 4) >> 16;
        for (const Gate& g : kGates) {
            if (id != g.id || rd32(rec + 0x18) != g.minZ || rd32(rec + 0x54) != g.counter) continue;
            if (rd32(rec + 0x10) != 0xC4FA0000u || rd32(rec + 0x14) != 0x45866000u ||  // -2000, 4300
                rd32(rec + 0x28) != 0x459C4000u || rd32(rec + 0x2C) != 0x453B8000u ||  // 5000, 3000
                rd32(rec + 0x30) != 0x42C80000u)                                        // 100
                continue;
            wr32(rec + 0x48, 1);
            logf("game", "Prisons of the Maw: gate %X (counter %u) reaches from the ground to the sky (record %08X)",
                 id, g.counter & 0xFFFF, rec);
        }
    }
}

void hle_AreaTriggerUpdate(Ctx& c) {
    if (const u32 list = rd32(c.r[3] + 0x34)) mawGateWalls(list);
    f_7FC23008_orig(c);
}

// Prisons of the Maw's force field: two hexagons (OBJmawShieldCore, vtable 0x7FDD1CD0; a list
// from 0x80096054 = r13-0x4D4C, next +0x104), each six triangular panels: centre +0xA8,
// generator i, generator i+1 (count +0xE0, their level records +0xE4, position at record +0x10).
// MawShieldCoreUpdate (0x7FD02AAC) flags generator i at +0x1F8 + i while its OBJmawShieldGen's
// +0x104 is above 0 (ion cannons set it: disabled) and the hexagon at +0xFC (down) once fewer
// than 3 of its generators run. MawShieldCoreDraw (0x7FD02BE8) draws a panel only if neither of
// its generators is flagged, but MawShieldCollide (0x7FD02EC0: the ray test for craft and shots)
// tests every panel of a hexagon that is not down: a panel next to a disabled generator vanishes
// and stays solid (the user flew into the gap and crashed). The game's own code does that on
// hardware too; as the user asked, the panel test (MawShieldPanelHit 0x7FD02CC0, called only from
// there: ray r3/r4, centre r5, generator positions r6/r7) now misses the panels the draw leaves
// out.
void hle_MawShieldPanelHit(Ctx& c) {
    const u32 core = c.r[5] - 0xA8;
    const u32 n = rd32(core + 0xE0);
    if (rd32(core + 0x80) == 0x7FDD1CD0u && n >= 3 && n <= 6) {
        u32 open = 0, panel = n;  // open: panels with a disabled generator (bit i: generators i, i+1)
        for (u32 i = 0; i < n; i++) {
            const u32 next = (i + 1) % n;
            if (rd8(core + 0x1F8 + i) || rd8(core + 0x1F8 + next)) open |= 1u << i;
            if (rd32(core + 0xE4 + i * 4) + 0x10 == c.r[6] && rd32(core + 0xE4 + next * 4) + 0x10 == c.r[7])
                panel = i;
        }
        static u32 s_core[4], s_open[4];
        for (u32 k = 0; k < 4; k++) {
            if (s_core[k] && s_core[k] != core) continue;
            if (s_core[k] != core || s_open[k] != open) {
                s_core[k] = core;
                s_open[k] = open;
                logf("game", "Prisons of the Maw: force field hexagon %08X: panels open (generator disabled, no collision): mask %02X",
                     core, open);
            }
            break;
        }
        if (panel < n && (open >> panel & 1)) {
            c.r[3] = 0;  // no hit
            return;
        }
    }
    f_7FD02CC0_orig(c);
}

// Perfect mission stats (settings). The mission's running stats live at 0x7FDF04F0 (StatsReset
// 0x7FC464B0 at a mission's start): +0x00/+0x04 shots fired (lasers / other), +0x0C hits, +0x10
// enemies that appeared, +0x14 enemies the player destroyed, +0x18 friendlies, +0x1C friendlies
// lost (StatsObjectDestroyed 0x7FC46840), +0x20 lives lost, +0x24 mission time (seconds; weapons
// and respawns read it, so it is left alone during play), +0x28/+0x2C targeting computer uses
// and seconds. At the end MissionResults (0x7FD76514) turns them into the record the results
// screen shows and MedalRank (0x7FD765F4) compares with the mission's requirements: +0x00 time
// (minutes, float), +0x04 enemies destroyed, +0x06 accuracy %, +0x08 friendlies lost (u16 each),
// +0x0C lives lost (u32), +0x10 targeting computer efficiency % (float). With the cheat the
// record says 0:01, every enemy that appeared destroyed, 100%, 0, 0 and 100%. (Mission 15's
// +0x08 counts something where more is better: left as it is.)
// DitherStrength (0x7FD345CC): the alpha of the TEV color with which the frame setup (0x7FD34A78)
// blends a screen-space dither texture over the picture (25 at boot, 45 later; 0 = none). Its
// texture matrix (0x7FD2AB48) repeats every 4 pixels of a 640x480 screen, so at 4K a texel
// covers ~4.5 pixels: a zigzag grid over smooth areas such as Hoth's sky. Off by default, like
// Dolphin's "Disable Dithering" code (it zeroes this byte and the pattern's mode word). The
// pattern is not zero-mean: it also brightened the picture (~10/255 in Hoth's sky).
void hle_DitherStrength(Ctx& c) {
    f_7FD345CC_orig(c);
    if (!settings::get().gameDither) c.r[3] = 0;
}

void hle_MissionResults(Ctx& c) {
    const u32 rec = c.r[4];
    f_7FD76514_orig(c);
    if (!settings::get().perfectStats) return;
    constexpr u32 kStats = 0x7FDF04F0;
    const u32 enemies = std::max(rd32(kStats + 0x10), rd32(kStats + 0x14));
    wr32(rec + 0x00, fbits(1.25f / 60.f));  // 1.25 s: shown as 0:01, compared in whole seconds
    wr16(rec + 0x04, std::min<u32>(enemies, 0xFFFF));
    wr16(rec + 0x06, 100);
    if (rd8(0x800962EC) != 15) wr16(rec + 0x08, 0);  // the mission (r13-0x4AB4)
    wr32(rec + 0x0C, 0);
    wr32(rec + 0x10, fbits(100.f));
    logf("cheat", "perfect mission stats: 0:01, %u enemies destroyed, 100%% accuracy, nothing lost, 100%% targeting computer",
         enemies);
}

// R2_RESULTS_TEST=<seconds>: once, that long after the start, MissionResults into a scratch
// record below the guest stack, logged before and after the cheat (missions are long to finish
// in a test). Called from GameFrameDelta, an ordinary guest call.
static void resultsTest(Ctx& c) {
    static const double at = getenv("R2_RESULTS_TEST") ? atof(getenv("R2_RESULTS_TEST")) : -1.0;
    static bool done = false;
    if (at < 0 || done) return;
    static const u64 t0 = tbNow();
    if (double(tbNow() - t0) / double(kTbClock) < at) return;
    done = true;
    for (int pass = 0; pass < 2; pass++) {
        settings::Settings& s = settings::get();
        const bool keep = s.perfectStats;
        s.perfectStats = pass == 1;
        Ctx t = c;
        t.r[1] = c.r[1] - 0x200;
        const u32 rec = t.r[1] + 0x100;
        t.r[4] = rec;
        t.lr = 0;
        hle_MissionResults(t);
        s.perfectStats = keep;
        logf("cheat", "results test (%s): time %.3f min, enemies %u, accuracy %u%%, friendlies lost %u, lives lost %u, targeting computer %.1f%%",
             pass ? "cheat" : "game's", s2d(rd32(rec)), rd16(rec + 4), rd16(rec + 6), rd16(rec + 8), rd32(rec + 12),
             s2d(rd32(rec + 16)));
    }
}

// The targeting computer (cockpit view, Y). TargetingRect (0x7FCD5D08) projects the corners of
// its screen with the camera's projection (+0xA4) to 640x480 screen units, and TargetingPass
// (0x7FCD7D28, each frame pass) draws the orange screen there as a 2D quad and copies the EFB
// under it. In widescreen those
// are positions on the widened screen; Aurora laid the quad out like any 4:3 2D element and
// squeezed it a second time. The drawing is bracketed for the bridge (gfx/gx_bridge.cpp,
// screenSpace2D) by BP writes to an unused register, through the gather pipe so they stay in
// order with the game's commands.
static void gatherBp(u32 v) {
    wr8(0xCC008000, 0x61);
    wr32(0xCC008000, v);
}

void hle_TargetingPass(Ctx& c) {
    static bool logged = false;
    if (!logged) {
        logged = true;
        logf("wide", "targeting computer screen drawn in screen space");
    }
    gatherBp(0x7E5C5201);
    f_7FCD7D28_orig(c);
    gatherBp(0x7E5C5200);
}

// The flight reticle (issue #10: "the crosshair is not a circle when using widescreen").
// ReticleDraw (0x7FC6BFA8) draws its rings and side lines as 2D quads under an identity
// orthographic projection, at the aim point it projected with the camera, which widescreen
// already made wide - so the game's numbers are already right for the wide screen, and Aurora's
// 4:3 layout for 2D squeezed the rings toward the middle a second time (about 0.75 as wide at
// 16:9). Bracketed like the targeting computer, with another value: the bridge sends these as
// screen space, identity projection included.
void hle_ReticleDraw(Ctx& c) {
    static bool logged = false;
    if (!logged) {
        logged = true;
        logf("wide", "flight reticle drawn in screen space");
    }
    gatherBp(0x7E5C5221);
    f_7FC6BFA8_orig(c);
    gatherBp(0x7E5C5220);
}

// Cutscene movies: CutsceneVideoDraw (0x7FD68C54) draws a movie object's quad (the Death Star's
// explosion at the end of mission 1, the capital ship's in the Factor 5 intro), placed in the 3D
// scene to cover the game's own view with a margin. Widescreen showed the scene past its edges
// (7% at each side of the intro's at 2.12:1); the bridge (gfx/gx_bridge.cpp, fillScreen)
// stretches draws inside this bracket out to the screen's edges.
void hle_CutsceneVideoDraw(Ctx& c) {
    gatherBp(0x7E5C5211);
    f_7FD68C54_orig(c);
    gatherBp(0x7E5C5210);
}

// Infinite lives (settings): LoseLife (0x7FD7648C) decrements the lives byte at 0x800962EE
// (3 ships at a mission start; at 1 it ends the game instead: 0x800962EF = 0x21, mode 6) and
// returns 1. With the cheat it reports the life lost without taking it.
// Audio commentary pause / resume (L3 / left stick click, or C on keyboard).
// Freezes the active MusyX commentary voice stream at its exact sample via sndStreamFrq(stid, 0),
// restores normal BGM/SFX volume while paused via f_7FD4B524, and re-ducks on resume.
static std::atomic<u8> s_commentaryUi{0};  // 0 = hidden, 1 = playing, 2 = paused

CommentaryOverlay commentaryOverlay() {
    const u8 st = s_commentaryUi.load(std::memory_order_relaxed);
    return {st != 0, st == 2};
}

static void commentaryUpdate(Ctx& c) {
    struct PausedVoice {
        u32 stid = 0;
        u32 frq = 0;
        u8 pan = 64;
        bool active = false;
    };
    static PausedVoice s_paused[2];
    static bool s_isPaused = false;
    static double s_duckVol1 = 0.35;
    static double s_duckVol2 = 0.35;

    constexpr u32 kVoiceMgr = 0x7FE1C1D8u;
    constexpr u32 kUnlockWord = 0x7FE1C4A0u + 0x658u;
    constexpr u32 kAudioMixer = 0x7FE19510u;

    auto callStreamFrq = [&](u32 stid, u32 frq) {
        const double f1 = c.f[1], ps1 = c.ps1[1];
        GuestFrame gf(c);
        c.r[3] = stid;
        c.r[4] = frq;
        f_80013AC0(c);
        c.f[1] = f1;
        c.ps1[1] = ps1;
    };
    auto callStreamMix = [&](u32 stid, u32 vol, u8 pan) {
        const double f1 = c.f[1], ps1 = c.ps1[1];
        GuestFrame gf(c);
        c.r[3] = stid;
        c.r[4] = vol;
        c.r[5] = pan;
        c.r[6] = 0;
        c.r[7] = 0;
        c.r[8] = 0;
        f_8001386C(c);
        c.f[1] = f1;
        c.ps1[1] = ps1;
    };
    auto callAudioDuck = [&](double v1, double v2, double fadeSec) {
        const double f1 = c.f[1], ps1 = c.ps1[1];
        GuestFrame gf(c);
        c.f[1] = c.ps1[1] = rsingle(v1);
        c.f[2] = c.ps1[2] = rsingle(v2);
        c.f[3] = c.ps1[3] = rsingle(fadeSec);
        f_7FD4B524(c);
        c.f[1] = f1;
        c.ps1[1] = ps1;
    };

    if (s_isPaused) {
        bool anyStillValid = false;
        for (int i = 0; i < 2; i++) {
            if (!s_paused[i].active) continue;
            const u32 slot = kVoiceMgr + u32(i) * 0x70u;
            if (rd8(slot) == 3 && rd32(slot + 0x50u) == s_paused[i].stid) anyStillValid = true;
            else s_paused[i].active = false;
        }
        if (!anyStillValid) {
            s_isPaused = false;
        } else {
            // Keep BGM/SFX at normal volume if the game re-ducks (e.g. after unpausing the Start menu)
            const double cur1 = s2d(rd32(kAudioMixer + 0xD58u));
            const double cur2 = s2d(rd32(kAudioMixer + 0xD7Cu));
            if (cur1 < 0.99 || cur2 < 0.99) callAudioDuck(1.0, 1.0, 0.2);
        }
    }

    const bool commentaryEnabled = (rd32(kUnlockWord) & 0x100u) != 0;
    const bool commentaryFlag = rd8(c.r[13] + 0xFFFFB4E2u) != 0;
    bool streamPlaying = false;
    for (int i = 0; i < 2; i++) {
        if (rd8(kVoiceMgr + u32(i) * 0x70u) == 3) streamPlaying = true;
    }

    if (input::commentaryTogglePressed()) {
        if (s_isPaused) {
            for (int i = 0; i < 2; i++) {
                if (!s_paused[i].active) continue;
                callStreamFrq(s_paused[i].stid, s_paused[i].frq);
                callStreamMix(s_paused[i].stid, 127, s_paused[i].pan);
                s_paused[i].active = false;
            }
            s_isPaused = false;
            callAudioDuck(s_duckVol1, s_duckVol2, 0.25);
        } else if (commentaryEnabled && streamPlaying) {
            const u32 vdyMgr = rd32(c.r[13] + 0xFFFFB4F0u);
            const u32 vdyTbl = vdyMgr ? rd32(vdyMgr + 4u) : 0;
            int count = 0;
            for (int i = 0; i < 2; i++) {
                const u32 slot = kVoiceMgr + u32(i) * 0x70u;
                if (rd8(slot) != 3) continue;
                const u32 stid = rd32(slot + 0x50u);
                const u8 pan = rd8(slot + 0x0Bu);
                const u32 entry = rd16(slot + 0x08u);
                const u32 frq = vdyTbl ? rd16(vdyTbl + entry * 56u + 2u) : 22050u;
                s_paused[i] = {stid, frq ? frq : 22050u, pan, true};
                callStreamFrq(stid, 0);
                callStreamMix(stid, 0, pan);
                count++;
            }
            if (count > 0) {
                const double cur1 = s2d(rd32(kAudioMixer + 0xD58u));
                const double cur2 = s2d(rd32(kAudioMixer + 0xD7Cu));
                s_duckVol1 = (cur1 > 0.01 && cur1 < 0.99) ? cur1 : 0.35;
                s_duckVol2 = (cur2 > 0.01 && cur2 < 0.99) ? cur2 : 0.35;
                callAudioDuck(1.0, 1.0, 0.25);
                s_isPaused = true;
            }
        }
    }

    if (s_isPaused) s_commentaryUi.store(2, std::memory_order_relaxed);
    else if (commentaryEnabled && commentaryFlag && streamPlaying) s_commentaryUi.store(1, std::memory_order_relaxed);
    else s_commentaryUi.store(0, std::memory_order_relaxed);
}

// --- Art Gallery caption overlay --------------------------------------------------------------
namespace {
constexpr int kArtGalleryCount = 27;
const char* const kArtGalleryTitles[kArtGalleryCount] = {
    "Massassi Temple Steps",            // 00: frontendGal/01
    "Bespin Tibanna Gas Refineries",    // 01: frontendGal/02
    "Hoth DF.9 Laser Turret",           // 02: frontendGal/03
    "Hoth Rebel Turret",                // 03: frontendGal/04
    "Bespin Tibanna Gas Platforms",     // 04: frontendGal/05
    "Bespin Gas Balloons & Cloud Car",  // 05: frontendGal/06
    "Hoth Planetary Ion Cannon",        // 06: frontendGal/07
    "Hoth Ion Cannon",                  // 07: frontendGal/08
    "Hoth Main Power Generators",       // 08: frontendGal/09
    "AT-AT Walker",                     // 09: frontendGal/ATAT
    "AT-ST Walker",                     // 10: frontendGal/AtSt
    "A-Wing Starfighter",               // 11: frontendGal/AWing
    "B-Wing Starfighter",               // 12: frontendGal/BWing
    "Millennium Falcon",                // 13: frontendGal/Falcon
    "Lambda-Class Imperial Shuttle",    // 14: frontendGal/ImpShuttle
    "Maw Installation Fuel Tanks",      // 15: frontendGal/mawFuelTanks
    "Maw Installation Landing Platform",// 16: frontendGal/mawPlatform
    "Proximity Space Mine",             // 17: frontendGal/mine
    "Orbital Shield Generator",         // 18: frontendGal/shieldGenerator
    "Space Defense Gun Turret",         // 19: frontendGal/spaceGunTurret
    "Rebel Snowspeeder",                // 20: frontendGal/Speeder
    "TIE Fighter",                      // 21: frontendGal/TieFighter
    "TIE Interceptor",                  // 22: frontendGal/TieInterceptor
    "Bespin Twin-Pod Cloud Car",        // 23: frontendGal/CloudCar
    "X-Wing Starfighter",               // 24: frontendGal/XWing
    "X-Wing Cockpit",                   // 25: frontendGal/XWingCockpit
    "Y-Wing Starfighter",               // 26: frontendGal/YWing
};
std::atomic<int> s_artGalleryIdx{-1};  // -1 = inactive, 0..26 = active image index
}  // namespace

const char* artGalleryTitleForIndex(int idx, int& outTotal) {
    outTotal = kArtGalleryCount;
    if (idx < 0 || idx >= kArtGalleryCount) return "";
    return kArtGalleryTitles[idx];
}

ArtGalleryOverlay artGalleryOverlay() {
    const int idx = s_artGalleryIdx.load(std::memory_order_relaxed);
    if (idx < 0 || idx >= kArtGalleryCount) return {};
    return {true, idx, kArtGalleryCount, kArtGalleryTitles[idx]};
}

static void artGalleryUpdate(Ctx& c) {
    int idx = -1;
    if (rd32(0x7FDE8254) == 6) {
        const u32 menuObj = rd32(c.r[13] + 0xFFFFB548u);
        if (menuObj >= 0x7F000000u && menuObj < 0x81800000u) {
            // Art Gallery is screen 0x14 (stored at r13 + 0xFFFFB54F) with mode 1 at menuObj + 0x04;
            // gallery struct sits at menuObj + 0x6794
            const u8 curScreen = rd8(c.r[13] + 0xFFFFB54Fu);
            const u8 galState = rd8(menuObj + 0x6794u + 0x3E7u);
            if ((curScreen == 0x14u || rd32(menuObj + 0x04u) == 1u) && galState == 1u) {
                const u8 pic = rd8(menuObj + 0x6794u + 0x3E8u);
                if (pic < u8(kArtGalleryCount)) {
                    idx = int(pic);
                }
            }
        }
    }
    s_artGalleryIdx.store(idx, std::memory_order_relaxed);
}

void hle_LoseLife(Ctx& c) {
    if (settings::get().infiniteLives && rd8(0x800962EF) != 0x21) {
        c.r[3] = 1;
        return;
    }
    f_7FD7648C_orig(c);
}

// --- Passcodes and unlocks --------------------------------------------------------------------
// The game's progress block (0x7FE1C4A0, bss in its VM; saved to the memory card: +0 = 1 asks for
// a save, +2 the current pilot, pilot records of 288 bytes after it) keeps two flag words that
// the Options > Passcodes screen sets (0x7FD6D4A0: the 8 characters, zero-padded to 1 KiB, go
// through SHA-1 1000 times - each digest written back into the buffer after the code - and the
// result is looked up in a table of 71 digests at 0x7FDE2350; verified with THATSME!, ?INSIDER,
// BLAHBLAH, EXHIBIT!, COMPOSER, LIONHEAD, U!?!VWZC + GIVEITUP, !??QWTTJ + CLASSIC, ...):
//   +0x658 unlocks (SetUnlockFlag 0x7FD588C0: progress r3, mask r4, on r5): 0x1 credits, 0x2 the
//     documentary ("making of"), 0x8 audio commentary, 0x10 art gallery, 0x20 music hall,
//     0x40 ace mode (Options > Special Features shows each, 0x7FD70188), 0x100 / 0x200
//     commentary / ace mode switched on there; 0x400..0x40000 and 0x200000 the craft codes
//     (read only by CraftMask, below). Indices 1-7 of the table set bits 0x1..0x40.
//   +0x654 cheats (SetCheatFlag 0x7FD58914): 0x1 every main mission open (0x7FD7F9C0 ORs
//     0x7FF into the pilot's mission masks), 0x10000..0x100000 the five bonus missions (each
//     once the main mission before it is open), 0x2 every tech upgrade at its best (UpgradeLevel
//     0x7FD57FF0 returns 3), 0x8 infinite lives (LoseLife), 0x8000 black and white (LIONHEAD,
//     a toggle; the renderer's frame setup 0x7FD34978), 0x10..0x4000 single upgrade levels,
//     0x200000 a developer test mode (test textures: left out).
// Both setters mark the block for saving unless the mask only holds bits the game does not
// keep (cheats 0x8 / 0x8000, unlock 0x200000). The menu's rows set and clear the same bits
// (applied from the per-frame update, an ordinary guest call: the block's VM page can fault in
// there), so they end up in the game's save as if the codes had been entered.
namespace {
constexpr u32 kProgress = 0x7FE1C4A0, kCheatFlags = kProgress + 0x654, kUnlockFlags = kProgress + 0x658;
constexpr u32 kCraftCodeBits = 0x0007FC00u | 0x00200000u;  // the craft passcodes' unlock bits
struct Unlock {
    const char* name;
    const char* help;
    int word;  // 0 cheats (+0x654), 1 unlocks (+0x658)
    u32 mask;
    bool saved;
};
const Unlock kUnlocks[unlocks::Count] = {
    {"All missions", "Every mission is open, the five bonus missions too (the game's \"all missions\" passcodes).", 0,
     0x001F0001u, true},
    {"All tech upgrades", "Every craft has every tech upgrade at its best: advanced lasers, homing torpedoes, shields, "
     "targeting computer and the rest.", 0, 0x2u, true},
    {"Ace mode", "Options > Special Features gets Ace Mode: the game at its hardest.", 1, 0x40u, true},
    {"Art gallery", "Options > Special Features gets the Art Gallery.", 1, 0x10u, true},
    {"Music hall", "Options > Special Features gets the Music Hall.", 1, 0x20u, true},
    {"Documentary", "Options > Special Features gets the documentary (the making of the game).", 1, 0x2u, true},
    {"Audio commentary", "Options > Special Features gets the developers' audio commentary (switch it on there).", 1,
     0x8u, true},
    {"Credits", "Options > Special Features gets the end credits.", 1, 0x1u, true},
    {"Black and white", "The whole game in black and white, like an old film (the LIONHEAD passcode). The game does not "
     "save it.", 0, 0x8000u, false},
};
std::atomic<u32> s_flagsSeen[2];       // the words as last read by the game's frame update
std::atomic<bool> s_flagsKnown{false};
std::atomic<u32> s_setBits[2], s_clearBits[2];  // asked for by the menu, not applied yet
}  // namespace

namespace unlocks {
const char* name(int i) { return i >= 0 && i < Count ? kUnlocks[i].name : ""; }
const char* help(int i) { return i >= 0 && i < Count ? kUnlocks[i].help : ""; }
int state(int i) {
    if (i < 0 || i >= Count || !s_flagsKnown.load(std::memory_order_relaxed)) return -1;
    const Unlock& u = kUnlocks[i];
    return (s_flagsSeen[u.word].load(std::memory_order_relaxed) & u.mask) == u.mask;
}
void set(int i, bool on) {
    if (i < 0 || i >= Count) return;
    const Unlock& u = kUnlocks[i];
    (on ? s_setBits : s_clearBits)[u.word].fetch_or(u.mask);
    (on ? s_clearBits : s_setBits)[u.word].fetch_and(~u.mask);
    // shown at once (the game's frame update confirms it)
    s_flagsSeen[u.word].store(on ? s_flagsSeen[u.word].load() | u.mask : s_flagsSeen[u.word].load() & ~u.mask);
}
}  // namespace unlocks

// From GameFrameDelta (every mode loop's frame update): the menu's changes go in, the words come out.
static void unlocksFrame() {
    // R2_UNLOCKS=<seconds>:all or <seconds>:<item numbers> (unlocks::Item, comma-separated):
    // switched on that long after the start (tests of the menu's rows; the game loads its save
    // from the card at boot, over anything set before)
    static const char* env = getenv("R2_UNLOCKS");
    static const u64 envStart = tbNow();
    if (env && double(tbNow() - envStart) / double(kTbClock) >= atof(env)) {
        const char* list = strchr(env, ':') ? strchr(env, ':') + 1 : env;
        const std::string l = std::string(",") + list + ",";
        for (int i = 0; i < unlocks::Count; i++)
            if (!strcmp(list, "all") || l.find("," + std::to_string(i) + ",") != std::string::npos) unlocks::set(i, true);
        env = nullptr;
    }
    const u32 addr[2] = {kCheatFlags, kUnlockFlags};
    for (int w = 0; w < 2; w++) {
        const u32 on = s_setBits[w].exchange(0), off = s_clearBits[w].exchange(0);
        if (on | off) {
            const u32 v = rd32(addr[w]), nv = (v | on) & ~off;
            if (nv != v) {
                wr32(addr[w], nv);
                bool saved = false;
                for (const Unlock& u : kUnlocks)
                    if (u.word == w && ((v ^ nv) & u.mask) && u.saved) saved = true;
                if (saved) wr8(kProgress, 1);  // as the game's setters do: the front end saves it
                for (const Unlock& u : kUnlocks)
                    if (u.word == w && ((v ^ nv) & u.mask))
                        logf("unlock", "%s %s%s", u.name, (nv & u.mask) == u.mask ? "on" : "off",
                             u.saved ? " (kept in the game's save)" : "");
            }
        }
        s_flagsSeen[w].store(rd32(addr[w]), std::memory_order_relaxed);
    }
    s_flagsKnown.store(true, std::memory_order_relaxed);
}

// All craft (settings): CraftMask (0x7FCB9544; mission r3, game mode r4) builds the hangar's
// craft for a mission from the craft passcodes' unlock bits (+0x658: 0x400 .. 0x40000, 0x200000;
// copied to a mask at r13-19980) and the pilot's medals (AllMissionsMedal 0x7FCB9458: every
// main (r3 = 0) or every (r3 = 1) mission at medal level r4 or better; MissionMedal 0x7FCB94E4:
// mission r3 at level r4 - both only called by the craft code, 0x7FCB9458..0x7FCB9FB4). With the
// cheat both answer yes and the craft bits are set for the call only: every craft the game lets
// a mission have, nothing written to the save. Its per-mission rules (the snowspeeder only on
// Hoth's surface, ...) stay the game's.
void hle_CraftMask(Ctx& c) {
    const u32 mission = c.r[3];
    const bool all = settings::get().allCraft;
    const u32 v = all ? rd32(kUnlockFlags) : 0, add = all ? kCraftCodeBits & ~v : 0;
    if (add) wr32(kUnlockFlags, v | add);
    f_7FCB9544_orig(c);
    if (add) wr32(kUnlockFlags, rd32(kUnlockFlags) & ~add);
    static u32 lastMission = ~0u, lastMask = ~0u;
    if (mission != lastMission || c.r[3] != lastMask) {
        lastMission = mission;
        lastMask = c.r[3];
        logf("craft", "mission %u: craft %04X%s", mission, c.r[3], all ? " (all craft)" : "");
    }
}
void hle_AllMissionsMedal(Ctx& c) {
    if (settings::get().allCraft) {
        c.r[3] = 1;
        return;
    }
    f_7FCB9458_orig(c);
}
void hle_MissionMedal(Ctx& c) {
    if (settings::get().allCraft) {
        c.r[3] = 1;
        return;
    }
    f_7FCB94E4_orig(c);
}

// Invincibility (settings): CraftDestroy (0x7FC3E528, a method every craft class shares) starts
// a craft's explosion - whatever destroyed it, shots or a crash. The player's craft
// (0x7FDEFE14) is spared.
void hle_CraftDestroy(Ctx& c) {
    if (settings::get().invincible && c.r[3] == rd32(0x7FDEFE14)) return;
    f_7FC3E528_orig(c);
}

// Infinite bombs: WeaponInit (0x7FC396D4) sets up each weapon of a craft - count (+0) and
// maximum (+4) from the craft's data, the weapon slot at +0x520. Weapon objects sit at a
// different place in each craft class (inside it or behind a pointer), so they are recorded
// here as the game initializes them. The player's secondary weapons are given back what they
// fire right away (WeaponUpdate, below) and topped up every frame (missions can also restore a
// count from a checkpoint); unlimited weapons have a maximum of 0 and are left alone.
//
// Death Star Attack fails ("You have no proton torpedoes left to fire at the exhaust port")
// from its mission script once the torpedo count runs out; the count has to stay up in every
// state the craft flies in, the trench run included, not only in free flight (+0x370 = 0).
struct WeaponRec {
    u32 craft, weapon, slot, max;
};
static std::vector<WeaponRec> s_weapons;

void hle_WeaponInit(Ctx& c) {
    const u32 weapon = c.r[3], craft = c.r[4], slot = c.r[5];
    f_7FC396D4_orig(c);
    const u32 max = rd32(weapon + 4);
    for (WeaponRec& w : s_weapons)
        if (w.weapon == weapon) {  // re-initialized (respawn), or memory reused by another craft
            w = {craft, weapon, slot, max};
            return;
        }
    if (s_weapons.size() >= 1024) s_weapons.erase(s_weapons.begin(), s_weapons.begin() + 512);
    s_weapons.push_back({craft, weapon, slot, max});
}

// fill one of the player craft's recorded weapons (a mission may have set a new maximum)
static void refill(WeaponRec& w) {
    if (w.max == 0 || rd32(w.weapon + 0x520) != w.slot) return;  // unlimited, or no longer this weapon
    const u32 max = rd32(w.weapon + 4);
    if (max == 0 || max > 999) return;
    w.max = max;
    const s32 count = s32(rd32(w.weapon));
    if (count >= 0 && count < s32(max)) wr32(w.weapon, max);  // (-1: unlimited already)
}

static void keepAmmo() {
    if (!settings::get().infiniteAmmo) return;
    const u32 craft = playerCraft();
    if (!craft) return;
    for (WeaponRec& w : s_weapons)
        if (w.craft == craft) refill(w);
}

// WeaponUpdate (0x7FC39F24, weapon r3, craft r4): a weapon's update, which fires it on the
// trigger and takes one from its count. The player's shot is given back before anything (HUD,
// mission script) reads the count.
void hle_WeaponUpdate(Ctx& c) {
    const u32 weapon = c.r[3], craft = c.r[4];
    f_7FC39F24_orig(c);
    if (!settings::get().infiniteAmmo || !craft || craft != playerCraft()) return;
    for (WeaponRec& w : s_weapons)
        if (w.weapon == weapon && w.craft == craft) {
            refill(w);
            break;
        }
}

// ...and its health (craft +0x6F4, a float: 180 for the X-wing) is kept full, so the HUD ring
// stays green. Only for an object whose vtable (+0x80) has CraftDestroy: 0x7FDEFE14 points
// at other things outside missions.
// The full value is what the craft has when it starts flying (state +0x370 = 0: the game fills it
// at a spawn and a respawn); while the craft is being set up the field holds other things, and
// keeping one of those (more than full) made the HUD's health ring vanish.
static u32 s_healthCraft = 0;  // keepHealth's craft and its full health
static float s_healthFull = 0.f;
static bool s_healthFlying = false;
static void keepHealth() {
    u32& craft = s_healthCraft;
    float& full = s_healthFull;
    bool& flyingBefore = s_healthFlying;
    const u32 p = settings::get().invincible ? playerCraft() : 0;
    if (p != craft) {
        craft = p;
        full = 0.f;
        flyingBefore = false;
    }
    if (!craft) return;
    const bool flying = rd32(craft + 0x370) == 0;
    const float h = float(s2d(rd32(craft + 0x6F4)));
    if (!flying) {
        flyingBefore = false;
        return;
    }
    if (!flyingBefore || full <= 0.f) {
        flyingBefore = true;
        full = h > 0.f && h <= 100000.f ? h : 0.f;
        return;
    }
    if (h < full) wr32(craft + 0x6F4, d2s(full));
}

// Debugging: R2_CRAFT_DIFF=<len hex>[,<deref offset hex>...]: every frame, compare the player
// craft object's first <len> bytes (or those of objects it points to at the given offsets)
// with the previous frame and log bytes/halfwords/words that drop by exactly 1 from a small
// value (ammo counters); fields that keep changing are muted after 8 reports.
static void craftDiffProbe() {
    static const char* spec = getenv("R2_CRAFT_DIFF");
    if (!spec) return;
    static u32 len = 0;
    static std::vector<s32> derefs;
    if (!len) {
        char* end;
        len = u32(strtoul(spec, &end, 16)) & ~3u;
        while (*end == ',') derefs.push_back(s32(strtol(end + 1, &end, 16)));
        if (derefs.empty()) derefs.push_back(-1);
    }
    const u32 craft = playerCraft();
    static std::vector<std::vector<u8>> prev;
    static std::vector<u32> prevBase;
    static std::unordered_map<u64, int> reports;
    prev.resize(derefs.size());
    prevBase.resize(derefs.size());
    for (size_t d = 0; d < derefs.size(); d++) {
        u32 base = craft;
        if (craft && derefs[d] >= 0) {
            base = rd32(craft + u32(derefs[d]));
            if ((base >> 24) != 0x80 && (base >> 24) != 0x81 && (base >> 24) != 0x7F) base = 0;
        }
        std::vector<u8> cur;
        if (base) {
            cur.resize(len);
            for (u32 o = 0; o < len; o += 4) {
                const u32 w = rd32(base + o);
                cur[o] = u8(w >> 24), cur[o + 1] = u8(w >> 16), cur[o + 2] = u8(w >> 8), cur[o + 3] = u8(w);
            }
        }
        if (base && base == prevBase[d] && prev[d].size() == len) {
            for (u32 o = 0; o < len; o++) {
                for (u32 size : {1u, 2u, 4u}) {
                    if (o % size) continue;
                    u32 a = 0, b = 0;
                    for (u32 i = 0; i < size; i++) a = a << 8 | prev[d][o + i], b = b << 8 | cur[o + i];
                    if (a >= 1 && a <= 255 && b == a - 1) {
                        const u64 key = (u64(d) << 40) | (u64(o) << 8) | size;
                        if (reports[key]++ < 8)
                            logf("diff", "craft %08X (%s+%X) +%03X size %u: %u -> %u", base, derefs[d] < 0 ? "obj" : "ptr",
                                 derefs[d] < 0 ? 0 : derefs[d], o, size, a, b);
                    }
                }
            }
        }
        prev[d] = std::move(cur);
        prevBase[d] = base;
    }
}

}  // namespace rt

// --- save states ----------------------------------------------------------------------------
#include "savestate.h"
namespace rt {
void hleSaveState(state::Writer& w) {
    w.section("HLE ");
    w.vec(s_weapons);
    for (const auto& t : s_portTables) w.pod(t.block), w.pod(t.size);  // (their data: memory.cpp)
    w.pod(s_tileListRecords), w.pod(s_heightRowsExtra);
    // the heap extension's blocks (their data: memory.cpp, with MEM1)
    std::vector<ExtCell> cells;
    for (const auto* m : {&s_extFree, &s_extUsed}) {
        cells.clear();
        for (const auto& [cell, size] : *m) cells.push_back({cell, size});
        w.vec(cells);
    }
    w.pod(s_extInUse), w.pod(s_extPeak);
}

void hleLoadState(state::Reader& r) {
    r.section("HLE ");
    r.vec(s_weapons);
    for (auto& t : s_portTables) r.pod(t.block), r.pod(t.size);
    r.pod(s_tileListRecords), r.pod(s_heightRowsExtra);
    std::vector<ExtCell> cells;
    for (auto* m : {&s_extFree, &s_extUsed}) {
        r.vec(cells);
        m->clear();
        for (const auto& e : cells) (*m)[e.cell] = e.size;
    }
    r.pod(s_extInUse), r.pod(s_extPeak);
}

// After a load (on the guest's fiber): caches of guest objects start over, a skip in progress
// ends, and the camera aspect constants hold what the saved session wrote (a stale aspect for
// the camera objects of the state if the window has another shape now).
void hleAfterLoad() {
    s_craftLast = 0;
    s_craftLastIsCraft = false;
    s_healthCraft = 0;
    s_healthFull = 0.f;
    s_healthFlying = false;
    if (s_skip) {
        s_skip = false;
        viSetRateHz(frameRateHz());
        presentPaced(true);
        setDvdBoost(1.0);
        setAudioSpeed(1.0, false);
    }
    const u32 bits = rd32(0x80099548);  // the world camera's (see setGameAspect)
    s_aspectWritten = bits;
    float f;
    memcpy(&f, &bits, 4);
    const float display = s_displayAspect.load(std::memory_order_relaxed);
    if (bits != k43 && f > 0.5f && f < 8.f && std::abs(f - display) >= 0.0005f && !oldGameAspect(f)) {
        const u32 i = s_oldAspectCount.fetch_add(1, std::memory_order_relaxed);
        s_oldAspects[i % 8].store(f, std::memory_order_relaxed);
    }
}

namespace {
struct MouseMenuState {
    std::mutex mu;
    float mx = 0.f, my = 0.f;
    float winW = 0.f, winH = 0.f;
    bool moved = false;
    bool leftClicked = false;
    bool rightClicked = false;
    int wheelDelta = 0;  // +1 = up, -1 = down
};
MouseMenuState s_mouseMenu;
std::atomic<u16> s_mouseInjectMask{0};
}  // namespace

void moviePauseMouse(int sel, bool click) {
    if (sel >= 0 && sel <= 1) s_movieMouseHover.store(sel, std::memory_order_relaxed);
    if (click) s_movieMouseClick.store(true, std::memory_order_relaxed);
}

void mouseMenuEvent(const void* sdlEvent) {
    if (!sdlEvent || gfx::menuOpen()) return;
    const SDL_Event& e = *static_cast<const SDL_Event*>(sdlEvent);
    std::lock_guard lk(s_mouseMenu.mu);
    auto updateWinSize = [&](SDL_WindowID id) {
        if (SDL_Window* w = SDL_GetWindowFromID(id)) {
            int ww = 0, wh = 0;
            SDL_GetWindowSize(w, &ww, &wh);
            if (ww > 0 && wh > 0) {
                s_mouseMenu.winW = float(ww);
                s_mouseMenu.winH = float(wh);
            }
        }
    };
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        updateWinSize(e.motion.windowID);
        s_mouseMenu.mx = e.motion.x;
        s_mouseMenu.my = e.motion.y;
        s_mouseMenu.moved = true;
    } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        updateWinSize(e.button.windowID);
        s_mouseMenu.mx = e.button.x;
        s_mouseMenu.my = e.button.y;
        if (e.button.button == SDL_BUTTON_LEFT) s_mouseMenu.leftClicked = true;
        else if (e.button.button == SDL_BUTTON_RIGHT) s_mouseMenu.rightClicked = true;
    } else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
        updateWinSize(e.wheel.windowID);
        if (e.wheel.y > 0.f) s_mouseMenu.wheelDelta = 1;
        else if (e.wheel.y < 0.f) s_mouseMenu.wheelDelta = -1;
    }
}

u16 mouseMenuPadButtons() {
    return s_mouseInjectMask.load(std::memory_order_relaxed);
}

static void mouseMenuUpdate(Ctx& c) {
    static u16 heldMask = 0;
    static int heldFrames = 0;

    if (heldFrames > 0) {
        heldFrames--;
        if (heldFrames == 0) heldMask = 0;
    }

    const bool inFrontEnd = rd32(0x7FDE8254) == 6;
    const bool inPause = rd32(0x7FDE8668) == 2;
    const bool active = !gfx::menuOpen() && !s_moviePaused && (inFrontEnd || inPause);

    float mx = 0.f, my = 0.f, winW = 0.f, winH = 0.f;
    bool moved = false, leftClick = false, rightClick = false;
    int wheel = 0;
    {
        std::lock_guard lk(s_mouseMenu.mu);
        mx = s_mouseMenu.mx;
        my = s_mouseMenu.my;
        winW = s_mouseMenu.winW;
        winH = s_mouseMenu.winH;
        moved = s_mouseMenu.moved;
        leftClick = s_mouseMenu.leftClicked;
        rightClick = s_mouseMenu.rightClicked;
        wheel = s_mouseMenu.wheelDelta;
        s_mouseMenu.moved = false;
        s_mouseMenu.leftClicked = false;
        s_mouseMenu.rightClicked = false;
        s_mouseMenu.wheelDelta = 0;
    }

    if (!active) {
        heldMask = 0;
        heldFrames = 0;
        s_mouseInjectMask.store(0, std::memory_order_relaxed);
        return;
    }

    auto pulse = [&](u16 btn) {
        heldMask = btn;
        heldFrames = 3;
    };

    if (inFrontEnd) {
        const u32 menuObj = rd32(c.r[13] + 0xFFFFB548u);
        const u8 screen = rd8(c.r[13] + 0xFFFFB54Fu);
        const bool inArtGallery = (screen == 0x14u && menuObj >= 0x7FC00000u && rd8(menuObj + 0x6794u + 0x3E7u) == 1u);

        if (inArtGallery) {
            if (leftClick || wheel < 0) pulse(0x0002u);       // PAD_BUTTON_RIGHT (next image)
            else if (wheel > 0) pulse(0x0001u);               // PAD_BUTTON_LEFT (prev image)
            else if (rightClick) pulse(0x0200u);              // PAD_BUTTON_B (back)
        } else if (screen != 0x08u) {
            // Standard front-end menus (Main Menu, Options, Game/Video/Sound Settings, Special Features, Rogue Leaders, Music Hall, etc.)
            const bool inMusicHall = (screen == 0x14u && menuObj >= 0x7FC00000u && rd32(menuObj + 0x04u) == 0u);
            if (menuObj >= 0x7FC00000u && rd8(menuObj) == 0x26u &&
                winW > 0.f && winH > 0.f && (moved || leftClick)) {
                const int minSel = inMusicHall ? 0 : int(s8(rd8(menuObj + 0x25u)));
                const int itemCount = inMusicHall ? s_musicHallItemCount : int(s8(rd8(menuObj + 0x24u)));
                const bool horiz = !inMusicHall && rd8(menuObj + 0x02u) == 1u;
                const u32 slotBase = rd32(menuObj + 0x3B80u);
                if (minSel >= 0 && itemCount > minSel && itemCount <= 16 && slotBase >= 0x7FC00000u) {
                    const u32 xfbLines = viXfbLines();
                    const float logicalH = (xfbLines >= 200u && xfbLines <= 528u) ? float(xfbLines) : 448.f;
                    const float aspect43W = winH * (4.f / 3.f);
                    const float boxW = std::min(winW, aspect43W);
                    const float boxH = (winW < aspect43W) ? (winW * (3.f / 4.f)) : winH;
                    const float offsetX = (winW - boxW) * 0.5f;
                    const float offsetY = (winH - boxH) * 0.5f;
                    const float gx = (mx - offsetX) * (640.f / boxW);
                    // Account for the front-end EFB display-copy vertical offset (18 logical lines)
                    const float gy = (my - offsetY) * (logicalH / boxH) + 18.f;

                    int best = -1;
                    float bestDist = 1e9f;
                    constexpr u32 kScratch = 0x8F0000A0u;
                    for (int i = minSel; i < itemCount; i++) {
                        const u32 slot = slotBase + u32(i) * 108u;
                        // Note: do not filter on rd8(slot + 1u) == 0, because toggle items like
                        // AUDIO COMMENTARY: ON and ACE MODE: ON hide their primary OFF slot and
                        // display a secondary ON slot at the exact same coordinates.
                        if (rd32(slot + 4u) == 0 || s32(rd32(slot + 0x60u)) <= 0) continue;

                        {
                            const Ctx saved = c;
                            GuestFrame gf(c);
                            c.r[3] = slot;
                            c.r[4] = kScratch + 0u;
                            c.r[5] = kScratch + 4u;
                            c.r[6] = kScratch + 8u;
                            c.r[7] = kScratch + 12u;
                            f_7FD3EB1C(c);
                            c = saved;
                        }
                        const float minX = float(s2d(rd32(kScratch + 0u)));
                        const float maxX = float(s2d(rd32(kScratch + 4u)));
                        const float minY = float(s2d(rd32(kScratch + 8u)));
                        const float maxY = float(s2d(rd32(kScratch + 12u)));

                        float sxMin = std::min(minX, maxX), sxMax = std::max(minX, maxX);
                        float syMin = std::min(minY, maxY), syMax = std::max(minY, maxY);
                        const u32 mat = rd32(slot + 0x68u);
                        if (mat >= 0x7FC00000u) {
                            const float m00 = float(s2d(rd32(mat + 0x00u))), m01 = float(s2d(rd32(mat + 0x04u)));
                            const float m02 = float(s2d(rd32(mat + 0x08u))), m03 = float(s2d(rd32(mat + 0x0Cu)));
                            const float m10 = float(s2d(rd32(mat + 0x10u))), m11 = float(s2d(rd32(mat + 0x14u)));
                            const float m12 = float(s2d(rd32(mat + 0x18u))), m13 = float(s2d(rd32(mat + 0x1Cu)));
                            constexpr float lz = -15.625f;  // -500 / 32.0f in f_7FD3F54C
                            const float x0 = m00 * minX + m01 * minY + m02 * lz + m03;
                            const float x1 = m00 * maxX + m01 * maxY + m02 * lz + m03;
                            const float y0 = m10 * minX + m11 * minY + m12 * lz + m13;
                            const float y1 = m10 * maxX + m11 * maxY + m12 * lz + m13;
                            sxMin = std::min(x0, x1);
                            sxMax = std::max(x0, x1);
                            syMin = std::min(y0, y1);
                            syMax = std::max(y0, y1);
                        }

                        if (!horiz) {
                            if (gx < std::min(sxMin - 24.f, 110.f) || gx > std::max(sxMax + 24.f, 530.f)) continue;
                            const float syCenter = 0.5f * (syMin + syMax);
                            const float syHalf = std::max(0.5f * (syMax - syMin) + 3.5f, 10.f);
                            const float dist = std::fabs(gy - syCenter);
                            if (dist <= syHalf && dist < bestDist) {
                                bestDist = dist;
                                best = i;
                            }
                        } else {
                            if (gy < syMin - 8.f || gy > syMax + 8.f) continue;
                            const float sxCenter = 0.5f * (sxMin + sxMax);
                            const float sxHalf = std::max(0.5f * (sxMax - sxMin) + 12.f, 24.f);
                            const float dist = std::fabs(gx - sxCenter);
                            if (dist <= sxHalf && dist < bestDist) {
                                bestDist = dist;
                                best = i;
                            }
                        }
                    }
                    if (best >= 0 && u8(best) != rd8(menuObj + 0x23u)) {
                        const Ctx saved = c;
                        wr8(menuObj + 0x23u, u8(best));
                        {
                            GuestFrame gf(c);
                            c.r[3] = 0x1E5u;
                            c.r[4] = 0x40u;
                            f_7FD5123C(c);
                            if (inMusicHall) {
                                musicHallApplyHighlight(c, menuObj);
                            } else {
                                c.r[3] = menuObj;
                                f_7FD6AF54(c);
                            }
                        }
                        c = saved;
                    }
                }
            }
            // Show the Rebel emblem ONLY next to the currently selected item in front-end menus
            if (menuObj >= 0x7FC00000u && menuObj < 0x81800000u && !inMusicHall && !inArtGallery && screen != 0x13u) {
                const u32 embBase = rd32(menuObj + 0x6520u);
                const int sel = int(s8(rd8(menuObj + 0x23u)));
                const int count = int(rd8(menuObj + 0x24u));
                if (embBase >= 0x7FC00000u && embBase < 0x81800000u && count > 0 && count <= 9) {
                    for (int i = 0; i < 9; i++) {
                        const u32 emb = embBase + u32(i) * 44u;
                        wr8(emb, (i == sel && i < count) ? 1u : 0u);
                        if (i == 0) wr32(emb + 0x24u, 0u);
                    }
                }
            }
            if (leftClick) pulse(0x0100u);       // PAD_BUTTON_A
            else if (rightClick) pulse(0x0200u); // PAD_BUTTON_B
            else if (wheel > 0) pulse(0x0008u);  // PAD_BUTTON_UP
            else if (wheel < 0) pulse(0x0004u);  // PAD_BUTTON_DOWN
        }
    } else if (inPause) {
        if (leftClick) pulse(0x0100u);           // PAD_BUTTON_A
        else if (rightClick) pulse(0x0200u);     // PAD_BUTTON_B
        else if (wheel > 0) pulse(0x0008u);      // PAD_BUTTON_UP
        else if (wheel < 0) pulse(0x0004u);      // PAD_BUTTON_DOWN
    }

    s_mouseInjectMask.store(heldMask, std::memory_order_relaxed);
}
}  // namespace rt
