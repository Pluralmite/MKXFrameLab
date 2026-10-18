#include "buffer.h"
#include "memory.h"
#include "log.h"
#include "MinHook.h"
#include "bot.h"
#include <windows.h>
#include <xinput.h>
#include <atomic>
#include <mutex>
#include <deque>
#include <cstdlib>
#include <cstring>

namespace ibuf {

bool g_enabled = false;
int g_window = 6;
bool g_probe = true;

// attack-ish buttons: A B X Y LB RB
static const WORD kMask = XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_Y |
                          XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER;

using XGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
static XGetStateFn o_XGetState = nullptr;
static std::string g_status = "not initialised";

// shared state (game thread writes tick info, poll thread reads)
static std::atomic<uint64_t> g_tick{0};
static std::atomic<bool> g_busy{false};
static std::atomic<int> g_slot{-1};

static std::mutex g_mtx;
struct Press { uint64_t tick; WORD buttons; bool gameBufferedAtPress; };
static std::deque<Press> g_presses;      // presses during the current busy period (+ a few after)
static WORD g_prevButtons = 0;
// injection
static WORD g_injectMask = 0;
static int g_injectPhase = 0;            // 0 idle, 1 = report released, 2.. = report pressed
static int g_injectPolls = 0;
static uint64_t g_injectArmTick = 0;
// probe results
static std::deque<ProbeRow> g_rows;
struct PendingEval { ProbeRow row; uint64_t freeTick; };
static std::vector<PendingEval> g_eval;

static std::atomic<uint32_t> g_lastPress{0};   // low 16: buttons, high 16: d-pad bits
static std::atomic<uint32_t> g_pressSeq{0};
static uint32_t g_pressRing[32];
void LastPress(uint16_t& buttons, uint16_t& dirs) { uint32_t v = g_lastPress.load(); buttons = v & 0xFFFF; dirs = v >> 16; }
uint32_t PressSeq() { return g_pressSeq.load(); }
bool GetPress(uint32_t n, uint16_t& buttons, uint16_t& dirs) {
    uint32_t cur = g_pressSeq.load();
    if (n == 0 || n > cur || cur - n >= 32) return false;
    uint32_t v = g_pressRing[n % 32];
    buttons = v & 0xFFFF; dirs = v >> 16;
    return true;
}
static void RecordPress(uint32_t v) {
    g_lastPress = v;
    uint32_t n = g_pressSeq.load() + 1;
    g_pressRing[n % 32] = v;
    g_pressSeq = n;
}

static std::atomic<bool> g_capture{false}, g_drain{false};
static std::atomic<bool> g_allowed{false};
void SetAllowed(bool on) { g_allowed = on; }
static std::atomic<uint32_t> g_realBtn{0};
static std::atomic<int32_t> g_realStick{0};
void SetCapture(bool on) {
    if (on == g_capture.load()) return;
    g_capture = on;
    if (!on) g_drain = true;
}
void RealPad(uint16_t& buttons, int16_t& lx, int16_t& ly) {
    buttons = (uint16_t)g_realBtn.load();
    int32_t v = g_realStick.load();
    lx = (int16_t)(v & 0xFFFF); ly = (int16_t)((uint32_t)v >> 16);
}
static void Neutral(XINPUT_STATE* st) {
    if (!g_capture && !g_drain) return;
    const XINPUT_GAMEPAD& g = st->Gamepad;
    if (!g_capture && g.wButtons == 0 && std::abs((int)g.sThumbLX) < 9000 && std::abs((int)g.sThumbLY) < 9000 && g.bLeftTrigger < 40 &&
        g.bRightTrigger < 40)
        g_drain = false;
    memset(&st->Gamepad, 0, sizeof(st->Gamepad));
}
static DWORD Captured(DWORD idx, XINPUT_STATE* st, DWORD r);
static DWORD WINAPI h_XGetState(DWORD idx, XINPUT_STATE* st) {
    DWORD r = o_XGetState(idx, st);
    {
        static bool seenIdx[16] = {};
        if (idx < 16 && !seenIdx[idx]) { seenIdx[idx] = true; Log("ibuf: game polls XInput index %u (result %lu)", idx, r); }
    }
    if (idx == 0 && st && r == ERROR_SUCCESS && !bot::Running()) {
        g_realBtn = st->Gamepad.wButtons;
        g_realStick = (int32_t)(((uint32_t)(uint16_t)st->Gamepad.sThumbLY << 16) | (uint16_t)st->Gamepad.sThumbLX);
        if (g_capture || g_drain) { Neutral(st); return r; }
    }
    return Captured(idx, st, r);
}
static DWORD Captured(DWORD idx, XINPUT_STATE* st, DWORD r) {
    uint16_t botBtn;
    // the script drives whichever XInput slot the game bound to player 1 (it moves when a pad sleeps and wakes)
    if (idx < 8 && st && g_allowed && bot::Override(botBtn)) {
        // scripted test input replaces the pad completely (also works with no pad connected)
        static DWORD packet = 1;
        memset(&st->Gamepad, 0, sizeof(st->Gamepad));
        st->Gamepad.wButtons = botBtn & ~0x0C00;
        st->Gamepad.bLeftTrigger = (botBtn & 0x0400) ? 255 : 0;
        st->Gamepad.bRightTrigger = (botBtn & 0x0800) ? 255 : 0;
        static uint16_t last = 0;
        if (botBtn != last) { packet++; last = botBtn; }
        st->dwPacketNumber = packet;
        if (idx == 0) {
            static WORD prevB = 0;
            WORD b = st->Gamepad.wButtons;
            WORD pr = (WORD)(b & ~prevB & kMask);
            if (pr) RecordPress((uint32_t)pr | ((uint32_t)(b & 0x000F) << 16));
            prevB = b;
        }
        return ERROR_SUCCESS;
    }
    static bool loggedPad[4] = {}, loggedNone = false, loggedPress = false;
    if (idx < 4 && r == ERROR_SUCCESS && !loggedPad[idx]) { loggedPad[idx] = true; Log("ibuf: XInput pad %u connected", idx); }
    if (idx == 0 && r != ERROR_SUCCESS && !loggedNone) { loggedNone = true; Log("ibuf: XInput pad 0 not connected (keyboard/DirectInput device?)"); }
    if (r != ERROR_SUCCESS || !st || idx != 0) return r;
    {
        static WORD prevB = 0;
        WORD b = st->Gamepad.wButtons;
        WORD pr = (WORD)(b & ~prevB & kMask);
        if (pr) RecordPress((uint32_t)pr | ((uint32_t)(b & 0x000F) << 16));
        prevB = b;
    }
    if (!loggedPress && (st->Gamepad.wButtons & kMask)) { loggedPress = true; Log("ibuf: first XInput button press seen (%04X)", st->Gamepad.wButtons); }
    std::lock_guard<std::mutex> l(g_mtx);
    WORD btn = st->Gamepad.wButtons;
    WORD pressed = (WORD)(btn & ~g_prevButtons & kMask);
    g_prevButtons = btn;
    uint64_t tick = g_tick.load();
    if (pressed) {
        g_presses.push_back({tick, pressed, false});
        while (g_presses.size() > 32) g_presses.pop_front();
    }
    // extended buffer: rewrite the report around the first free frame
    if (!g_allowed) { g_injectPhase = 0; g_injectMask = 0; }
    if (g_injectPhase == 1) {
        st->Gamepad.wButtons &= (WORD)~g_injectMask;          // force a release edge
        g_injectPhase = 2;
        g_injectPolls = 0;
    } else if (g_injectPhase >= 2) {
        st->Gamepad.wButtons |= g_injectMask;                  // then press
        if (++g_injectPolls >= 3) { g_injectPhase = 0; g_injectMask = 0; }
    }
    return r;
}

// scripted input also has to look like a connected pad to the game's "reconnect the controller" check
using XGetCapsFn = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);
static XGetCapsFn o_XGetCaps = nullptr;
static DWORD WINAPI h_XGetCaps(DWORD idx, DWORD flags, XINPUT_CAPABILITIES* caps) {
    DWORD r = o_XGetCaps(idx, flags, caps);
    if (r != ERROR_SUCCESS && idx < 8 && caps && g_allowed && bot::Running()) {
        memset(caps, 0, sizeof(*caps));
        caps->Type = XINPUT_DEVTYPE_GAMEPAD;
        caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
        caps->Gamepad.wButtons = 0xF3FF;
        caps->Gamepad.bLeftTrigger = caps->Gamepad.bRightTrigger = 255;
        return ERROR_SUCCESS;
    }
    return r;
}
void Init() {
    MH_CreateHookApi(L"xinput9_1_0", "XInputGetCapabilities", (LPVOID)h_XGetCaps, (LPVOID*)&o_XGetCaps);
    MH_STATUS s = MH_CreateHookApi(L"xinput9_1_0", "XInputGetState", (LPVOID)h_XGetState, (LPVOID*)&o_XGetState);
    if (s == MH_OK) s = MH_EnableHook(MH_ALL_HOOKS);
    g_status = s == MH_OK ? "XInput hooked (pad 1)" : "XInput hook failed (" + std::to_string((int)s) + ")";
    Log("ibuf: %s", g_status.c_str());
}

