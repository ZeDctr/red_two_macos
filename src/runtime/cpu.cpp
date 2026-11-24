// CPU helpers called by recompiled code: SPRs, cache ops, paired-single quantization,
// estimates, indirect dispatch.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <limits>
#include <vector>

#include "runtime.h"
#include "table.h"
#include "hw/gx_fifo.h"
#include <string>

namespace rt {

bool g_trace = false;
volatile u32 g_irqPending = 0;

// ---------------------------------------------------------------------------------------
// Function lookup: dense tables over each cluster of code addresses.
// ---------------------------------------------------------------------------------------
namespace {
struct Region {
    u32 base = 0, end = 0;
    std::vector<GuestFn> fns;
};
std::vector<Region> s_regions;
std::vector<std::pair<u32, GuestFn>> s_orig;
}  // namespace

void tableInit() {
    std::vector<FuncEntry> all(g_funcTable, g_funcTable + g_funcCount);
    std::sort(all.begin(), all.end(), [](auto& a, auto& b) { return a.addr < b.addr; });
    for (size_t i = 0; i < all.size();) {
        size_t j = i + 1;
        while (j < all.size() && all[j].addr - all[j - 1].addr < 0x100000) j++;
        Region r;
        r.base = all[i].addr;
        r.end = all[j - 1].addr + 4;
        r.fns.assign((r.end - r.base) / 4, nullptr);
        for (size_t k = i; k < j; k++) r.fns[(all[k].addr - r.base) / 4] = all[k].fn;
        s_regions.push_back(std::move(r));
        i = j;
    }
    for (const FuncEntry* e = g_origTable; e->fn; e++) s_orig.push_back({e->addr, e->fn});
}

GuestFn lookupFn(u32 addr) {
    for (auto& r : s_regions)
        if (addr >= r.base && addr < r.end && !(addr & 3)) return r.fns[(addr - r.base) / 4];
    return nullptr;
}

GuestFn lookupOrig(u32 addr) {
    for (auto& [a, f] : s_orig)
        if (a == addr) return f;
    return nullptr;
}

void dispatch(Ctx& c, u32 addr) {
    if (GuestFn f = lookupFn(addr)) [[likely]] {
        f(c);
        return;
    }
    // No recompiled function: code built at runtime (the game's heap modules) or an entry the
    // analysis missed. Interpret it; calls out of it reach recompiled code again.
    if (!isMem1(addr) && !isVm(addr))
        fatal("indirect call to %08X, outside memory (from %s, lr=%08X)", addr, describeAddr(c.curFn).c_str(), c.lr);
    interpret(c, addr);
}

static u32 s_ring[64];
static volatile u32 s_ringPos;
static std::vector<u32> s_logFns;  // R2_LOGFN=addr,addr,...
static std::vector<u32> s_logFnLr;  // the caller filter of each (0: any)
static u32 s_watchLo = 0, s_watchHi = 0;
static u32 s_watchValLo = 0, s_watchValHi = 0;  // R2_WATCH_VALUE: 32-bit stores of such values
static double s_watchValFrom = 0;
static u32 s_watchValMinAddr = 0;
bool g_watch = false;

void debugInit() {
    if (const char* v = getenv("R2_LOGFN")) {  // addr[@lr],...: @lr logs only calls returning there
        for (const char* p = v; *p;) {
            s_logFns.push_back(u32(strtoul(p, const_cast<char**>(&p), 16)));
            u32 lr = 0;
            if (*p == '@') lr = u32(strtoul(p + 1, const_cast<char**>(&p), 16));
            s_logFnLr.push_back(lr);
            while (*p == ',' || *p == ' ') p++;
        }
        if (!s_logFns.empty()) g_trace = true;
    }
    if (const char* v = getenv("R2_WATCH")) {
        char* end = nullptr;
        s_watchLo = u32(strtoul(v, &end, 16));
        u32 len = 4;
        if (end && *end == ',') len = u32(strtoul(end + 1, nullptr, 16));
        s_watchHi = s_watchLo + len;
        g_watch = true;
    }
    // R2_WATCH_VALUE=<lo>,<hi>[,<from seconds>[,<lowest address>]] (hex but the seconds): every
    // 32-bit guest store of a value in [lo, hi), e.g. a pointer past the end of memory (who
    // wrote it), and DMA writes carrying such a word; the first 20 with the guest call chain
    if (const char* v = getenv("R2_WATCH_VALUE")) {
        char* end = nullptr;
        s_watchValLo = u32(strtoul(v, &end, 16));
        s_watchValHi = end && *end == ',' ? u32(strtoul(end + 1, &end, 16)) : s_watchValLo + 1;
        if (end && *end == ',') s_watchValFrom = strtod(end + 1, &end);
        if (end && *end == ',') s_watchValMinAddr = u32(strtoul(end + 1, nullptr, 16));
        g_watch = true;
    }
}

// R2_WATCH_PTR (main.cpp): the watched range follows a pointer the game keeps
void watchSet(u32 lo, u32 len) {
    s_watchLo = lo;
    s_watchHi = lo + len;
    g_watch = len != 0;
}

void watchStore(u32 a, u32 v, int size) {
    if (size == 4 && v >= s_watchValLo && v < s_watchValHi && a < 0xCC000000 && a >= s_watchValMinAddr) {
        static int n = 0;
        static const u64 t0 = tbNow();
        if (n < 200 && double(tbNow() - t0) / double(kTbClock) >= s_watchValFrom) {
            const Ctx& c = curCtx();
            logf("watch", "value store %08X = %08X in %s (lr %08X)%s%s", a, v, describeAddr(c.curFn).c_str(), c.lr,
                 n < 20 ? " " : "", n < 20 ? guestBacktrace().c_str() : "");
            n++;
        }
    }
    if (a + u32(size) <= s_watchLo || a >= s_watchHi) return;
    const Ctx& c = curCtx();
    logf("watch", "store%d %08X = %08X in %s (lr %08X)", size * 8, a, v, describeAddr(c.curFn).c_str(), c.lr);
    // R2_WATCH_BT=<n>: the guest call chain with the first n stores (who got there)
    static int bt = getenv("R2_WATCH_BT") ? atoi(getenv("R2_WATCH_BT")) : 0;
    if (bt > 0) {
        bt--;
        logf("watch", "  r1 %08X %s", c.r[1], guestBacktrace().c_str());
    }
}

// Host-side writes into guest memory (DMA, HLE): same R2_WATCH range, physical addresses.
void watchHost(u32 phys, u32 len, const char* who) {
    if (!g_watch) return;
    if (s_watchValHi > s_watchValLo) {  // R2_WATCH_VALUE: words of the DMA'd data
        const u8* d = physPtr(phys & 0x1FFFFFFC, len);
        static int n = 0;
        for (u32 i = 0; d && i + 4 <= len && n < 50; i += 4) {
            const u32 w = u32(d[i] << 24 | d[i + 1] << 16 | d[i + 2] << 8 | d[i + 3]);
            if (w >= s_watchValLo && w < s_watchValHi && (0x80000000u | ((phys & 0x1FFFFFFC) + i)) >= s_watchValMinAddr) {
                logf("watch", "%s wrote %08X at %08X (in %s)", who, w, (phys & 0x1FFFFFFC) + i,
                     describeAddr(curCtx().curFn).c_str());
                n++;
            }
        }
    }
    if (s_watchHi <= s_watchLo) return;
    const u32 lo = s_watchLo & 0x1FFFFFFF, hi = lo + (s_watchHi - s_watchLo);
    phys &= 0x1FFFFFFF;
    if (phys + len <= lo || phys >= hi) return;
    const u8* p = physPtr(lo, 4);
    logf("watch", "%s writes %08X+%X; watched word now %08X (in %s)", who, phys, len,
         p ? u32(p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]) : 0, describeAddr(curCtx().curFn).c_str());
}

