#include "ui.h"
#include "movelist.h"
#include "config.h"
#include "game.h"
#include "boxes.h"
#include "memory.h"
#include "log.h"
#include "imgui.h"
#include "mkx.h"
#include "trace.h"
#include "buffer.h"
#include "screenshot.h"
#define MKXFL_VERSION "1.0"
#include <windows.h>
#include <vector>
#include <string>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <cfloat>

UiState g_ui;
volatile bool g_reloadRequested = false;

static Camera g_cam;

// ------------------------------------------------------------------ tracer preset
static int g_traceN = 0;
void UiStartTrace() {
    if (trace::Active()) return;
    mkx::Snapshot sn = mkx::GetSnapshot();
    uintptr_t p1 = sn.p[0].pci, p2 = sn.p[1].pci;
    if (!p1 || !p2) { Log("trace: no PlayerCollisionInfo yet"); return; }
    uintptr_t a[4] = {p2 + 0x130, p2 + 0x124, p2 + 0x250, p1 + 0x344};
    const char* n[4] = {"P2 hurt[6] radius (Spine2)", "P2 hurt[6] X", "P2 hurt[15] radius (Head)", "P1 hurt[23] X (RightHand)"};
    trace::Start(a, n, 5, std::to_string(++g_traceN));
}

// ------------------------------------------------------------------ hotkeys
static bool KeyPressed(int vk) {
    static bool prev[256] = {};
    if (vk <= 0 || vk > 255) return false;
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool fired = down && !prev[vk];
    prev[vk] = down;
    return fired;
}

void UiHotkeys() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    bool focused = pid == GetCurrentProcessId();
    // always poll so edge state stays correct
    bool menu = KeyPressed(g_cfg.menuKey), boxes = KeyPressed(g_cfg.boxesKey), meter = KeyPressed(g_cfg.meterKey),
         pause = KeyPressed(g_cfg.pauseKey), step = KeyPressed(g_cfg.stepKey), shotKey = KeyPressed(VK_F8), rec = KeyPressed(VK_F9), trc = KeyPressed(VK_F10), mtr = KeyPressed(VK_F11);
    if (!focused) return;
    if (menu) g_ui.gameMenu = !g_ui.gameMenu;
    if (shotKey) shot::Request();
    if (boxes) g_ui.showBoxes = !g_ui.showBoxes;
    if (meter) g_ui.showMeter = !g_ui.showMeter;
    if (mkx::Ready()) {
        if (pause) mkx::SetFrozen(!mkx::Frozen());
        if (step) mkx::StepOnce();
        if (g_logVerbose) {   // research tools (they write large files): only with VerboseLog=1
            if (rec && !mkx::Recording()) mkx::StartRecording(g_ui.recTicks, g_ui.recLabel);
            if (trc) UiStartTrace();
            if (mtr) mkx::ArmMeleeTrace();
        }
    } else {
        if (pause) GameTogglePause();
        if (step) GameStep();
    }
}

// ------------------------------------------------------------------ colors
static ImU32 StateColor(FState s) {
    switch (s) {
    case FState::Idle: return IM_COL32(58, 56, 52, 255);
    case FState::Startup: return IM_COL32(104, 196, 118, 255);
    case FState::Active: return IM_COL32(226, 62, 52, 255);
    case FState::Recovery: return IM_COL32(84, 142, 222, 255);
    case FState::Hitstun: return IM_COL32(236, 204, 92, 255);   // hitstun / other busy
    case FState::Blockstun: return IM_COL32(226, 138, 58, 255);
    default: return IM_COL32(30, 29, 27, 200);
    }
}

// ------------------------------------------------------------------ frame data HUD (game-style)
ImFont* g_hudFont = nullptr;   // serif caps font loaded in dllmain (falls back to default)
ImFont* g_uiFont = nullptr;
ImFont* g_menuFont = nullptr;
ImFont* g_valueFont = nullptr;

// Text with extra letter spacing, like the game's caps labels
static void SpacedText(ImDrawList* dl, ImFont* f, float size, ImVec2 pos, ImU32 col, const char* t, float spacing) {
    for (const char* c = t; *c; ++c) {
        char ch[2] = {*c, 0};
        dl->AddText(f, size, pos, col, ch);
        pos.x += f->CalcTextSizeA(size, FLT_MAX, 0.f, ch).x + spacing;
    }
}
static float SpacedWidth(ImFont* f, float size, const char* t, float spacing) {
    float w = 0;
    for (const char* c = t; *c; ++c) { char ch[2] = {*c, 0}; w += f->CalcTextSizeA(size, FLT_MAX, 0.f, ch).x + spacing; }
    return w > 0 ? w - spacing : 0;
}

// ---- Mortal Kombat X menu drawing (colours sampled from the game's pause menu and move list)
namespace xc {
static const ImU32 White = IM_COL32(246, 244, 238, 255), Text = IM_COL32(226, 224, 218, 255), Dim = IM_COL32(150, 148, 142, 255);
static const ImU32 Tan = IM_COL32(204, 182, 140, 255), Bronze = IM_COL32(150, 130, 96, 255), Gold = IM_COL32(190, 160, 96, 255);
static const ImU32 Fill = IM_COL32(20, 19, 17, 240), FillDeep = IM_COL32(12, 12, 11, 244), Live = IM_COL32(244, 224, 158, 255);
static const ImU32 Plus = IM_COL32(132, 216, 128, 255), Minus = IM_COL32(236, 108, 96, 255);
}
// text drawn twice a hair apart: Belleza is a light face, the game's menu font is a little heavier
static void XText(ImDrawList* dl, ImFont* f, float size, ImVec2 p, ImU32 col, const char* t, float wrap = 0.f) {
    dl->AddText(f, size, p, col, t, nullptr, wrap);
    dl->AddText(f, size, ImVec2(p.x + size * 0.022f, p.y), col, t, nullptr, wrap);
}
static void XCentered(ImDrawList* dl, ImFont* f, float size, ImVec2 c, ImU32 col, const char* t) {
    ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0, t);
    XText(dl, f, size, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), col, t);
}
// letter-spaced caps title ("MOVE DATA", "FRAME DATA") centred on c
static void XTitle(ImDrawList* dl, ImFont* f, float size, ImVec2 c, ImU32 col, const char* t, float spacing) {
    float w = SpacedWidth(f, size, t, spacing);
    float x = c.x - w * 0.5f, y = c.y - size * 0.5f;
    SpacedText(dl, f, size, ImVec2(x, y), col, t, spacing);
    SpacedText(dl, f, size, ImVec2(x + size * 0.02f, y), col, t, spacing);
}
// the move list's data box: charcoal plate with pointed ends and a thin bronze frame set just inside it
static void XBox(ImDrawList* dl, ImVec2 a, ImVec2 b, float k, ImU32 fill = xc::Fill) {
    float my = (a.y + b.y) * 0.5f, c = (b.y - a.y) * 0.32f;
    ImVec2 o[6] = {ImVec2(a.x, my), ImVec2(a.x + c, a.y), ImVec2(b.x - c, a.y), ImVec2(b.x, my), ImVec2(b.x - c, b.y), ImVec2(a.x + c, b.y)};
    dl->AddConvexPolyFilled(o, 6, fill);
    float i = 4.f * k;
    ImVec2 n[6] = {ImVec2(a.x + i * 1.4f, my), ImVec2(a.x + c + i * 0.6f, a.y + i), ImVec2(b.x - c - i * 0.6f, a.y + i), ImVec2(b.x - i * 1.4f, my),
                   ImVec2(b.x - c - i * 0.6f, b.y - i), ImVec2(a.x + c + i * 0.6f, b.y - i)};
    dl->AddPolyline(n, 6, xc::Bronze, ImDrawFlags_Closed, 1.4f * k);
    dl->AddPolyline(o, 6, IM_COL32(10, 10, 9, 200), ImDrawFlags_Closed, 1.f * k);
}
// the move list header rule: a thin gold line with arrow tips at both ends
static void XRule(ImDrawList* dl, float x0, float x1, float y, float k) {
    dl->AddLine(ImVec2(x0 + 10 * k, y), ImVec2(x1 - 10 * k, y), xc::Gold, 2.f * k);
    float h = 4.f * k;
    dl->AddTriangleFilled(ImVec2(x0, y), ImVec2(x0 + 12 * k, y - h), ImVec2(x0 + 12 * k, y + h), xc::Gold);
    dl->AddTriangleFilled(ImVec2(x1, y), ImVec2(x1 - 12 * k, y - h), ImVec2(x1 - 12 * k, y + h), xc::Gold);
}
// selected row: grey bar, brightest on the left, with light top / bottom edges (the move list's cursor)
static void XSelBar(ImDrawList* dl, ImVec2 a, ImVec2 b, float k) {
    float m = a.x + (b.x - a.x) * 0.55f;
    ImU32 l = IM_COL32(150, 148, 142, 190), mid = IM_COL32(112, 110, 104, 150), r = IM_COL32(80, 78, 74, 30);
    dl->AddRectFilledMultiColor(a, ImVec2(m, b.y), l, mid, mid, l);
    dl->AddRectFilledMultiColor(ImVec2(m, a.y), b, mid, r, r, mid);
    dl->AddRectFilledMultiColor(a, ImVec2(b.x, a.y + 2 * k), IM_COL32(220, 218, 210, 220), IM_COL32(220, 218, 210, 20), IM_COL32(220, 218, 210, 20), IM_COL32(220, 218, 210, 220));
    dl->AddRectFilledMultiColor(ImVec2(a.x, b.y - 2 * k), b, IM_COL32(220, 218, 210, 160), IM_COL32(220, 218, 210, 10), IM_COL32(220, 218, 210, 10), IM_COL32(220, 218, 210, 160));
}
static void XDiamond(ImDrawList* dl, ImVec2 c, float r, ImU32 col, bool outline = false) {
    if (outline) dl->AddQuad(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y), col, 1.5f);
    else dl->AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y), col);
}
// a gamepad face button the way the game prints it: filled coloured disc with the letter
static void XPadButton(ImDrawList* dl, ImFont* f, float k, ImVec2 c, char btn) {
    ImU32 col = btn == 'A' ? IM_COL32(84, 186, 68, 255) : btn == 'B' ? IM_COL32(214, 52, 44, 255) : btn == 'X' ? IM_COL32(56, 116, 214, 255) : IM_COL32(230, 200, 30, 255);
    dl->AddCircleFilled(c, 16 * k, col, 24);
    dl->AddCircle(c, 16 * k, IM_COL32(255, 255, 255, 90), 24, 1.2f * k);
    char t[2] = {btn, 0};
    XCentered(dl, f, 22 * k, c, IM_COL32(255, 255, 255, 240), t);
}
// a short notice in the game's style: a data box with the text centred
static void XToast(ImDrawList* dl, ImFont* f, float k, ImVec2 c, const char* t) {
    float fs = 28 * k;
    ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, t);
    float w = ts.x + 110 * k, h = 58 * k;
    XBox(dl, ImVec2(c.x - w * 0.5f, c.y - h * 0.5f), ImVec2(c.x + w * 0.5f, c.y + h * 0.5f), k, xc::FillDeep);
    XCentered(dl, f, fs, c, xc::White, t);
}