std::string Status() { return g_status; }
int GameBufferSlot() { return g_slot.load(); }

std::vector<ProbeRow> Rows() {
    std::lock_guard<std::mutex> l(g_mtx);
    return std::vector<ProbeRow>(g_rows.rbegin(), g_rows.rend());
}

void OnTick(uint64_t tick, bool p1Valid, uint32_t flags, uintptr_t p1Obj, bool /*unused*/) {
    g_tick = tick;
    if (!p1Valid) return;
    static bool wasBusy = false;
    static uint64_t busySince = 0;
    bool busy = flags != 0;
    // game's own buffered-move slot: [obj+0xA20]+0x330, 0x80000000 = empty
    int slot = -1;
    uintptr_t ctrl = mem::ReadOr<uintptr_t>(p1Obj + 0xA20, 0);
    if (ctrl) {
        uint32_t v = mem::ReadOr<uint32_t>(ctrl + 0x330, 0x80000000u);
        slot = v == 0x80000000u ? -1 : (int)(v & 0x7FFFFFFF);
    }
    g_slot = slot;
    g_busy = busy;

    std::lock_guard<std::mutex> l(g_mtx);
    if (slot >= 0)
        for (auto& p : g_presses) if (p.tick > busySince) p.gameBufferedAtPress = true;

    // resolve probe rows waiting to see whether the move came out
    for (size_t i = 0; i < g_eval.size();) {
        auto& e = g_eval[i];
        if (busy && tick > e.freeTick) e.row.cameOut = true;
        if (tick >= e.freeTick + 2 || e.row.cameOut) {
            g_rows.push_back(e.row);
            if (g_logVerbose) Log("ibuf probe: pressed %d frames early  came_out=%s  game_buffered=%s  our_buffer=%s  buttons=%04X",
                e.row.framesEarly, e.row.cameOut ? "yes" : "no", e.row.gameBuffered ? "yes" : "no",
                e.row.injected ? "re-pressed" : "-", e.row.buttons);
            while (g_rows.size() > 40) g_rows.pop_front();
            g_eval.erase(g_eval.begin() + i);
        } else ++i;
    }

    if (busy && !wasBusy) { busySince = tick; }
    if (!busy && wasBusy) {
        // first free tick: evaluate presses made during the busy period
        WORD lateMask = 0;
        uint64_t newest = 0;
        bool gameHas = false;
        for (auto& p : g_presses) {
            if (p.tick <= busySince) continue;   // the press that started this action
            int early = (int)(tick - p.tick);
            if (g_probe && early <= 20) {   // ignore string/combo presses made long before recovery
                ProbeRow r{p.tick, early, false, p.gameBufferedAtPress, false, p.buttons};
                g_eval.push_back({r, tick});
            }
            if (early <= g_window) { lateMask |= p.buttons; newest = p.tick; }
            gameHas |= p.gameBufferedAtPress;
        }
        // extended buffer: only when the game did not buffer it itself
        if (g_enabled && lateMask && !gameHas && slot < 0) {
            g_injectMask = lateMask;
            g_injectPhase = 1;
            g_injectArmTick = tick;
            Log("ibuf: re-pressing %04X on free tick %llu", lateMask, (unsigned long long)tick);
            for (auto& e : g_eval) if (e.row.pressTick == newest) e.row.injected = true;
        }
        g_presses.clear();
    }
    wasBusy = busy;
}

} // namespace ibuf