void traceEnter(Ctx& c, u32 addr) {
    c.curFn = addr;
    s_ring[s_ringPos & 63] = addr;
    s_ringPos = s_ringPos + 1;
    if (!s_logFns.empty()) [[unlikely]] {
        for (size_t i = 0; i < s_logFns.size(); i++)
            if (s_logFns[i] == addr && (!s_logFnLr[i] || s_logFnLr[i] == c.lr)) {
                logf("call", "%s(r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X f1=%g f2=%g f3=%g) lr=%08X r1=%08X r2=%08X r13=%08X",
                     describeAddr(addr).c_str(), c.r[3], c.r[4], c.r[5], c.r[6], c.r[7], c.f[1], c.f[2], c.f[3], c.lr, c.r[1],
                     c.r[2], c.r[13]);
                break;
            }
    }
}
void printTraceRing(FILE* f, int n) {
    const u32 pos = s_ringPos;
    for (int i = n; i >= 1; i--) {
        const u32 a = s_ring[(pos - u32(i)) & 63];
        if (!a) continue;
        const char* nm = symName(a);
        fprintf(f, "  %08X %s\n", a, nm ? nm : "");
    }
}
std::string traceRing(int n) {
    std::string out;
    const u32 pos = s_ringPos;
    for (int i = n; i >= 1; i--) {
        const u32 a = s_ring[(pos - u32(i)) & 63];
        if (!a) continue;
        out += describeAddr(a);
        out += i > 1 ? " > " : "";
    }
    return out;
}