// One 3-row table: label cell (black) | value cell (dark red)
static void DrawStatTable(ImDrawList* dl, ImVec2 o, float k, const char* const labels[3], const std::string values[3], const bool live[3]) {
    ImFont* f = g_hudFont ? g_hudFont : ImGui::GetFont();
    const float lw = 136 * k, vw = 66 * k, rh = 36.5f * k;
    const float fs = 19.f * k, vs = 20.f * k, sp = 1.6f * k;
    const ImU32 white = IM_COL32(235, 235, 235, 255), labelBg = IM_COL32(8, 8, 10, 190), valBg = IM_COL32(110, 22, 20, 215),
                valText = IM_COL32(236, 178, 160, 255), liveText = IM_COL32(255, 230, 200, 255);
    ImVec2 end(o.x + lw + vw, o.y + rh * 3);
    dl->AddRectFilled(o, ImVec2(o.x + lw, end.y), labelBg);
    dl->AddRectFilled(ImVec2(o.x + lw, o.y), end, valBg);
    for (int r = 0; r < 3; ++r) {
        float y = o.y + r * rh;
        float tw = SpacedWidth(f, fs, labels[r], sp);
        SpacedText(dl, f, fs, ImVec2(o.x + lw - 14 * k - tw, y + (rh - fs) * 0.5f), white, labels[r], sp);
        ImVec2 vsz = f->CalcTextSizeA(vs, FLT_MAX, 0.f, values[r].c_str());
        dl->AddText(f, vs, ImVec2(o.x + lw + 14 * k, y + (rh - vsz.y) * 0.5f), live[r] ? liveText : valText, values[r].c_str());
        if (r > 0) dl->AddLine(ImVec2(o.x, y), ImVec2(end.x, y), IM_COL32(235, 235, 235, 200), 1.2f * k);
    }
    dl->AddLine(ImVec2(o.x + lw, o.y), ImVec2(o.x + lw, end.y), IM_COL32(235, 235, 235, 200), 1.2f * k);
    dl->AddRect(o, end, white, 0.f, 0, 2.2f * k);
}

static std::string Num(int v) { return std::to_string(v); }
static std::string Adv(int v) { char b[16]; snprintf(b, sizeof(b), "%+d", v); return b; }

// stats to show: the move in progress once it has hit its first active frame, else the last finished move
static MoveStats Shown(const PlayerMeter& pm) {
    if (!(pm.inMove && pm.seenActive)) return pm.last;
    MoveStats s = pm.cur;
    s.valid = true;
    s.startup = pm.startupFrames;
    s.active = pm.lastActive - pm.firstActive + 1;
    s.recovery = pm.moveFrames - pm.lastActive;
    s.total = pm.moveFrames;
    s.offMismatch = false;
    return s;
}

static void DrawPlayerPanel(ImDrawList* dl, int p, ImVec2 o, float k) {
    const PlayerMeter& m = g_game.meter[p];
    const MoveStats& ms = m.last;
    static const char* const l1[3] = {"STARTUP", "ACTIVE", "RECOVERY"};
    static const char* const l2[3] = {"HIT", "BLOCK", "CANCEL"};
    std::string v1[3], v2[3];
    bool live1[3] = {false, false, false}, live2[3] = {false, false, false};
    if (m.inMove) {   // live while the move runs
        if (m.seenActive) { v1[0] = Num(m.startupFrames - 1); v1[1] = Num(m.activeFrames); v1[2] = Num(m.recoveryFrames); live1[1] = live1[2] = true; }
        else { v1[0] = Num(m.moveFrames); v1[1] = "-"; v1[2] = "-"; live1[0] = true; }
    } else if (ms.valid) {
        v1[0] = Num(ms.startup - 1); v1[1] = Num(ms.active); v1[2] = Num(ms.recovery + 1);
    } else { v1[0] = v1[1] = v1[2] = "-"; }
    v2[0] = m.hitValid ? Adv(m.hitAdv) : "-";
    v2[1] = m.blockValid ? Adv(m.blockAdv) : "-";
    v2[2] = m.cancelValid ? Adv(m.cancelAdv) : (m.cancelUnknown ? "?" : "-");
    if (m.liveCell == 0 || m.liveCell == 1) { v2[m.liveCell] = Adv(m.liveAdv); live2[m.liveCell] = true; }
    DrawStatTable(dl, o, k, l1, v1, live1);
    DrawStatTable(dl, ImVec2(o.x + (136 + 66 + 22) * k, o.y), k, l2, v2, live2);
}

static std::string HitText(const PlayerMeter& m) {
    if (m.liveCell == 0) return Adv(m.liveAdv) + "...";
    if (!m.hitValid) return "-";
    std::string t = Adv(m.hitAdv);
    if (m.hitKD && m.kdFrameKill)
        t += " (after " + (m.kdAfter.empty() ? std::string("follow-up") : m.kdAfter) + "; knockdown " + Adv(m.kdBase) + ")";
    else if (m.hitKD) t += m.kdDelay > 0 ? " (knockdown, opponent delayed +" + std::to_string(m.kdDelay) + ")" : " (knockdown)";
    return t;
}