// ---------------------------------------------------------------------------------------
// Special purpose registers
// ---------------------------------------------------------------------------------------
namespace {
u32 s_spr[1024];
u64 s_tbOffset = 0;  // guest TB = host TB + offset (TBL/TBU writes)

void lockedCacheDma() {
    const u32 u = s_spr[922], l = s_spr[923];
    u32 lines = ((u & 0x1F) << 2) | ((l >> 2) & 3);
    if (!lines) lines = 128;
    const u32 mem = u & ~0x1Fu;
    const u32 lc = l & ~0x1Fu;
    const bool load = (l >> 4) & 1;
    const u32 bytes = lines * 32;
    // MEM_ADDR is physical; LC_ADDR is the locked cache's effective address (0xE00xxxxx).
    u8* lcp = g_base + kLockedCache + (lc & (kLockedCacheSize - 1));
    if ((lc & (kLockedCacheSize - 1)) + bytes > kLockedCacheSize) fatal("locked cache DMA past the cache: lc=%08X len=%u", lc, bytes);
    u8* mp = physPtr(mem, bytes);
    if (!mp) fatal("locked cache DMA to unmapped memory %08X (len %u)", mem, bytes);
    if (load) memcpy(lcp, mp, bytes);
    else memcpy(mp, lcp, bytes);
    s_spr[923] &= ~3u;  // T and F clear: done
}
}  // namespace

u32 mfspr(Ctx& c, u32 spr) {
    switch (spr) {
    case 18: return c.dsisr;
    case 19: return c.dar;
    case 22: return decRead();
    case 26: return c.srr0;
    case 27: return c.srr1;
    case 272: case 273: case 274: case 275: return c.sprg[spr - 272];
    case 268: return u32(tbNow() + s_tbOffset);
    case 269: return u32((tbNow() + s_tbOffset) >> 32);
    case 287: return 0x00083214;  // PVR: Gekko
    case 920: return s_spr[920] & ~0x0F000000u;  // HID2: DMA queue length 0
    case 921: return s_spr[921] & ~1u;           // WPAR: buffer empty
    default: return s_spr[spr & 1023];
    }
}

void mtspr(Ctx& c, u32 spr, u32 v) {
    switch (spr) {
    case 18: c.dsisr = v; return;
    case 19: c.dar = v; return;
    case 22: decWrite(v); return;
    case 26: c.srr0 = v; return;
    case 27: c.srr1 = v; return;
    case 272: case 273: case 274: case 275: c.sprg[spr - 272] = v; return;
    case 284: {  // TBL / TBU writes: the guest's time base takes the new half from now on
        const u64 host = tbNow();
        s_tbOffset = (((host + s_tbOffset) & ~0xFFFFFFFFull) | v) - host;
        return;
    }
    case 285: {
        const u64 host = tbNow();
        s_tbOffset = ((u64(v) << 32) | ((host + s_tbOffset) & 0xFFFFFFFFull)) - host;
        return;
    }
    case 25:
        s_spr[25] = v;
        vmSetSdr1(v);
        return;
    case 921:
        s_spr[921] = v;
        gxResetGatherPipe();
        return;
    case 923:
        s_spr[923] = v;
        if (v & 2) lockedCacheDma();
        return;
    default: s_spr[spr & 1023] = v; return;
    }
}

// The console's clock (issue #49: Tatooine and Imperial Academy Heist always at night). On the
// GameCube the IPL sets the time base to the RTC's time (local time, as set in the console's
// menu) before the game starts, and games read the time of day from it (OSGetTime). Ours
// counted from 0 at every start: 2000-01-01 00:00, midnight. Like Dolphin: the time base starts
// at the PC's local time, and the RTC reads local time too. R2_CLOCK=<hh>[:<mm>] (tests): today
// at that time.
namespace {
s64 s_localBias = 0;  // local wall clock minus UTC, seconds (+ R2_CLOCK's shift)
constexpr s64 kEpoch2000 = 946684800;  // 2000-01-01 00:00 UTC in Unix seconds
}  // namespace

void clockInit() {
    const time_t now = time(nullptr);
    tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    if (const char* c = getenv("R2_CLOCK"); c && *c) {
        int h = 0, m = 0;
        if (sscanf(c, "%d:%d", &h, &m) >= 1) {
            local.tm_hour = h;
            local.tm_min = m;
            local.tm_sec = 0;
        }
    }
#ifdef _WIN32
    const time_t wall = _mkgmtime(&local);  // the local wall clock's fields read as UTC
#else
    const time_t wall = timegm(&local);
#endif
    s_localBias = s64(wall) - s64(now);
    const u64 secs = u64(s64(now) + s_localBias - kEpoch2000);
    s_tbOffset = secs * kTbClock - tbNow();
    logf("clock", "the console's clock: %04d-%02d-%02d %02d:%02d (local time%s)", local.tm_year + 1900, local.tm_mon + 1,
         local.tm_mday, local.tm_hour, local.tm_min, getenv("R2_CLOCK") ? ", R2_CLOCK" : "");
}

u32 rtcSeconds() { return u32(s64(time(nullptr)) + s_localBias - kEpoch2000); }

u32 mftb(Ctx&, u32 which) {
    const u64 t = tbNow() + s_tbOffset;
    return which == 269 ? u32(t >> 32) : u32(t);
}

extern volatile u32 g_irqLine;
void mtmsr(Ctx& c, u32 v, u32 pc) {
    c.msr = v;
    if ((v & 0x8000) && (g_irqPending || g_irqLine)) safepoint(c, pc);
}

void dcbz(Ctx&, u32 a) {
    a &= ~31u;
    if (isIo(a)) return;
    memset(hpw(a), 0, 32);
}
void dcbzl(Ctx&, u32 a) {
    a &= ~31u;
    memset(g_base + kLockedCache + (a & (kLockedCacheSize - 1)), 0, 32);
}
void dcbi(Ctx&, u32) {}
void icbi(Ctx&, u32) {}

// System call (PPCSync): nothing to do. Draining a partly filled gather buffer here was tried
// for the post-menu stall and did not help; the SDK pads with NOPs (GXFlush) before syncing.
void sc(Ctx&, u32) {}

void trap(Ctx& c, u32 pc) { logf("cpu", "trap at %08X (lr=%08X)", pc, c.lr); }

void illegal(Ctx& c, u32 pc, u32 word) {
    fatal("illegal instruction %08X at %08X (fn %s)", word, pc, describeAddr(c.curFn).c_str());
}

void rfi(Ctx& c, u32 pc) {
    fatal("rfi at %08X outside the HLE'd context switch (srr0=%08X)", pc, c.srr0);
}

void lswi(Ctx& c, u32 rd, u32 ea, u32 n) {
    u32 r = rd - 1, sh = 0;
    for (u32 i = 0; i < n; i++) {
        if (sh == 0) {
            r = (r + 1) & 31;
            c.r[r] = 0;
            sh = 32;
        }
        sh -= 8;
        c.r[r] |= rd8(ea + i) << sh;
    }
}
void stswi(Ctx& c, u32 rs, u32 ea, u32 n) {
    u32 r = rs - 1, sh = 0;
    for (u32 i = 0; i < n; i++) {
        if (sh == 0) {
            r = (r + 1) & 31;
            sh = 32;
        }
        sh -= 8;
        wr8(ea + i, (c.r[r] >> sh) & 0xFF);
    }
}

// ---------------------------------------------------------------------------------------
// Floating point
// ---------------------------------------------------------------------------------------
double fres(double v) { return double(float(1.0 / v)); }
double frsqrte(double v) { return 1.0 / std::sqrt(v); }