// ------------------------------------------------------------------ game-styled frame data panel
// Mirrors the move list's FRAME DATA block: a letter-spaced title over charcoal boxes with pointed ends and
// thin bronze frames; tan "LABEL:" captions over large white values. The frame meter sits in its own box.
static void DrawFrameMeterGame(float sw, float sh) {
    const float k = g_ui.hudScale;
    const int cells = 80;
    const float side = 38 * k, cw = 9 * k, ch = 15 * k, cg = 1 * k, labelW = 36 * k;
    const float W = side * 2 + labelW + cells * (cw + cg);
    const float titleH = 40 * k, meterH = 2 * ch + 7 * k + 28 * k, statH = 80 * k, gapH = 9 * k, lineH = 44 * k;
    ImFont* mf = g_menuFont ? g_menuFont : ImGui::GetFont();
    ImFont* vf = g_valueFont ? g_valueFont : mf;
    const float ls = 21 * k, vs = 35 * k;

    const PlayerMeter& pm = g_game.meter[0];
    const MoveStats ms = Shown(pm);
    bool haveList = ms.valid && ms.off;
    bool haveGaps = !pm.gaps.empty();
    float H = titleH + meterH + gapH + (statH + gapH) * 2 + (haveList ? lineH + gapH : 0) + (haveGaps ? lineH + gapH : 0);

    ImGui::SetNextWindowPos(ImVec2((sw - W) * 0.5f, sh * 0.66f), ImGuiCond_FirstUseEver);
    ImGuiWindowFlags fl = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_AlwaysAutoResize |
                          ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar;
    if (!g_ui.menuOpen) fl |= ImGuiWindowFlags_NoInputs;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::Begin("##framemeter_game", nullptr, fl);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(W, H));

    // title, with a soft shadow band behind it so it reads over bright stages
    float y = o.y;
    dl->AddRectFilledMultiColor(ImVec2(o.x + W * 0.25f, y + 4 * k), ImVec2(o.x + W * 0.5f, y + titleH - 2 * k), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 120), IM_COL32(0, 0, 0, 120), IM_COL32(0, 0, 0, 0));
    dl->AddRectFilledMultiColor(ImVec2(o.x + W * 0.5f, y + 4 * k), ImVec2(o.x + W * 0.75f, y + titleH - 2 * k), IM_COL32(0, 0, 0, 120), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 120));
    XTitle(dl, mf, 24 * k, ImVec2(o.x + W * 0.5f, y + titleH * 0.5f), IM_COL32(232, 230, 224, 255), "FRAME DATA", 6.f * k);
    if (g_game.paused || mkx::Frozen()) {
        const char* fz = "FROZEN";
        float fw = SpacedWidth(mf, 15 * k, fz, 3.f * k);
        XDiamond(dl, ImVec2(o.x + W - side - fw - 16 * k, y + titleH * 0.5f), 6 * k, xc::Gold);
        SpacedText(dl, mf, 15 * k, ImVec2(o.x + W - side - fw, y + (titleH - 15 * k) * 0.5f), xc::Live, fz, 3.f * k);
    }
    y += titleH;

    // frame meter box
    XBox(dl, ImVec2(o.x, y), ImVec2(o.x + W, y + meterH), k);
    {
        float by = y + 13 * k;
        for (int p = 0; p < 2; ++p) {
            const PlayerMeter& m = g_game.meter[p];
            XText(dl, mf, 19 * k, ImVec2(o.x + side, by + (ch - 19 * k) * 0.5f - 1 * k), xc::Tan, p == 0 ? "P1" : "P2");
            size_t n = m.history.size(), start = n > (size_t)cells ? n - cells : 0;
            float x0 = o.x + side + labelW;
            for (int i = 0; i < cells; ++i) {
                size_t idx = start + i;
                FState s = idx < n ? m.history[idx] : FState::None;
                ImVec2 a(x0 + i * (cw + cg), by), b(a.x + cw, a.y + ch);
                dl->AddRectFilled(a, b, StateColor(s));
                if (i % 10 == 9) dl->AddLine(ImVec2(b.x + cg * 0.5f, by - 2 * k), ImVec2(b.x + cg * 0.5f, b.y + 2 * k), IM_COL32(204, 182, 140, 90), 1.f);
            }
            by += ch + 7 * k;
        }
    }
    y += meterH + gapH;

    // stat boxes: 2 rows x 3 columns, like the move list's FRAME DATA panel
    char v[6][48];
    ImU32 vc[6] = {xc::White, xc::White, xc::White, xc::White, xc::White, xc::White};
    const char* lab[6] = {"START-UP:", "ACTIVE:", "RECOVERY:", "HIT ADV:", "CANCEL:", "TOTAL:"};
    ImU32 mark[6] = {StateColor(FState::Startup), StateColor(FState::Active), StateColor(FState::Recovery), StateColor(FState::Hitstun), 0, 0};
    auto advCol = [&](int a) { return a > 0 ? xc::Plus : a < 0 ? xc::Minus : xc::White; };
    if (pm.inMove && !pm.seenActive) {
        snprintf(v[0], 48, "%d", pm.moveFrames); snprintf(v[1], 48, "-"); snprintf(v[2], 48, "-"); snprintf(v[5], 48, "%d", pm.moveFrames);
        vc[0] = xc::Live;
    } else if (ms.valid) {
        snprintf(v[0], 48, "%d", ms.startup - 1); snprintf(v[1], 48, "%d", ms.active); snprintf(v[2], 48, "%d", ms.recovery + 1);
        snprintf(v[5], 48, "%d", ms.total);
    } else {
        for (int i : {0, 1, 2, 5}) snprintf(v[i], 48, "-");
    }
    bool showBlock = pm.liveCell == 1 || (pm.liveCell != 0 && pm.blockValid);
    bool showHit = pm.liveCell == 0 || (pm.liveCell != 1 && pm.hitValid);
    if (showBlock && !showHit) { lab[3] = "BLOCK ADV:"; mark[3] = StateColor(FState::Blockstun); }
    if (pm.liveCell == 0 || pm.liveCell == 1) { snprintf(v[3], 48, "%+d", pm.liveAdv); vc[3] = xc::Live; }
    else if (showHit) { snprintf(v[3], 48, "%+d", pm.hitAdv); vc[3] = advCol(pm.hitAdv); }
    else if (showBlock) { snprintf(v[3], 48, "%+d", pm.blockAdv); vc[3] = advCol(pm.blockAdv); }
    else snprintf(v[3], 48, "-");
    if (ms.valid && ms.off) snprintf(v[4], 48, ms.offCancel ? "%d" : "N/A", ms.offCancel);
    else snprintf(v[4], 48, "-");
    const char* sub3 = nullptr;
    if (showHit && pm.liveCell != 0 && pm.hitKD) sub3 = pm.kdDelay > 0 ? "KNOCKDOWN (DELAYED)" : "KNOCKDOWN";
    static std::string fkSub;
    if (showHit && pm.liveCell != 0 && pm.hitKD && pm.kdFrameKill) {
        // frame kill: the value is what is left after the follow-up; the knockdown's own value goes underneath
        std::string n = pm.kdAfter.empty() ? std::string("FOLLOW-UP") : pm.kdAfter;
        for (char& c : n) c = (char)toupper((unsigned char)c);
        char b[24]; snprintf(b, sizeof(b), "%+d", pm.kdBase);
        fkSub = "AFTER " + n + " (KD " + b + ")";
        sub3 = fkSub.c_str();
    }

    float colW = (W - side * 2) / 3.f;
    for (int r = 0; r < 2; ++r) {
        XBox(dl, ImVec2(o.x, y), ImVec2(o.x + W, y + statH), k);
        for (int c = 0; c < 3; ++c) {
            int i = r * 3 + c;
            float cx = o.x + side + colW * c + colW * 0.5f;
            ImVec2 lsz = mf->CalcTextSizeA(ls, FLT_MAX, 0.f, lab[i]);
            float lx = cx - lsz.x * 0.5f, ly = y + 10 * k;
            XText(dl, mf, ls, ImVec2(lx, ly), xc::Tan, lab[i]);
            if (mark[i]) XDiamond(dl, ImVec2(lx - 13 * k, ly + lsz.y * 0.55f), 5.5f * k, mark[i]);
            ImVec2 vsz = vf->CalcTextSizeA(vs, FLT_MAX, 0.f, v[i]);
            XText(dl, vf, vs, ImVec2(cx - vsz.x * 0.5f, y + statH - 10 * k - vsz.y), vc[i], v[i]);
            if (i == 3 && sub3) {
                ImVec2 ss = mf->CalcTextSizeA(14 * k, FLT_MAX, 0.f, sub3);
                XText(dl, mf, 14 * k, ImVec2(cx + vsz.x * 0.5f + 10 * k, y + statH - 12 * k - ss.y), xc::Tan, sub3);
            }
        }
        y += statH + gapH;
    }

    // the game's own move-list entry for this move
    if (haveList) {
        XBox(dl, ImVec2(o.x, y), ImVec2(o.x + W, y + lineH), k, xc::FillDeep);
        char t[200];
        snprintf(t, sizeof(t), "%s  (%s)    %d / %d / %d    BLOCK %+d    HIT %+d", movelist::Notation(ms.offName).c_str(), movelist::TypeName(ms.offType), ms.offStartup,
                 ms.offActive, ms.offRecovery, ms.offBlock, ms.offHit);
        float cy = y + lineH * 0.5f;
        SpacedText(dl, mf, 16 * k, ImVec2(o.x + side, cy - 8.5f * k), xc::Tan, "MOVE LIST", 3.f * k);
        ImVec2 ts = mf->CalcTextSizeA(22 * k, FLT_MAX, 0, t);
        XText(dl, mf, 22 * k, ImVec2(o.x + side + 140 * k, cy - ts.y * 0.5f), xc::Text, t);
        if (ms.offMismatch) {
            const char* md = "MEASURED DIFFERS";
            float mw = SpacedWidth(mf, 13 * k, md, 2.f * k);
            SpacedText(dl, mf, 13 * k, ImVec2(o.x + W - side - mw, cy - 7 * k), IM_COL32(236, 160, 80, 255), md, 2.f * k);
        }
        y += lineH + gapH;
    }
    // gaps between the hits of a string / block string
    if (haveGaps) {
        XBox(dl, ImVec2(o.x, y), ImVec2(o.x + W, y + lineH), k, xc::FillDeep);
        float cy = y + lineH * 0.5f;
        SpacedText(dl, mf, 16 * k, ImVec2(o.x + side, cy - 8.5f * k), xc::Tan, pm.gapsBlocked ? "GAPS ON BLOCK" : "GAPS ON HIT", 3.f * k);
        float gx = o.x + side + 190 * k;
        for (int gp : pm.gaps) {
            char t[40];
            snprintf(t, sizeof(t), "%dF", gp);
            ImU32 gc = xc::Text;
            ImVec2 ts = mf->CalcTextSizeA(21 * k, FLT_MAX, 0, t);
            XDiamond(dl, ImVec2(gx + 6 * k, cy), 5.5f * k, gc);
            XText(dl, mf, 21 * k, ImVec2(gx + 20 * k, cy - ts.y * 0.5f), gc, t);
            gx += ts.x + 44 * k;
        }
        y += lineH + gapH;
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

static void DrawFrameMeter(float sw, float sh) {
    const int cells = 80;
    const float cw = 9.f, chh = 16.f, gap = 1.f;
    float width = cells * (cw + gap) + 16;
    ImGui::SetNextWindowPos(ImVec2((sw - width) * 0.5f, sh * 0.80f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.72f);
    ImGuiWindowFlags fl = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                          ImGuiWindowFlags_NoNav;
    if (!g_ui.menuOpen) fl |= ImGuiWindowFlags_NoInputs;
    ImGui::Begin("##framemeter", nullptr, fl);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int p = 0; p < 2; ++p) {
        const PlayerMeter& m = g_game.meter[p];
        ImGui::TextColored(p == 0 ? ImVec4(1, .86f, .25f, 1) : ImVec4(.25f, .86f, 1, 1), "P%d", p + 1);
        ImGui::SameLine(34);
        ImVec2 o = ImGui::GetCursorScreenPos();
        size_t n = m.history.size();
        size_t start = n > (size_t)cells ? n - cells : 0;
        for (int i = 0; i < cells; ++i) {
            size_t idx = start + i;
            FState s = idx < n ? m.history[idx] : FState::None;
            ImVec2 a(o.x + i * (cw + gap), o.y), b(a.x + cw, a.y + chh);
            dl->AddRectFilled(a, b, StateColor(s));
        }
        ImGui::Dummy(ImVec2(cells * (cw + gap), chh));
    }

    // move line (live while the move runs)
    const PlayerMeter& pm = g_game.meter[0];
    const MoveStats ms = Shown(pm);
    char buf[256];
    // During strings / combos keep showing the last move that finished (or was chained into the next),
    // with the move currently in progress as a short tail.
    char cur[96] = "";
    if (pm.inMove) {
        if (pm.seenActive) snprintf(cur, sizeof(cur), "   [now %d/%d/%d...]", pm.startupFrames - 1, pm.activeFrames, pm.recoveryFrames + 1);
        else snprintf(cur, sizeof(cur), "   [now startup %d...]", pm.moveFrames);
    }
    // move line: labels coloured like the bars (Startup / Active / Recovery)
    auto colOf = [](FState f) { return ImGui::ColorConvertU32ToFloat4(StateColor(f)); };
    ImGui::TextUnformatted("P1 move ");
    ImGui::SameLine(0, 4);
    if (ms.valid) {
        ImGui::TextColored(colOf(FState::Startup), "Startup %d", ms.startup - 1); ImGui::SameLine(0, 14);
        ImGui::TextColored(colOf(FState::Active), "Active %d", ms.active); ImGui::SameLine(0, 14);
        ImGui::TextColored(colOf(FState::Recovery), "Recovery %d", ms.recovery + 1); ImGui::SameLine(0, 14);
        ImGui::Text("Total %d%s%s%s", ms.total, ms.chained ? " (chained)" : "", ms.fromCache ? "  (whiff/block data)" : "", cur);
    } else if (pm.noHitbox && ms.total > 0) {
        ImGui::Text("Total %d   (no hitbox found - throw, stance or movement?)%s", ms.total, cur);
    } else {
        ImGui::Text("-%s", cur);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("   P1 %s | P2 %s%s", FStateName(g_game.p[0].fstate), FStateName(g_game.p[1].fstate),
                        (g_game.paused || mkx::Frozen()) ? "   [FROZEN]" : "");
    // official move-list values for the identified move
    if (ms.valid && ms.off) {
        ImGui::TextDisabled("Move list: %s (%s)  %d / %d / %d   block %+d   hit %+d   cancel %s", movelist::Notation(ms.offName).c_str(), movelist::TypeName(ms.offType),
                            ms.offStartup, ms.offActive, ms.offRecovery, ms.offBlock, ms.offHit,
                            ms.offCancel ? std::to_string(ms.offCancel).c_str() : "N/A");
        if (ms.offMismatch) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, .55f, .2f, 1), "  measured differs"); }
    } else if (movelist::Scanning()) {
        ImGui::TextDisabled("Move list: loading...");
    }

    // advantage line: Hit / Block / Cancel
    auto advCol = [](int a) { return a > 0 ? ImVec4(.4f, 1, .5f, 1) : a < 0 ? ImVec4(1, .45f, .45f, 1) : ImVec4(1, 1, 1, 1); };
    // show either hit or block advantage - whichever the latest (or live) interaction was
    bool showBlock = pm.liveCell == 1 || (pm.liveCell != 0 && pm.blockValid);
    bool showHit = pm.liveCell == 0 || (pm.liveCell != 1 && pm.hitValid);
    if (showHit) {
        ImGui::TextColored(colOf(FState::Hitstun), "Hit advantage");
        ImGui::SameLine();
        ImGui::TextColored(advCol(pm.liveCell == 0 ? pm.liveAdv : pm.hitAdv), "%s", HitText(pm).c_str());
        ImGui::SameLine(0, 24);
    } else if (showBlock) {
        ImGui::TextColored(colOf(FState::Blockstun), "Block advantage");
        ImGui::SameLine();
        if (pm.liveCell == 1) ImGui::TextColored(advCol(pm.liveAdv), "%s...", Adv(pm.liveAdv).c_str());
        else ImGui::TextColored(advCol(pm.blockAdv), "%s", Adv(pm.blockAdv).c_str());
        ImGui::SameLine(0, 24);
    } else {
        ImGui::TextDisabled("Advantage -");
        ImGui::SameLine(0, 24);
    }
    ImGui::TextUnformatted("Cancel");
    ImGui::SameLine();
    if (ms.valid && ms.off) ImGui::Text("%s", ms.offCancel ? Adv(ms.offCancel).c_str() : "N/A");
    else if (pm.cancelValid) ImGui::TextColored(advCol(pm.cancelAdv), "%s", Adv(pm.cancelAdv).c_str());
    else ImGui::TextDisabled("-");
    // gaps in the last string / multi-hit special: free frames the defender had between hits
    if (!pm.gaps.empty()) {
        ImGui::TextUnformatted(pm.gapsBlocked ? "Gaps on block:" : "Gaps on hit:");
        for (int gp : pm.gaps) {
            ImGui::SameLine();
            ImGui::Text("%df", gp);
        }
    } else if (pm.blockValid || pm.hitValid) {
        ImGui::TextDisabled("No gaps in the last string");
    }
    ImGui::End();
}

// ------------------------------------------------------------------ struct explorer
struct Watch { std::string label; uintptr_t addr; int type; }; // 0 int32, 1 float, 2 u8, 3 ptr
static std::vector<Watch> g_watches;