u32 fctiw(Ctx& c, double v, bool truncate) {
    if (v != v) return 0x80000000u;
    double r;
    if (truncate) r = std::trunc(v);
    else {
        switch (c.fpscr & 3) {
        case 0: r = std::nearbyint(v); break;  // host default: round to nearest even
        case 1: r = std::trunc(v); break;
        case 2: r = std::ceil(v); break;
        default: r = std::floor(v); break;
        }
    }
    if (r >= 2147483648.0) return 0x7FFFFFFFu;
    if (r < -2147483648.0) return 0x80000000u;
    return u32(s32(r));
}

static inline s32 sext6(u32 v) { return (v & 0x20) ? s32(v | ~0x3Fu) : s32(v); }

// GQR scale fields (6-bit signed exponents) -> 2^-e for loads, 2^e for stores. A table: ldexp
// was a hot spot (psq_l runs for most float loads in the game's vector code).
struct QuantScales {
    float load[64], store[64];
    QuantScales() {
        for (u32 i = 0; i < 64; i++) {
            load[i] = std::ldexp(1.0f, -sext6(i));
            store[i] = std::ldexp(1.0f, sext6(i));
        }
    }
};
static const QuantScales s_quant;

void psqLoadQ(Ctx& c, u32 fd, u32 a, bool w, u32 gqr) {
    const u32 type = (gqr >> 16) & 7;
    double p0, p1 = 1.0;
    const float scale = s_quant.load[(gqr >> 24) & 0x3F];
    switch (type) {
    case 4:
        p0 = float(rd8(a)) * scale;
        if (!w) p1 = float(rd8(a + 1)) * scale;
        break;
    case 5:
        p0 = float(rd16(a)) * scale;
        if (!w) p1 = float(rd16(a + 2)) * scale;
        break;
    case 6:
        p0 = float(s8(rd8(a))) * scale;
        if (!w) p1 = float(s8(rd8(a + 1))) * scale;
        break;
    case 7:
        p0 = float(s16(rd16(a))) * scale;
        if (!w) p1 = float(s16(rd16(a + 2))) * scale;
        break;
    default:
        p0 = s2d(rd32(a));
        if (!w) p1 = s2d(rd32(a + 4));
        break;
    }
    c.f[fd] = p0;
    c.ps1[fd] = p1;
}

template <typename T>
static T quantize(double v, float scale) {
    float f = float(v) * scale;
    const float lo = float(std::numeric_limits<T>::min()), hi = float(std::numeric_limits<T>::max());
    if (!(f >= lo)) f = lo;  // also NaN
    if (f > hi) f = hi;
    return T(f);
}

void psqStoreQ(Ctx& c, u32 fs, u32 a, bool w, u32 gqr) {
    const u32 type = gqr & 7;
    const float scale = s_quant.store[(gqr >> 8) & 0x3F];
    const double p0 = c.f[fs], p1 = c.ps1[fs];
    switch (type) {
    case 4:
        wr8(a, quantize<u8>(p0, scale));
        if (!w) wr8(a + 1, quantize<u8>(p1, scale));
        break;
    case 5:
        wr16(a, quantize<u16>(p0, scale));
        if (!w) wr16(a + 2, quantize<u16>(p1, scale));
        break;
    case 6:
        wr8(a, u8(quantize<s8>(p0, scale)));
        if (!w) wr8(a + 1, u8(quantize<s8>(p1, scale)));
        break;
    case 7:
        wr16(a, u16(quantize<s16>(p0, scale)));
        if (!w) wr16(a + 2, u16(quantize<s16>(p1, scale)));
        break;
    default:
        wr32(a, d2s(p0));
        if (!w) wr32(a + 4, d2s(p1));
        break;
    }
}

}  // namespace rt

// --- save states ----------------------------------------------------------------------------
#include "savestate.h"
namespace rt {
void vmSaveState(state::Writer& w);
void vmLoadState(state::Reader& r);

void cpuSaveState(state::Writer& w) {
    w.section("CPU ");
    w.pod(s_spr);
    w.pod(s_tbOffset);
    vmSaveState(w);
}

void cpuLoadState(state::Reader& r) {
    r.section("CPU ");
    r.pod(s_spr);
    r.pod(s_tbOffset);
    // the guest's time base goes on from where it was saved (host time moved on by tbDelta)
    s_tbOffset = u64(s64(s_tbOffset) - r.tbDelta);
    vmLoadState(r);
}
}  // namespace rt