static char g_exAddrText[256] = "p1";
static int g_exSize = 0x400;
static int g_exView = 0; // 0 int32, 1 float, 2 hex
static std::vector<uint8_t> g_exCur, g_exPrev, g_snapA, g_snapB;
static std::vector<uint32_t> g_exLastChange; // frame index of last change per dword
static uintptr_t g_exAddr = 0;
static std::string g_exErr;
static bool g_onlyChanged = false, g_onlyDiff = false;

static uintptr_t EvalText(const char* text, std::string* err) {
    auto e = mem::Expr::Parse(text, err);
    if (!e) return 0;
    mem::EvalCtx c;
    if (g_cfg.p1) c.p1 = g_cfg.p1->Eval(c);
    if (g_cfg.p2) c.p2 = g_cfg.p2->Eval(c);
    c.self = c.p1;
    uintptr_t a = e->Eval(c);
    if (!a && err) *err = "expression evaluated to 0 (unresolved pointer?)";
    return a;
}

static void ExplorerRead() {
    g_exAddr = EvalText(g_exAddrText, &g_exErr);
    if (!g_exAddr) return;
    g_exErr.clear();
    size_t n = (size_t)g_exSize;
    std::vector<uint8_t> buf(n);
    if (!mem::ReadRaw(g_exAddr, buf.data(), n)) {
        // read what we can, dword by dword
        for (size_t i = 0; i + 4 <= n; i += 4) if (!mem::ReadRaw(g_exAddr + i, &buf[i], 4)) memset(&buf[i], 0xCD, 4);
    }
    if (g_exCur.size() != n) { g_exCur = buf; g_exPrev = buf; g_exLastChange.assign(n / 4, 0); return; }
    g_exPrev = g_exCur;
    g_exCur = buf;
    for (size_t i = 0; i + 4 <= n; i += 4)
        if (memcmp(&g_exCur[i], &g_exPrev[i], 4) != 0) g_exLastChange[i / 4] = (uint32_t)g_game.logicalFrame;
}

static void DrawExplorer() {
    ImGui::SetNextWindowSize(ImVec2(560, 600), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Struct Explorer", &g_ui.showExplorer)) { ImGui::End(); return; }
    ImGui::TextWrapped("Live view of memory at an address expression. Use p1 / p2 for the player objects, "
                       "e.g. p1, [p1+40], rva:1234. Green rows changed in the last 30 frames.");
    ImGui::SetNextItemWidth(260);
    ImGui::InputText("Address", g_exAddrText, sizeof(g_exAddrText));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::InputInt("Size", &g_exSize, 0x100, 0x400, ImGuiInputTextFlags_CharsHexadecimal);
    if (g_exSize < 0x10) g_exSize = 0x10;
    if (g_exSize > 0x4000) g_exSize = 0x4000;
    ImGui::RadioButton("int32", &g_exView, 0); ImGui::SameLine();
    ImGui::RadioButton("float", &g_exView, 1); ImGui::SameLine();
    ImGui::RadioButton("hex", &g_exView, 2); ImGui::SameLine();
    ImGui::Checkbox("Only changing", &g_onlyChanged);

    if (ImGui::Button("Snapshot A")) g_snapA = g_exCur;
    ImGui::SameLine();
    if (ImGui::Button("Snapshot B")) g_snapB = g_exCur;
    ImGui::SameLine();
    ImGui::Checkbox("Only A != B", &g_onlyDiff);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Snapshot A while idle, do a move and freeze (F5) on an active frame, Snapshot B.\n"
                          "Offsets that differ are candidates for state / active-flag fields.");

    ExplorerRead();
    if (!g_exErr.empty()) ImGui::TextColored(ImVec4(1, .4f, .4f, 1), "%s", g_exErr.c_str());
    else ImGui::Text("Base: %llX", (unsigned long long)g_exAddr);

    bool diffMode = g_onlyDiff && g_snapA.size() == g_exCur.size() && g_snapB.size() == g_exCur.size();
    if (ImGui::BeginTable("ex", diffMode ? 5 : 4, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders,
                          ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Value");
        if (diffMode) ImGui::TableSetupColumn("A -> B");
        ImGui::TableSetupColumn("Ptr?", ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableHeadersRow();
        for (size_t off = 0; off + 4 <= g_exCur.size(); off += 4) {
            bool recent = g_game.logicalFrame - g_exLastChange[off / 4] < 30 && g_exLastChange[off / 4] != 0;
            if (g_onlyChanged && !recent) continue;
            if (diffMode && memcmp(&g_snapA[off], &g_snapB[off], 4) == 0) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("+%03zX", off);
            ImGui::TableNextColumn();
            int32_t iv; float fv; memcpy(&iv, &g_exCur[off], 4); memcpy(&fv, &g_exCur[off], 4);
            ImVec4 col = recent ? ImVec4(.4f, 1, .5f, 1) : ImVec4(.85f, .85f, .85f, 1);
            if (g_exView == 0) ImGui::TextColored(col, "%d", iv);
            else if (g_exView == 1) ImGui::TextColored(col, "%.4f", fv);
            else ImGui::TextColored(col, "%08X", (uint32_t)iv);
            if (diffMode) {
                ImGui::TableNextColumn();
                int32_t a, b; memcpy(&a, &g_snapA[off], 4); memcpy(&b, &g_snapB[off], 4);
                if (g_exView == 1) { float fa, fb; memcpy(&fa, &a, 4); memcpy(&fb, &b, 4); ImGui::Text("%.3f -> %.3f", fa, fb); }
                else ImGui::Text("%d -> %d", a, b);
            }
            ImGui::TableNextColumn();
            ImGui::PushID((int)off);
            if (off % 8 == 0 && off + 8 <= g_exCur.size()) {
                uint64_t pv; memcpy(&pv, &g_exCur[off], 8);
                if (mem::LooksLikePointer(pv) && ImGui::SmallButton("go")) {
                    std::string t = std::string("[") + g_exAddrText + "+" ;
                    char h[32]; snprintf(h, sizeof(h), "%zX]", off);
                    t += h;
                    strncpy(g_exAddrText, t.c_str(), sizeof(g_exAddrText) - 1);
                    g_exCur.clear();
                }
            }
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("watch")) {
                char lbl[300]; snprintf(lbl, sizeof(lbl), "%s+%zX", g_exAddrText, off);
                g_watches.push_back({lbl, g_exAddr + off, g_exView == 1 ? 1 : 0});
                g_ui.showWatches = true;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

static void DrawWatches() {
    ImGui::SetNextWindowSize(ImVec2(460, 260), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Watches", &g_ui.showWatches)) { ImGui::End(); return; }
    ImGui::TextDisabled("Pinned addresses. Copy the label into MKXFrameLab.ini (replace the p1 prefix with self for [Fields]).");
    int remove = -1;
    for (size_t i = 0; i < g_watches.size(); ++i) {
        Watch& w = g_watches[i];
        ImGui::PushID((int)i);
        const char* types[] = {"int32", "float", "u8", "ptr"};
        ImGui::SetNextItemWidth(70);
        ImGui::Combo("##t", &w.type, types, 4);
        ImGui::SameLine();
        switch (w.type) {
        case 0: ImGui::Text("%-12d", mem::ReadOr<int32_t>(w.addr, 0)); break;
        case 1: ImGui::Text("%-12.4f", mem::ReadOr<float>(w.addr, 0)); break;
        case 2: ImGui::Text("%-12u", (unsigned)mem::ReadOr<uint8_t>(w.addr, 0)); break;
        case 3: ImGui::Text("%llX", (unsigned long long)mem::ReadOr<uint64_t>(w.addr, 0)); break;
        }
        ImGui::SameLine(220);
        ImGui::TextUnformatted(w.label.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("copy")) ImGui::SetClipboardText(w.label.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) remove = (int)i;
        ImGui::PopID();
    }
    if (remove >= 0) g_watches.erase(g_watches.begin() + remove);
    ImGui::End();
}

static void DrawCollList() {
    ImGui::SetNextWindowSize(ImVec2(620, 380), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Collision objects", &g_ui.showCollList)) { ImGui::End(); return; }
    mkx::Snapshot sn = mkx::GetSnapshot();
    for (const auto& t : sn.live)
        ImGui::Text("%-24s %llX  born tick %llu  caller %llX  args %llX %llX", mkx::KindName(t.kind), (unsigned long long)t.ptr,
                    (unsigned long long)t.createdTick, (unsigned long long)t.callerRva, (unsigned long long)t.arg1,
                    (unsigned long long)t.arg2);
    ImGui::Separator();
    for (const auto& sh : sn.shapes)
        ImGui::Text("%-22s type %d state %d owner P%d  r %.1f w %.1f l %.1f  pos %.0f %.0f %.0f %s", mkx::KindName(sh.kind), sh.type, sh.state,
                    sh.owner + 1, sh.radius, sh.width, sh.length, sh.pos.x, sh.pos.y, sh.pos.z, sh.hasMat ? "mat" : "");
    ImGui::Separator();
    auto ev = mkx::RecentEvents();
    for (int i = (int)ev.size() - 1; i >= 0 && i >= (int)ev.size() - 40; --i) ImGui::TextUnformatted(ev[i].c_str());
    ImGui::End();
}

// ------------------------------------------------------------------ main menu
static void DrawMenu() {
    // after every (re)load the window starts at the top left, expanded and sized to its contents
    static bool placed = false;
    if (!placed) {
        placed = true;
        ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
        ImGui::SetNextWindowCollapsed(false, ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Always);
    }
    ImGui::Begin("MKX Frame Lab", &g_ui.menuOpen);
    ImGui::TextDisabled("Build %s %s", __DATE__, __TIME__);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reload mod")) g_reloadRequested = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Unloads the mod and loads the newest MKXFrameLab_core.dll (also Ctrl+F5).\nNo game restart needed.");
    ImGui::TextColored(ImVec4(1, .8f, .3f, 1), "Offline / Practice mode only. Do not use online.");
    if (g_game.locked) ImGui::TextColored(ImVec4(1, .4f, .4f, 1), "%s", g_game.lockReason.c_str());
    if (g_cfg.demo) ImGui::TextColored(ImVec4(.5f, .8f, 1, 1), "DEMO MODE: meter shows a scripted pattern (Demo=1 in ini).");


    if (ImGui::CollapsingHeader("Mortal Kombat X link", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextWrapped("%s", mkx::Status().c_str());
        if (mkx::Ready()) {
            mkx::Snapshot sn = mkx::GetSnapshot();
            ImGui::Text("Tick %llu   %s", (unsigned long long)sn.tick, mkx::Frozen() ? "[FROZEN]" : "");
            for (int i = 0; i < 2; ++i) {
                const auto& p = sn.p[i];
                if (p.valid)
                    ImGui::Text("P%d state %04X move %d attack %d  hurt %d strike %d  pos %.0f %.0f %.0f", i + 1, p.stateFlags, p.moveId,
                                p.meleeState, (int)p.hurt.size(), (int)p.strike.size(), p.pos.x, p.pos.y, p.pos.z);
                else
                    ImGui::TextDisabled("P%d not in a fight", i + 1);
            }
            if (sn.camValid)
                ImGui::Text("Camera %.0f %.0f %.0f  rot %d %d %d  fov %.1f", sn.camPos.x, sn.camPos.y, sn.camPos.z, sn.camRot.pitch,
                            sn.camRot.yaw, sn.camRot.roll, sn.camFov);
            else
                ImGui::TextDisabled("Camera not captured yet");
            ImGui::Checkbox("Hurtboxes", &g_ui.showHurtPts); ImGui::SameLine();
            ImGui::Checkbox("Hitboxes", &g_ui.showStrike); ImGui::SameLine();
            ImGui::SetNextItemWidth(90);
            ImGui::SliderInt("Hitbox linger", &g_ui.strikeLinger, 0, 30, "%d f");
            ImGui::Checkbox("Projectiles / shaped attacks", &g_ui.showShapes); ImGui::SameLine();
            ImGui::Checkbox("incl. upcoming (dashed)", &g_ui.shapesInactive);
            ImGui::Text("Shaped collision objects: %d", (int)sn.shapes.size());
            ImGui::Checkbox("Part ids", &g_ui.hurtLabels); ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            ImGui::DragFloat("r=0 size", &g_ui.zeroRadius, 0.25f, 0.f, 30.f, "%.1f");
            ImGui::Checkbox("Show bones", &g_ui.showBones); ImGui::SameLine();
            ImGui::Checkbox("Bone names", &g_ui.boneLabels); ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            ImGui::DragFloat("Joint radius", &g_ui.boneRadius, 0.25f, 0.f, 40.f, "%.1f");
            if (ImGui::Button(mkx::Frozen() ? "Unfreeze (F5)" : "Freeze (F5)")) mkx::SetFrozen(!mkx::Frozen());
            ImGui::SameLine();
            if (ImGui::Button("Step (F6)")) mkx::StepOnce();
            ImGui::Separator();
            ImGui::SetNextItemWidth(120);
            ImGui::InputText("Label", g_ui.recLabel, sizeof(g_ui.recLabel));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90);
            ImGui::InputInt("Ticks", &g_ui.recTicks, 60, 120);
            if (g_ui.recTicks < 10) g_ui.recTicks = 10;
            if (g_ui.recTicks > 900) g_ui.recTicks = 900;
            if (mkx::Recording()) ImGui::Text("Recording... %d%%", mkx::RecordProgress());
            else if (ImGui::Button("Record (F9)")) mkx::StartRecording(g_ui.recTicks, g_ui.recLabel);
            std::string last = mkx::LastRecordingPath();
            if (!last.empty()) ImGui::TextWrapped("Last: %s", last.c_str());
            if (trace::Active()) ImGui::TextColored(ImVec4(1, .8f, .3f, 1), "Tracing hit-test reads... attack now!");
            else if (ImGui::Button("Trace hit test (F10)")) UiStartTrace();
            ImGui::SameLine();
            if (mkx::MeleeTraceArmed()) ImGui::TextColored(ImVec4(1, .8f, .3f, 1), "Armed: attack now");
            else if (ImGui::Button("Trace next attack (F11)")) mkx::ArmMeleeTrace();
            if (!trace::LastFile().empty()) ImGui::TextWrapped("Trace: %s", trace::LastFile().c_str());
            ImGui::Text("Live collision objects: %d", (int)sn.live.size());
            ImGui::SameLine();
            ImGui::Checkbox("List", &g_ui.showCollList);
        }
    }
    if (ImGui::CollapsingHeader("Input buffer")) {
        ImGui::TextDisabled("%s", ibuf::Status().c_str());
        int slot = ibuf::GameBufferSlot();
        if (slot >= 0) ImGui::TextColored(ImVec4(.4f, 1, .5f, 1), "Game buffered-move slot: move #%d", slot);
        else ImGui::TextDisabled("Game buffered-move slot: empty");
        ImGui::Checkbox("Extended buffer (offline only)", &ibuf::g_enabled);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        ImGui::SliderInt("window", &ibuf::g_window, 1, 15, "%d frames");
        ImGui::TextDisabled("Pad 1 (XInput). Presses made while busy are re-pressed on your first free frame.");
        ImGui::Checkbox("Probe log", &ibuf::g_probe);
        if (ImGui::BeginTable("probe", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Pressed early");
            ImGui::TableSetupColumn("Came out");
            ImGui::TableSetupColumn("Game buffered");
            ImGui::TableSetupColumn("Our buffer");
            ImGui::TableSetupColumn("Buttons");
            ImGui::TableHeadersRow();
            auto rows = ibuf::Rows();
            for (size_t i = 0; i < rows.size() && i < 12; ++i) {
                const auto& r = rows[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%d f", r.framesEarly);
                ImGui::TableNextColumn(); ImGui::TextColored(r.cameOut ? ImVec4(.4f, 1, .5f, 1) : ImVec4(1, .45f, .45f, 1), r.cameOut ? "yes" : "no");
                ImGui::TableNextColumn(); ImGui::TextUnformatted(r.gameBuffered ? "yes" : "-");
                ImGui::TableNextColumn(); ImGui::TextUnformatted(r.injected ? "re-pressed" : "-");
                ImGui::TableNextColumn();
                char b[64] = "";
                if (r.buttons & 0x4000) strcat(b, "X ");
                if (r.buttons & 0x8000) strcat(b, "Y ");
                if (r.buttons & 0x2000) strcat(b, "B ");
                if (r.buttons & 0x1000) strcat(b, "A ");
                if (r.buttons & 0x0100) strcat(b, "LB ");
                if (r.buttons & 0x0200) strcat(b, "RB ");
                ImGui::TextUnformatted(b);
            }
            ImGui::EndTable();
        }
    }
    if (ImGui::CollapsingHeader("Display")) {
        ImGui::Checkbox("Frame meter (F4)", &g_ui.showMeter);
        ImGui::SameLine();
        ImGui::Checkbox("Classic look", &g_ui.classicMeter);
        ImGui::SliderFloat("Panel size", &g_ui.hudScale, 0.7f, 1.6f, "%.2f");
        ImGui::Checkbox("Hit / hurt shapes (F3)", &g_ui.showBoxes);
        ImGui::Indent();
        ImGui::Checkbox("Hurt", &g_ui.showHurt); ImGui::SameLine();
        ImGui::Checkbox("Hit", &g_ui.showHit); ImGui::SameLine();
        ImGui::Checkbox("Throw", &g_ui.showThrow); ImGui::SameLine();
        ImGui::Checkbox("Push", &g_ui.showPush); ImGui::SameLine();
        ImGui::Checkbox("Other", &g_ui.showOther);
        ImGui::SliderFloat("Hitbox opacity", &g_ui.hitAlpha, 0.1f, 4.f, "%.2f");
        ImGui::SliderFloat("Hurtbox opacity", &g_ui.hurtAlpha, 0.1f, 4.f, "%.2f");
        ImGui::Checkbox("Filled", &g_ui.fill); ImGui::SameLine();
        ImGui::Checkbox("Player position markers", &g_ui.showMarkers);
        ImGui::Unindent();
        if (ImGui::Button(g_game.paused ? "Resume (F5)" : "Pause (F5)")) GameTogglePause();
        ImGui::SameLine();
        if (ImGui::Button("Step 1 frame (F6)")) GameStep();
        if (!g_cfg.gameSpeed) { ImGui::SameLine(); ImGui::TextDisabled("(meter-only: set GameSpeed)"); }
    }

    if (ImGui::CollapsingHeader("Live values")) {
        for (int i = 0; i < 2; ++i) {
            const PlayerSample& p = g_game.p[i];
            ImGui::Text("P%d obj %llX %s", i + 1, (unsigned long long)p.object, p.valid ? "" : "(unresolved)");
            if (!p.valid) continue;
            ImGui::Text("  pos %.1f %.1f %.1f  state %d  moveFrame %d", p.pos[0], p.pos[1], p.pos[2], p.state, p.moveFrame);
            ImGui::Text("  actionable %d  active %d  hitstun %d  blockstun %d  -> %s", p.actionable, p.attackActive,
                        p.hitstun, p.blockstun, FStateName(p.fstate));
        }
        ImGui::Text("Logical frame %llu   Camera %s", (unsigned long long)g_game.logicalFrame, g_cam.valid ? "OK" : "not set");
    }

    if (ImGui::CollapsingHeader("Reverse-engineering tools")) {
        ImGui::Checkbox("Struct Explorer", &g_ui.showExplorer); ImGui::SameLine();
        ImGui::Checkbox("Watches", &g_ui.showWatches);
        static char sig[256] = "";
        static char res[128] = "";
        ImGui::SetNextItemWidth(280);
        ImGui::InputText("##sig", sig, sizeof(sig));
        ImGui::SameLine();
        if (ImGui::Button("Find pattern")) {
            uintptr_t a = mem::FindPattern(sig);
            if (a) snprintf(res, sizeof(res), "match at rva:%llX", (unsigned long long)(a - mem::ModuleBase()));
            else snprintf(res, sizeof(res), "no match");
        }
        ImGui::TextUnformatted(res);
        ImGui::Text("Module base %llX  size %llX", (unsigned long long)mem::ModuleBase(), (unsigned long long)mem::ModuleSize());
    }

    if (ImGui::CollapsingHeader("Config")) {
        ImGui::TextWrapped("%s", g_cfg.path.c_str());
        ImGui::Text("%d shape sources configured", (int)g_cfg.boxes.size());
        for (auto& e : g_cfg.errors) ImGui::TextColored(ImVec4(1, .5f, .4f, 1), "%s", e.c_str());
        if (ImGui::Button("Reload MKXFrameLab.ini")) {
            std::string p = g_cfg.path;
            g_cfg.Load(p);
            g_game = GameState();
        }
    }
    ImGui::End();
}

// ------------------------------------------------------------------ FRAME LAB menu (F2)
// Laid out like the game's practice pause menu, mirrored on the right: white caps items on a thin arc of
// bullets, a pointer line from the screen edge to the selected one. Pad / keyboard belong to it while open.
namespace dib { void SetBlock(bool on); }
bool UiCapturesInput() { return g_ui.gameMenu; }

static bool g_gmFresh = true;
struct Repeat { bool prev = false; ULONGLONG next = 0; };
static bool Fire(bool down, Repeat& r) {
    ULONGLONG now = GetTickCount64();
    bool f = false;
    if (down && !r.prev) { f = true; r.next = now + 320; }
    else if (down && now >= r.next) { f = true; r.next = now + 85; }
    r.prev = down;
    return f;
}

enum class GmKind { Toggle, Choice, Slider, Action };
enum GmId { GmFrameData, GmPanelStyle, GmPanelSize, GmHitboxes, GmHurtboxes, GmHitAlpha, GmHurtAlpha, GmLinger, GmFill, GmBuffer, GmBufWindow, GmScreenshot, GmAdvanced, GmReload, GmClose };
struct GmItem { GmId id; const char* label; GmKind kind; };
static const GmItem kGm[] = {
    {GmFrameData, "FRAME DATA", GmKind::Toggle},   {GmPanelStyle, "PANEL STYLE", GmKind::Choice}, {GmPanelSize, "PANEL SIZE", GmKind::Slider},
    {GmHitboxes, "HITBOXES", GmKind::Toggle},      {GmHurtboxes, "HURTBOXES", GmKind::Toggle},    {GmHitAlpha, "HITBOX OPACITY", GmKind::Slider},
    {GmHurtAlpha, "HURTBOX OPACITY", GmKind::Slider},
    {GmLinger, "HITBOX LINGER", GmKind::Slider},
    {GmFill, "FILLED SHAPES", GmKind::Toggle},     {GmBuffer, "INPUT BUFFER", GmKind::Toggle},    {GmBufWindow, "BUFFER WINDOW", GmKind::Slider},
    {GmScreenshot, "TAKE SCREENSHOT", GmKind::Action}, {GmAdvanced, "ADVANCED SETTINGS", GmKind::Toggle}, {GmReload, "RELOAD MOD", GmKind::Action}, {GmClose, "CLOSE", GmKind::Action},
};
static const int kGmN = (int)(sizeof(kGm) / sizeof(kGm[0]));

// sliders: range and step
struct GmRange { float lo, hi, step; };
static GmRange GmRangeOf(GmId id) {
    switch (id) {
    case GmPanelSize: return {0.7f, 1.6f, 0.05f};
    case GmHitAlpha: case GmHurtAlpha: return {0.1f, 4.f, 0.05f};   // above 100% = stronger than the default look
    case GmLinger: return {0.f, 30.f, 1.f};
    case GmBufWindow: return {1.f, 15.f, 1.f};
    default: return {0.f, 1.f, 1.f};
    }
}
static float GmGet(GmId id) {
    switch (id) {
    case GmPanelSize: return g_ui.hudScale;
    case GmHitAlpha: return g_ui.hitAlpha;
    case GmHurtAlpha: return g_ui.hurtAlpha;
    case GmLinger: return (float)g_ui.strikeLinger;
    case GmBufWindow: return (float)ibuf::g_window;
    default: return 0.f;
    }
}
static void GmSet(GmId id, float v) {
    GmRange r = GmRangeOf(id);
    v = r.lo + std::round((v - r.lo) / r.step) * r.step;
    v = v < r.lo ? r.lo : v > r.hi ? r.hi : v;
    switch (id) {
    case GmPanelSize: g_ui.hudScale = v; break;
    case GmHitAlpha: g_ui.hitAlpha = v; break;
    case GmHurtAlpha: g_ui.hurtAlpha = v; break;
    case GmLinger: g_ui.strikeLinger = (int)std::lround(v); break;
    case GmBufWindow: ibuf::g_window = (int)std::lround(v); break;
    default: break;
    }
}

static std::string GmValue(GmId id) {
    char b[32];
    switch (id) {
    case GmFrameData: return g_ui.showMeter ? "ON" : "OFF";
    case GmPanelStyle: return g_ui.classicMeter ? "CLASSIC" : "GAME";
    case GmPanelSize: snprintf(b, sizeof(b), "%.2f", g_ui.hudScale); return b;
    case GmHitAlpha: snprintf(b, sizeof(b), "%d%%", (int)std::lround(g_ui.hitAlpha * 100)); return b;
    case GmHurtAlpha: snprintf(b, sizeof(b), "%d%%", (int)std::lround(g_ui.hurtAlpha * 100)); return b;
    case GmHitboxes: return (g_ui.showBoxes && g_ui.showStrike) ? "ON" : "OFF";
    case GmHurtboxes: return (g_ui.showBoxes && g_ui.showHurtPts) ? "ON" : "OFF";
    case GmLinger: snprintf(b, sizeof(b), "%d F", g_ui.strikeLinger); return b;
    case GmFill: return g_ui.fill ? "ON" : "OFF";
    case GmBuffer: return ibuf::g_enabled ? "ON" : "OFF";
    case GmBufWindow: snprintf(b, sizeof(b), "%d F", ibuf::g_window); return b;
    case GmAdvanced: return g_ui.menuOpen ? "OPEN" : "";
    default: return "";
    }
}
// dir: -1 / +1 = left / right, 0 = select (A / Enter / click)
static void GmChange(int i, int dir) {
    GmId id = kGm[i].id;
    if (kGm[i].kind == GmKind::Slider) {
        GmRange r = GmRangeOf(id);
        float v = GmGet(id) + (dir < 0 ? -r.step : r.step);
        if (dir == 0 && v > r.hi + r.step * 0.01f) v = r.lo;   // A on a slider steps up and wraps
        GmSet(id, v);
        return;
    }
    switch (id) {
    case GmFrameData: g_ui.showMeter = !g_ui.showMeter; break;
    case GmPanelStyle: g_ui.classicMeter = !g_ui.classicMeter; break;
    case GmHitboxes: {
        bool on = !(g_ui.showBoxes && g_ui.showStrike);
        g_ui.showStrike = g_ui.showShapes = on;
        if (on) g_ui.showBoxes = true;
        break;
    }
    case GmHurtboxes: {
        bool on = !(g_ui.showBoxes && g_ui.showHurtPts);
        g_ui.showHurtPts = on;
        if (on) g_ui.showBoxes = true;
        break;
    }
    case GmFill: g_ui.fill = !g_ui.fill; break;
    case GmBuffer: ibuf::g_enabled = !ibuf::g_enabled; break;
    case GmAdvanced: g_ui.menuOpen = !g_ui.menuOpen; break;
    case GmReload: if (dir == 0) g_reloadRequested = true; break;
    case GmScreenshot: if (dir == 0) shot::Request(); break;
    case GmClose: if (dir == 0) g_ui.gameMenu = false; break;
    default: break;
    }
}

// one line per menu item for the reference card
static const char* GmDesc(GmId id) {
    switch (id) {
    case GmFrameData: return "The frame data panel: start-up, active and recovery of your last move, advantage on hit or block, the game's own move list entry and gaps in strings.";
    case GmPanelStyle: return "GAME draws the panel like the move list's frame data; CLASSIC is a compact strip with just the frame meter.";
    case GmPanelSize: return "Scales the frame data panel.";
    case GmHitboxes: return "Attack areas: strike spheres and projectiles, drawn exactly where the game's hit test checks them on the frames they are live.";
    case GmHurtboxes: return "The body spheres each fighter can be hit on.";
    case GmHitAlpha: return "How strong hitboxes are drawn. The mark is the default (100%).";
    case GmHurtAlpha: return "How strong hurtboxes are drawn. The mark is the default (100%).";
    case GmLinger: return "Keeps a hitbox on screen for a few frames after it was tested, so 1-frame actives are easy to see.";
    case GmFill: return "Filled shapes, or outlines only.";
    case GmBuffer: return "Queues a normal or special pressed shortly before you recover, so it comes out on the first possible frame.";
    case GmBufWindow: return "How early (in frames) a press may come and still be buffered.";
    case GmScreenshot: return "Saves a screenshot with the overlay to Pictures\\MKX Frame Lab (F8 works anywhere).";
    case GmAdvanced: return "Opens the full settings window (colours, labels, debug tools).";
    case GmReload: return "Reloads the mod's settings and code without restarting the game.";
    case GmClose: return "Closes this menu (F2 opens it again).";
    default: return "";
    }
}

static void DrawGameMenu(float sw, float sh) {
    // ---- input (pad 1 as the game would have seen it, plus keyboard when the game has focus)
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    bool focused = pid == GetCurrentProcessId();
    uint16_t pb = 0; int16_t lx = 0, ly = 0;
    ibuf::RealPad(pb, lx, ly);
    auto key = [&](int vk) { return focused && (GetAsyncKeyState(vk) & 0x8000) != 0; };
    bool up = (pb & 0x0001) || ly > 16000 || key(VK_UP), dn = (pb & 0x0002) || ly < -16000 || key(VK_DOWN);
    bool lf = (pb & 0x0004) || lx < -16000 || key(VK_LEFT), rt = (pb & 0x0008) || lx > 16000 || key(VK_RIGHT);
    bool ok = (pb & 0x1000) || key(VK_RETURN) || key(VK_SPACE), back = (pb & 0x2000) || key(VK_BACK) || key(VK_ESCAPE);
    static Repeat rUp, rDn, rLf, rRt, rOk, rBack;
    bool& fresh = g_gmFresh;    // ignore buttons already held when the menu opened
    if (fresh) { rUp.prev = up; rDn.prev = dn; rLf.prev = lf; rRt.prev = rt; rOk.prev = ok; rBack.prev = back; fresh = false; }
    int& sel = g_ui.gmSel;
    if (Fire(up, rUp)) sel = (sel + kGmN - 1) % kGmN;
    if (Fire(dn, rDn)) sel = (sel + 1) % kGmN;
    bool okF = Fire(ok, rOk), lfF = Fire(lf, rLf), rtF = Fire(rt, rRt), backF = rBack.prev != back && back;
    rBack.prev = back;
    if (lfF && kGm[sel].kind != GmKind::Action) GmChange(sel, -1);
    if (rtF && kGm[sel].kind != GmKind::Action) GmChange(sel, +1);
    if (okF) GmChange(sel, 0);
    if (backF) g_ui.gameMenu = false;
    if (!g_ui.gameMenu) return;

    // ---- layout: Mortal Kombat X's pause menu / move list (1440p reference), on the right half of the screen:
    // white title over a gold rule, a header plate, rows split by faint rules, the selected row on the grey
    // cursor bar with a gold diamond; a MOVE DATA style reference card beside it
    float k = sh / 1440.f;
    ImFont* mf = g_menuFont ? g_menuFont : ImGui::GetFont();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float x1 = sw - 70 * k, x0 = x1 - 860 * k, cx = (x0 + x1) * 0.5f;
    // below the game's top HUD (health bars / names end around y = 160 at 1440p), above its bottom bars
    const float rowH = 56 * k, fs = 30 * k, vfs = 27 * k;
    const float titleY = 214 * k, ruleY = 292 * k, plateY0 = 318 * k, plateY1 = 366 * k, itemsY0 = 384 * k;
    const float itemsY1 = itemsY0 + kGmN * rowH;

    // the game darkens the screen behind its pause menu: same here, fading in from the middle
    dl->AddRectFilledMultiColor(ImVec2(sw * 0.28f, 0), ImVec2(x0 - 60 * k, sh), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 200), IM_COL32(0, 0, 0, 200), IM_COL32(0, 0, 0, 0));
    dl->AddRectFilled(ImVec2(x0 - 60 * k, 0), ImVec2(sw, sh), IM_COL32(0, 0, 0, 200));
    dl->AddRectFilled(ImVec2(x0 - 30 * k, ruleY - 20 * k), ImVec2(x1 + 30 * k, itemsY0 + kGmN * rowH + 80 * k), IM_COL32(6, 6, 5, 120));   // the menu column itself

    // title + subtitle + rule
    XCentered(dl, mf, 58 * k, ImVec2(cx, titleY), xc::White, "FRAME LAB");
    {
        const char* sub = "PRACTICE TOOLS   v" MKXFL_VERSION;
        float w = SpacedWidth(mf, 19 * k, sub, 4.f * k);
        SpacedText(dl, mf, 19 * k, ImVec2(cx - w * 0.5f, titleY + 36 * k), xc::Tan, sub, 4.f * k);
    }
    XRule(dl, x0 - 30 * k, x1 + 30 * k, ruleY, k);
    // header plate (like the "KANO (CYBERNETIC)" plate over the move highlights)
    XBox(dl, ImVec2(x0, plateY0), ImVec2(x1, plateY1), k, IM_COL32(40, 39, 36, 245));
    XCentered(dl, mf, 28 * k, ImVec2(cx, (plateY0 + plateY1) * 0.5f), xc::White, "MORTAL KOMBAT X  -  PRACTICE");

    // mouse: an invisible window over the rows so clicks don't reach the game
    ImGui::SetNextWindowPos(ImVec2(x0, itemsY0));
    ImGui::SetNextWindowSize(ImVec2(x1 - x0, rowH * kGmN));
    ImGui::Begin("##framelabmenu", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus);
    for (int i = 0; i < kGmN; ++i) {
        float y0 = itemsY0 + i * rowH, y1 = y0 + rowH, cy = (y0 + y1) * 0.5f;
        bool s = i == sel;
        if (s) XSelBar(dl, ImVec2(x0 - 24 * k, y0 + 3 * k), ImVec2(x1 + 24 * k, y1 - 3 * k), k);
        else if (i + 1 < kGmN && i + 1 != sel) dl->AddLine(ImVec2(x0 + 20 * k, y1), ImVec2(x1 - 20 * k, y1), IM_COL32(255, 255, 255, 34), 1.2f * k);
        const char* lab = kGm[i].label;
        ImVec2 lsz = mf->CalcTextSizeA(fs, FLT_MAX, 0, lab);
        float tx = x0 + 46 * k;
        if (s) XDiamond(dl, ImVec2(x0 + 20 * k, cy), 7 * k, xc::Gold);
        XText(dl, mf, fs, ImVec2(tx, cy - lsz.y * 0.5f), s ? xc::White : xc::Text, lab);

        // value, right-aligned in the row
        std::string v = GmValue(kGm[i].id);
        bool slider = kGm[i].kind == GmKind::Slider;
        float right = x1 - 40 * k;
        ImVec2 barA(0, 0), barB(0, 0);
        float vxLeft = right;
        if (!v.empty()) {
            ImU32 vc = s ? xc::White : (v == "OFF" ? xc::Dim : xc::Tan);
            float vw = slider ? mf->CalcTextSizeA(vfs, FLT_MAX, 0, "100%").x : mf->CalcTextSizeA(vfs, FLT_MAX, 0, "CLASSIC").x;
            ImVec2 vs = mf->CalcTextSizeA(vfs, FLT_MAX, 0, v.c_str());
            float vx = right - vw;
            XText(dl, mf, vfs, ImVec2(vx + (vw - vs.x) * 0.5f, cy - vs.y * 0.5f), vc, v.c_str());
            vxLeft = vx;
            if (slider) {
                GmRange r = GmRangeOf(kGm[i].id);
                float frac = (GmGet(kGm[i].id) - r.lo) / (r.hi - r.lo);
                float bw = 220 * k, bh = 6 * k;
                barB = ImVec2(vx - 36 * k, cy + bh * 0.5f);
                barA = ImVec2(barB.x - bw, cy - bh * 0.5f);
                dl->AddRectFilled(barA, barB, IM_COL32(18, 17, 16, 230));
                dl->AddRectFilled(barA, ImVec2(barA.x + bw * frac, barB.y), s ? IM_COL32(226, 204, 150, 255) : IM_COL32(170, 150, 112, 230));
                dl->AddRect(ImVec2(barA.x - 1.5f * k, barA.y - 1.5f * k), ImVec2(barB.x + 1.5f * k, barB.y + 1.5f * k), xc::Bronze, 0, 0, 1.f * k);
                if (kGm[i].id == GmHitAlpha || kGm[i].id == GmHurtAlpha) {   // notch at the default (100%)
                    float x = barA.x + bw * (1.f - r.lo) / (r.hi - r.lo);
                    dl->AddLine(ImVec2(x, barA.y - 6 * k), ImVec2(x, barB.y + 6 * k), s ? xc::White : xc::Tan, 1.5f * k);
                }
                ImVec2 kc(barA.x + bw * frac, cy);
                XDiamond(dl, kc, (s ? 10.f : 7.f) * k, s ? xc::White : xc::Tan);
                vxLeft = barA.x;
            }
            if (s && kGm[i].kind != GmKind::Action) {   // arrows around the value
                float ah = 8 * k, ax = vxLeft - 26 * k, bx = right + 14 * k;
                dl->AddTriangleFilled(ImVec2(ax, cy), ImVec2(ax + ah, cy - ah), ImVec2(ax + ah, cy + ah), xc::White);
                dl->AddTriangleFilled(ImVec2(bx + ah, cy), ImVec2(bx, cy - ah), ImVec2(bx, cy + ah), xc::White);
            }
        }
        ImGui::PushID(i);
        if (slider) {
            // the bar can be clicked / dragged; the rest of the row behaves like any item
            float pad = 12 * k;
            ImGui::SetCursorScreenPos(ImVec2(barA.x - pad, y0));
            ImGui::InvisibleButton("bar", ImVec2(barB.x - barA.x + pad * 2, rowH));
            if (ImGui::IsItemActive()) {
                sel = i;
                GmRange r = GmRangeOf(kGm[i].id);
                float f = (ImGui::GetIO().MousePos.x - barA.x) / (barB.x - barA.x);
                f = f < 0 ? 0 : f > 1 ? 1 : f;
                GmSet(kGm[i].id, r.lo + f * (r.hi - r.lo));
            }
            if (ImGui::IsItemHovered() && (ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0)) sel = i;
            ImGui::SetCursorScreenPos(ImVec2(x0, y0));
            if (ImGui::InvisibleButton("it", ImVec2(barA.x - pad - x0, rowH))) { sel = i; GmChange(i, 0); }
            if (ImGui::IsItemHovered() && (ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0)) sel = i;
            ImGui::SetCursorScreenPos(ImVec2(barB.x + pad, y0));
            if (ImGui::InvisibleButton("it2", ImVec2(x1 - (barB.x + pad), rowH))) { sel = i; GmChange(i, +1); }
        } else {
            ImGui::SetCursorScreenPos(ImVec2(x0, y0));
            if (ImGui::InvisibleButton("it", ImVec2(x1 - x0, rowH))) { sel = i; GmChange(i, 0); }
        }
        if (ImGui::IsItemHovered() && (ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0)) sel = i;
        ImGui::PopID();
    }
    ImGui::End();

    // ---- reference card beside the menu (the move list's MOVE DATA block): colour key + what the selected item does
    {
        float c1 = x0 - 70 * k, c0 = c1 - 600 * k, ccx = (c0 + c1) * 0.5f;
        float y = plateY0;
        XTitle(dl, mf, 21 * k, ImVec2(ccx, y + 14 * k), IM_COL32(232, 230, 224, 255), "QUICK REFERENCE", 5.f * k);
        y += 40 * k;
        struct Leg { ImU32 col; const char* name; const char* what; } legs[] = {
            {g_cfg.colHit, "HITBOX", "attack, as the game tests it"},
            {0xFF9020D0u, "PROJECTILE", "projectile / thrown shape"},
            {g_cfg.colHurt, "HURTBOX", "where the fighter can be hit"},
        };
        float legH = 3 * 46 * k + 26 * k;
        XBox(dl, ImVec2(c0, y), ImVec2(c1, y + legH), k);
        float ly = y + 13 * k;
        for (auto& l : legs) {
            float cyy = ly + 23 * k;
            ImVec2 c(c0 + 66 * k, cyy);
            auto mk = [](unsigned rrggbbaa, float a) {
                unsigned al = (unsigned)((rrggbbaa & 0xFF) * a);
                return IM_COL32((rrggbbaa >> 24) & 0xFF, (rrggbbaa >> 16) & 0xFF, (rrggbbaa >> 8) & 0xFF, al > 255 ? 255 : al);
            };
            dl->AddCircleFilled(c, 12 * k, mk(l.col, 0.6f), 24);
            dl->AddCircle(c, 12 * k, mk(l.col | 0xFF, 1.f), 24, 2.f * k);
            ImVec2 ns = mf->CalcTextSizeA(24 * k, FLT_MAX, 0, l.name);
            XText(dl, mf, 24 * k, ImVec2(c0 + 92 * k, cyy - ns.y * 0.5f), xc::White, l.name);
            ImVec2 ws = mf->CalcTextSizeA(20 * k, FLT_MAX, 0, l.what);
            XText(dl, mf, 20 * k, ImVec2(c1 - 52 * k - ws.x, cyy - ws.y * 0.5f), xc::Tan, l.what);
            ly += 46 * k;
        }
        y += legH + 30 * k;
        // frame meter colours
        XTitle(dl, mf, 21 * k, ImVec2(ccx, y + 14 * k), IM_COL32(232, 230, 224, 255), "FRAME METER", 5.f * k);
        y += 40 * k;
        {
            struct St { FState s; const char* n; } sts[] = {{FState::Startup, "START-UP"}, {FState::Active, "ACTIVE"}, {FState::Recovery, "RECOVERY"},
                                                             {FState::Hitstun, "HITSTUN"}, {FState::Blockstun, "BLOCKSTUN"}, {FState::Idle, "FREE"}};
            float bh = 2 * 40 * k + 24 * k;
            XBox(dl, ImVec2(c0, y), ImVec2(c1, y + bh), k);
            float colw = (c1 - c0 - 100 * k) / 3.f;
            for (int i = 0; i < 6; ++i) {
                float xx = c0 + 50 * k + (i % 3) * colw, yy = y + 12 * k + (i / 3) * 40 * k + 20 * k;
                dl->AddRectFilled(ImVec2(xx, yy - 8 * k), ImVec2(xx + 12 * k, yy + 8 * k), StateColor(sts[i].s));
                ImVec2 ts = mf->CalcTextSizeA(20 * k, FLT_MAX, 0, sts[i].n);
                XText(dl, mf, 20 * k, ImVec2(xx + 22 * k, yy - ts.y * 0.5f), xc::Text, sts[i].n);
            }
            y += bh + 30 * k;
        }
        // the selected item
        const char* desc = GmDesc(kGm[sel].id);
        float wrapW = c1 - c0 - 120 * k;
        ImVec2 ds = mf->CalcTextSizeA(23 * k, FLT_MAX, wrapW, desc);
        XTitle(dl, mf, 21 * k, ImVec2(ccx, y + 14 * k), IM_COL32(232, 230, 224, 255), kGm[sel].label, 5.f * k);
        y += 40 * k;
        float dh = ds.y + 40 * k;
        if (dh < 70 * k) dh = 70 * k;
        XBox(dl, ImVec2(c0, y), ImVec2(c1, y + dh), k);
        XText(dl, mf, 23 * k, ImVec2(c0 + 60 * k, y + (dh - ds.y) * 0.5f), xc::Text, desc, wrapW);
    }

    // ---- footer: the game's status line (bottom left) and button prompts under the menu
    {
        float y = itemsY1 + 34 * k, x = x0 + 10 * k;
        struct Hint { char btn; const char* key; const char* text; } hints[] = {{'A', "ENTER", "SELECT"}, {'B', "ESC", "BACK"}, {0, "F2", "CLOSE"}, {0, "F8", "SCREENSHOT"}};
        for (auto& h : hints) {
            if (h.btn) { XPadButton(dl, mf, k, ImVec2(x + 16 * k, y + 16 * k), h.btn); x += 42 * k; }
            else {
                ImVec2 ks = mf->CalcTextSizeA(20 * k, FLT_MAX, 0, h.key);
                float w = ks.x + 18 * k;
                dl->AddRectFilled(ImVec2(x, y + 1 * k), ImVec2(x + w, y + 31 * k), IM_COL32(230, 228, 222, 235), 3 * k);
                XText(dl, mf, 20 * k, ImVec2(x + 9 * k, y + 16 * k - ks.y * 0.5f), IM_COL32(30, 29, 27, 255), h.key);
                x += w + 10 * k;
            }
            ImVec2 ts = mf->CalcTextSizeA(25 * k, FLT_MAX, 0, h.text);
            XText(dl, mf, 25 * k, ImVec2(x, y + 16 * k - ts.y * 0.5f), xc::White, h.text);
            x += ts.x + 34 * k;
        }
        // d-pad: change
        float cy = y + 16 * k, ah = 8 * k;
        dl->AddTriangleFilled(ImVec2(x, cy), ImVec2(x + ah, cy - ah), ImVec2(x + ah, cy + ah), xc::White);
        dl->AddTriangleFilled(ImVec2(x + 30 * k, cy), ImVec2(x + 22 * k, cy - ah), ImVec2(x + 22 * k, cy + ah), xc::White);
        ImVec2 ts = mf->CalcTextSizeA(25 * k, FLT_MAX, 0, "CHANGE");
        XText(dl, mf, 25 * k, ImVec2(x + 42 * k, cy - ts.y * 0.5f), xc::White, "CHANGE");
    }
}

void UiDraw(float sw, float sh) {
    trace::Poll();
    if (mkx::Ready()) mkx::PollStep();
    mem::EvalCtx ctx;
    if (!g_cfg.demo) {
        if (g_cfg.p1) ctx.p1 = g_cfg.p1->Eval(ctx);
        if (g_cfg.p2) ctx.p2 = g_cfg.p2->Eval(ctx);
    }
    mkx::Snapshot snap;
    bool haveGame = mkx::Ready();
    if (haveGame) snap = mkx::GetSnapshot();
    if (!(haveGame && CameraFromGame(g_cam, snap, sw, sh))) CameraUpdate(g_cam, ctx, sw, sh);

    bool hideForPause = haveGame && mkx::GamePaused();   // pause menu open: keep the menu readable
    // in a fight = both fighters exist and the match clock is running (or deliberately stopped: pause menu /
    // freeze). Main menu, character select, loading and intros show nothing.
    bool inFight = false;
    {
        static uint64_t lastFrame = 0;
        static ULONGLONG lastAdvance = 0;
        ULONGLONG now = GetTickCount64();
        // practice mode only: anywhere else (versus, online, story, towers ...) the overlay stays off
        bool practice = haveGame && snap.gameMode == "practice";
        {
            static std::string lastMode = "?";
            if (haveGame && snap.gameMode != lastMode) { lastMode = snap.gameMode; Log("ui: game mode '%s'", lastMode.c_str()); }
        }
        ibuf::SetAllowed(practice);
        g_game.locked = haveGame && !practice && !g_cfg.demo;
        if (g_game.locked) g_game.lockReason = "Overlay off: practice mode only (" + (snap.gameMode.empty() ? std::string("menus") : snap.gameMode) + ")";
        bool both = practice && snap.p[0].valid && snap.p[1].valid;
        if (both && snap.frame != lastFrame) { lastFrame = snap.frame; lastAdvance = now; }
        if (!both) lastAdvance = 0;
        inFight = both && (hideForPause || mkx::Frozen() || (lastAdvance && now - lastAdvance < 1500));
        static int logged = -1;
        if ((int)inFight != logged) {
            logged = inFight;
            Log("ui: in fight %d (P1 %d P2 %d, paused %d, frame %llu)", (int)inFight, haveGame && snap.p[0].valid, haveGame && snap.p[1].valid,
                (int)hideForPause, (unsigned long long)snap.frame);
        }
    }
    {
        // diagnostics: log pause-flag candidates and camera changes
        static int lastP[3] = {-1, -1, -1};
        static uintptr_t lastCam = 0;
        uintptr_t gi = haveGame ? mkx::GameInfo() : 0;
        if (gi) {
            int v[3] = {mem::ReadOr<int>(gi + 0x10, 0), mem::ReadOr<int>(gi + 0x14, 0), mem::ReadOr<int>(gi + 0x148, 0)};
            if (v[0] != lastP[0] || v[1] != lastP[1] || v[2] != lastP[2]) {
                Log("pauseflags GI+10 %d GI+14 %X GI+148 %d (P1 %X P2 %X)", v[0], v[1], v[2], snap.p[0].stateFlags, snap.p[1].stateFlags);
                for (int i = 0; i < 3; ++i) lastP[i] = v[i];
            }
        }
        if (haveGame && snap.camPtr != lastCam) { Log("camera object %llX -> %llX", (unsigned long long)lastCam, (unsigned long long)snap.camPtr); lastCam = snap.camPtr; }
    }
    if (!g_game.locked && inFight && hideForPause && g_ui.gameMenu && g_ui.showMeter) {
        if (g_ui.classicMeter) DrawFrameMeter(sw, sh); else DrawFrameMeterGame(sw, sh);
    }
    if (!g_game.locked && !hideForPause && (inFight || g_cfg.demo)) {
        ImDrawList* bg = ImGui::GetBackgroundDrawList();
        if (g_ui.showBoxes)
            BoxesDraw(bg, g_cam, ctx, g_ui.showHurt, g_ui.showHit, g_ui.showThrow, g_ui.showPush, g_ui.showOther, g_ui.fill);
        if (g_ui.showMarkers) DrawPlayerMarkers(bg, g_cam);
        if (haveGame && g_ui.showBones) DrawBones(bg, g_cam, snap, g_ui.boneRadius, g_ui.boneLabels);
        const int hurtV0 = bg->VtxBuffer.Size;   // vertex ranges: hurtboxes, then hitboxes (strike + shaped / projectiles)
        if (haveGame && g_ui.showHurtPts && g_ui.showBoxes) DrawHurt(bg, g_cam, snap, g_ui.zeroRadius, g_ui.fill, g_ui.hurtLabels);
        const int hitV0 = bg->VtxBuffer.Size;
        if (haveGame && g_ui.showStrike && g_ui.showBoxes) DrawStrike(bg, g_cam, snap, g_ui.strikeLinger, g_ui.fill);
        if (haveGame && g_ui.showShapes && g_ui.showBoxes) DrawShapes(bg, g_cam, snap, g_ui.shapesInactive, g_ui.fill);
        const int hitV1 = bg->VtxBuffer.Size;
        auto scaleAlpha = [&](int v0, int v1, float a) {
            if (a > 0.999f && a < 1.001f) return;
            if (a < 0.05f) a = 0.05f;
            for (int i = v0; i < v1; ++i) {
                ImU32& c = bg->VtxBuffer[i].col;
                float fa = ((c >> IM_COL32_A_SHIFT) & 0xFF) * a + 0.5f;
                ImU32 al = (ImU32)(fa > 255.f ? 255.f : fa);   // above 100%: fills get denser; outlines are already opaque
                c = (c & ~IM_COL32_A_MASK) | (al << IM_COL32_A_SHIFT);
            }
        };
        scaleAlpha(hurtV0, hitV0, g_ui.hurtAlpha);
        scaleAlpha(hitV0, hitV1, g_ui.hitAlpha);
        if (g_cfg.demo && g_ui.showBoxes) {
            // demo shapes so rendering can be verified with no offsets
            float cx = sw * 0.5f, cy = sh * 0.5f;
            bool active = g_game.p[0].fstate == FState::Active;
            bg->AddCircle(ImVec2(cx - 140, cy), 60, IM_COL32(60, 140, 255, 255), 32, 2.f);
            bg->AddCircleFilled(ImVec2(cx - 140, cy), 60, IM_COL32(60, 140, 255, 70), 32);
            bg->AddCircle(ImVec2(cx + 140, cy), 60, IM_COL32(60, 140, 255, 255), 32, 2.f);
            bg->AddCircleFilled(ImVec2(cx + 140, cy), 60, IM_COL32(60, 140, 255, 70), 32);
            if (active) {
                bg->AddCircleFilled(ImVec2(cx - 30, cy - 10), 30, IM_COL32(255, 48, 48, 110), 24);
                bg->AddCircle(ImVec2(cx - 30, cy - 10), 30, IM_COL32(255, 48, 48, 255), 24, 2.f);
            }
        }
        if (g_ui.showMeter) { if (g_ui.classicMeter) DrawFrameMeter(sw, sh); else DrawFrameMeterGame(sw, sh); }
    }
    {
        // which menus are open survives a hot reload (kept in the process environment)
        static bool restored = false;
        static int saved = -1;
        if (!restored) {
            restored = true;
            char b[8] = "";
            if (GetEnvironmentVariableA("MKXFL_MENUS", b, sizeof(b)) && b[0]) {
                int v = atoi(b);
                g_ui.menuOpen = (v & 1) != 0;
                g_ui.gameMenu = (v & 2) != 0;
                saved = v;
            }
        }
        int cur = (g_ui.menuOpen ? 1 : 0) | (g_ui.gameMenu ? 2 : 0);
        if (cur != saved) { saved = cur; char b[8]; snprintf(b, sizeof(b), "%d", cur); SetEnvironmentVariableA("MKXFL_MENUS", b); }
    }
    {
        static bool wasOpen = false;
        if (g_ui.gameMenu && !wasOpen) g_gmFresh = true;
        wasOpen = g_ui.gameMenu;
    }
    ibuf::SetCapture(g_ui.gameMenu);
    dib::SetBlock(g_ui.gameMenu);
    bool hideMenus = shot::HidingMenus();   // a screenshot is being taken: only the overlay itself
    if (g_ui.gameMenu && !hideMenus) DrawGameMenu(sw, sh);
    if (g_ui.menuOpen && !hideMenus) DrawMenu();
    {
        // once per game session: tell new users where the menu is
        static ULONGLONG hintUntil = 0;
        static bool hinted = false;
        if (!hinted && inFight) {
            hinted = true;
            char b[4] = "";
            if (!GetEnvironmentVariableA("MKXFL_HINTED", b, sizeof(b))) { hintUntil = GetTickCount64() + 7000; SetEnvironmentVariableA("MKXFL_HINTED", "1"); }
        }
        if (!hideMenus && !g_ui.gameMenu && GetTickCount64() < hintUntil && shot::Status().empty()) {
            ImFont* f = g_menuFont ? g_menuFont : ImGui::GetFont();
            float k = sh / 1440.f;
            XToast(ImGui::GetForegroundDrawList(), f, k, ImVec2(sw * 0.5f, 170 * k), "MKX FRAME LAB  -  PRESS F2 FOR THE MENU");
        }
    }
    if (!hideMenus) {
        std::string st = shot::Status();
        if (!st.empty()) {
            ImFont* f = g_menuFont ? g_menuFont : ImGui::GetFont();
            float k = sh / 1440.f;
            float cx = g_ui.gameMenu ? (sw - 1000 * k) * 0.5f : sw * 0.5f;   // beside the menu when it is open
            XToast(ImGui::GetForegroundDrawList(), f, k, ImVec2(cx, 170 * k), st.c_str());
        }
    }
    if (g_ui.showExplorer && g_ui.menuOpen && !hideMenus) DrawExplorer();
    if (g_ui.showWatches && g_ui.menuOpen && !hideMenus) DrawWatches();
    if (g_ui.showCollList && g_ui.menuOpen && !hideMenus) DrawCollList();
}
