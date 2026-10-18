#include "bot.h"
#include "movelist.h"
#include "mkx.h"
#include "screenshot.h"
#include "log.h"
#include "ui.h"
#include <windows.h>
#include <xinput.h>
#include <atomic>
#include <mutex>
#include <deque>
#include <vector>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <cmath>
#include <cstring>

/*  Command file (one command per line, '#' comments):
      pad <frames> <input>   hold an input for N simulation frames.  <input> = numpad direction relative to
                             P1's facing (1-9, 5 = neutral, 6 = forward, 4 = back, 2 = down) optionally joined
                             with buttons by '+': A B X Y LB RB LT RT BACK START.  e.g.  pad 2 2   pad 3 3+X   pad 2 X
      wait <frames>          neutral for N frames
      waitfree [max]         neutral until both fighters are free (state 0) for 10 frames (default max 600)
      mark <text>            write a marker line into MKXFrameLab.log
      reload                 hot-reload the mod
      stop                   abort the running script
    A new file replaces the running script. The file is deleted once read.                                   */

namespace bot {

struct Step { enum K { Pad, Wait, WaitFree, Mark, Reload, Dump, RPad, RWait, Rec, Find, Geo, Peek, Names, PtrScan } k; int frames = 0; int dir = 5; uint16_t buttons = 0; std::string text; };

static std::mutex g_mtx;
static std::deque<Step> g_steps;
static std::atomic<bool> g_active{false};
static std::atomic<uint16_t> g_out{0};
static int g_left = 0;          // frames left in the current step
static int g_freeRun = 0;
static bool g_stepStarted = false;
static std::string g_status = "idle";
static bool g_facingRight = true;
bool FacingRight() { return g_facingRight; }

static uint16_t ButtonBit(const std::string& t) {
    if (t == "A") return XINPUT_GAMEPAD_A;
    if (t == "B") return XINPUT_GAMEPAD_B;
    if (t == "X") return XINPUT_GAMEPAD_X;
    if (t == "Y") return XINPUT_GAMEPAD_Y;
    if (t == "LB") return XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (t == "RB") return XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if (t == "BACK") return XINPUT_GAMEPAD_BACK;
    if (t == "START") return XINPUT_GAMEPAD_START;
    if (t == "LT") return 0x0400;   // triggers are analog - flagged in unused bits, applied in Override
    if (t == "RT") return 0x0800;
    return 0;
}

static bool ParseInput(const std::string& s, int& dir, uint16_t& btn) {
    dir = 5; btn = 0;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, '+')) {
        if (tok.size() == 1 && tok[0] >= '1' && tok[0] <= '9') { dir = tok[0] - '0'; continue; }
        uint16_t b = ButtonBit(tok);
        if (!b) return false;
        btn |= b;
    }
    return true;
}

static std::string g_dir;
// RE helper: write the game's writable module memory + GameInfo to MKXFrameLab_dump_<label>.bin
// format: repeated { uint64 address, uint64 size, bytes }
static void DumpMemory(const std::string& label) {
    std::string path = g_dir + "MKXFrameLab_dump_" + label + ".bin";
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    auto put = [&](uintptr_t a, size_t n) {
        std::vector<uint8_t> b(n);
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), (void*)a, b.data(), n, &got) || got != n) return;
        uint64_t h[2] = {a, n};
        fwrite(h, 8, 2, f);
        fwrite(b.data(), 1, n, f);
    };
    uintptr_t base = mkx::Base();
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t a = base;
    size_t total = 0;
    while (VirtualQuery((void*)a, &mbi, sizeof(mbi)) && (uintptr_t)mbi.AllocationBase == base) {
        if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY))) { put(a, mbi.RegionSize); total += mbi.RegionSize; }
        a += mbi.RegionSize;
    }
    uintptr_t gi = mkx::GameInfo();
    if (gi) put(gi, 0x4000);
    fclose(f);
    Log("bot: dump '%s' written (%zu KB module data, GameInfo %llX)", label.c_str(), total / 1024, (unsigned long long)gi);
}

// RE helper: search all readable private/image memory for a value sequence; log up to 200 hits
static void FindSeq(const std::string& spec) {
    std::stringstream ss(spec);
    std::string type;
    ss >> type;
    std::vector<uint8_t> pat;
    double v;
    while (ss >> v) {
        if (type == "f32") { float f = (float)v; uint8_t b[4]; memcpy(b, &f, 4); pat.insert(pat.end(), b, b + 4); }
        else if (type == "i16") { int16_t x = (int16_t)v; uint8_t b[2]; memcpy(b, &x, 2); pat.insert(pat.end(), b, b + 2); }
        else if (type == "i8") { pat.push_back((uint8_t)(int8_t)v); }
        else { int32_t x = (int32_t)v; uint8_t b[4]; memcpy(b, &x, 4); pat.insert(pat.end(), b, b + 4); }
    }
    if (pat.empty()) return;
    std::string out = g_dir + "MKXFrameLab_find.txt";
    FILE* f = fopen(out.c_str(), "a");
    if (f) fprintf(f, "== find %s\n", spec.c_str());
    int hits = 0;
    uintptr_t base = mkx::Base();
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t a = 0x10000;
    std::vector<uint8_t> buf;
    while (a < 0x7FFFFFFF0000ull && VirtualQuery((void*)a, &mbi, sizeof(mbi))) {
        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        bool ok = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                  (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_READ)) &&
                  mbi.RegionSize < (512u << 20);
        if (ok) {
            buf.resize(mbi.RegionSize);
            SIZE_T got = 0;
            if (ReadProcessMemory(GetCurrentProcess(), mbi.BaseAddress, buf.data(), mbi.RegionSize, &got) && got > pat.size()) {
                for (size_t i = 0; i + pat.size() <= got; i += (type == "i8" ? 1 : 2)) {
                    if (buf[i] == pat[0] && memcmp(&buf[i], pat.data(), pat.size()) == 0) {
                        uintptr_t at = (uintptr_t)mbi.BaseAddress + i;
                        if (f) {
                            fprintf(f, "%llX %s", (unsigned long long)at, (at >= base && at < base + 0x10000000) ? "module" : "heap");
                            // context: 64 bytes before, 64 after as int32
                            int32_t ctx[48]; size_t s0 = i >= 64 ? i - 64 : 0;
                            if (s0 + sizeof(ctx) <= got) { memcpy(ctx, &buf[s0], sizeof(ctx)); fprintf(f, "  ctx:"); for (int k = 0; k < 48; ++k) fprintf(f, " %d", ctx[k]); }
                            fprintf(f, "\n");
                        }
                        if (++hits >= 200) break;
                    }
                }
            }
        }
        if (hits >= 200) break;
        a = next;
    }
    if (f) { fprintf(f, "== %d hits\n", hits); fclose(f); }
    Log("bot: find '%s' -> %d hits", spec.c_str(), hits);
}

void PollCommandFile(const std::string& dir) {
    g_dir = dir;
    static ULONGLONG next = 0;
    ULONGLONG now = GetTickCount64();
    if (now < next) return;
    next = now + 250;
    std::string path = dir + "MKXFrameLab_cmd.txt";
    std::ifstream f(path);
    if (!f) return;
    std::deque<Step> steps;
    std::string line;
    int ln = 0;
    while (std::getline(f, line)) {
        ++ln;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t h = line.find('#');
        std::string body = h == std::string::npos ? line : line.substr(0, h);
        std::stringstream ss(body);
        std::string cmd;
        if (!(ss >> cmd)) continue;
        Step st{};
        if (cmd == "pad") {
            std::string in;
            ss >> st.frames >> in;
            st.k = Step::Pad;
            if (st.frames <= 0 || !ParseInput(in, st.dir, st.buttons)) { Log("bot: line %d bad pad command '%s'", ln, line.c_str()); continue; }
        } else if (cmd == "wait") { st.k = Step::Wait; ss >> st.frames; }
        else if (cmd == "waitfree") { st.k = Step::WaitFree; st.frames = 600; ss >> st.frames; }
        else if (cmd == "mark") { st.k = Step::Mark; std::getline(ss, st.text); }
        else if (cmd == "freeze") { st.k = Step::Mark; st.text = "@freeze"; }     // sim-frame step: freeze the fight here
        else if (cmd == "unfreeze") { st.k = Step::Mark; st.text = "@unfreeze"; }
        else if (cmd == "reload") { st.k = Step::Reload; }
        else if (cmd == "traceshape") { mkx::ArmShapeTrace(); continue; }
        else if (cmd == "screenshot") { shot::Request(); continue; }
        else if (cmd == "ui") {   // ui [on]: log the overlay switches; "on" turns frame data / hitboxes / hurtboxes on
            std::string a; ss >> a;
            if (a == "on") { g_ui.showBoxes = g_ui.showMeter = g_ui.showHurtPts = g_ui.showStrike = g_ui.showShapes = true; }
            Log("ui: boxes %d meter %d hurt %d strike %d shapes %d gameMenu %d menu %d", g_ui.showBoxes, g_ui.showMeter, g_ui.showHurtPts,
                g_ui.showStrike, g_ui.showShapes, g_ui.gameMenu, g_ui.menuOpen);
            continue;
        }
        else if (cmd == "preset") { st.k = Step::Mark; st.text = "@preset"; }   // practice "Reset" (sim-frame step)
        else if (cmd == "pgame" || cmd == "pdrone" || cmd == "pdump") { st.k = Step::Mark; std::string rest; std::getline(ss, rest); st.text = "@" + cmd + rest; }
        else if (cmd == "dshot") { st.k = Step::Mark; st.text = "@dshot"; }   // in sequence (works while frozen)
        else if (cmd == "projdump") { mkx::ArmProjDump(); continue; }
        else if (cmd == "padfix") { mkx::RequestPadFix(); continue; }
        else if (cmd == "pdumpnow") { mkx::PracticeDump(); continue; }
        else if (cmd == "mlscan") { movelist::RequestScan(); continue; }   // rescan the move-list tables now
        else if (cmd == "rpad") {   // real-time press (works while the game is paused): rpad <ms> <input>
            std::string in;
            ss >> st.frames >> in;
            st.k = Step::RPad;
            if (st.frames <= 0 || !ParseInput(in, st.dir, st.buttons)) { Log("bot: line %d bad rpad command", ln); continue; }
        }
        else if (cmd == "rwait") { st.k = Step::RWait; ss >> st.frames; }
        else if (cmd == "geo") { st.k = Step::Geo; ss >> st.frames >> st.text; }   // geo <frames> <label>: per-frame geometry log (runs alongside)
        else if (cmd == "rec") { st.k = Step::Rec; ss >> st.frames >> st.text; }   // rec <frames> <label>: start per-frame object recording (runs alongside next steps)
        else if (cmd == "ptrscan") { st.k = Step::PtrScan; std::getline(ss, st.text); }   // ptrscan <lo> <hi> <label>
        else if (cmd == "names") { st.k = Step::Names; std::getline(ss, st.text); }   // names <label>: log string pointers reachable from P1 (sim-frame step)
        else if (cmd == "peek") { st.k = Step::Peek; std::getline(ss, st.text); }   // peek <hexaddr> <bytes> <label>
        else if (cmd == "find") { st.k = Step::Find; std::getline(ss, st.text); }   // find i32|f32|i16|i8 v1 v2 ... : scan memory for the sequence
        else if (cmd == "dump") { st.k = Step::Dump; ss >> st.text; }
        else if (cmd == "stop") { steps.clear(); break; }
        else { Log("bot: line %d unknown command '%s'", ln, cmd.c_str()); continue; }
        steps.push_back(st);
    }
    f.close();
    DeleteFileA(path.c_str());
    std::lock_guard<std::mutex> l(g_mtx);
    g_steps.swap(steps);
    g_stepStarted = false;
    g_active = !g_steps.empty();
    g_out = 0;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    Log("bot: script loaded (%d steps), game window %s", (int)g_steps.size(),
        pid == GetCurrentProcessId() ? "focused" : "NOT focused (the game ignores pad input while in the background)");
}

static uint16_t DirBits(int d) {
    // numpad relative to facing -> d-pad
    bool up = d >= 7, down = d <= 3;
    int h = (d - 1) % 3;   // 0 back-ish(1,4,7) 1 neutral 2 forward
    bool fwd = h == 2, back = h == 0;
    bool right = g_facingRight ? fwd : back;
    bool left = g_facingRight ? back : fwd;
    uint16_t b = 0;
    if (up) b |= XINPUT_GAMEPAD_DPAD_UP;
    if (down) b |= XINPUT_GAMEPAD_DPAD_DOWN;
    if (left) b |= XINPUT_GAMEPAD_DPAD_LEFT;
    if (right) b |= XINPUT_GAMEPAD_DPAD_RIGHT;
    return b;
}

static ULONGLONG g_rtEnd = 0;
static bool g_rtStarted = false;
// Real-time steps (menus, game paused): driven from the render thread.
void OnRenderFrame() {
    std::lock_guard<std::mutex> l(g_mtx);
    ULONGLONG now = GetTickCount64();
    while (!g_steps.empty()) {
        Step& s = g_steps.front();
        bool rt = s.k == Step::RPad || s.k == Step::RWait || s.k == Step::Dump || s.k == Step::Mark || s.k == Step::Reload || s.k == Step::Find || s.k == Step::Peek;
        if (!rt || (g_stepStarted && !g_rtStarted)) return;   // a frame-based step is running
        if (s.k == Step::Mark && s.text.rfind("@p", 0) == 0) return;   // practice controls wait for the game thread
        if (s.k == Step::Mark) {
            if (s.text == "@freeze") mkx::SetFrozen(true);
            else if (s.text == "@unfreeze") mkx::SetFrozen(false);
            else if (s.text == "@dshot") shot::RequestToGameDir();
            else Log("mark:%s", s.text.c_str());
            g_steps.pop_front(); continue;
        }
        if (s.k == Step::Reload) { g_reloadRequested = true; g_steps.clear(); break; }
        if (s.k == Step::Dump) { DumpMemory(s.text); g_steps.pop_front(); continue; }
        if (s.k == Step::Find) { FindSeq(s.text); g_steps.pop_front(); continue; }
        if (s.k == Step::Peek) {
            std::stringstream ps(s.text);
            std::string a, label; size_t n = 0;
            ps >> a >> n >> label;
            uintptr_t addr = (uintptr_t)strtoull(a.c_str(), nullptr, 16);
            std::vector<uint8_t> b(n);
            SIZE_T got = 0;
            ReadProcessMemory(GetCurrentProcess(), (void*)addr, b.data(), n, &got);
            FILE* f = fopen((g_dir + "MKXFrameLab_peek_" + label + ".bin").c_str(), "wb");
            if (f) { uint64_t h = addr; fwrite(&h, 8, 1, f); fwrite(b.data(), 1, got, f); fclose(f); }
            Log("bot: peek %llX %zu bytes -> %zu read", (unsigned long long)addr, n, (size_t)got);
            g_steps.pop_front(); continue;
        }
        if (!g_rtStarted) { g_rtStarted = true; g_stepStarted = true; g_rtEnd = now + (ULONGLONG)s.frames; }
        g_out = s.k == Step::RPad ? (uint16_t)(DirBits(s.dir) | s.buttons) : 0;
        if (now < g_rtEnd) return;
        g_out = 0;
        g_steps.pop_front();
        g_rtStarted = false; g_stepStarted = false;
    }
    if (g_active && g_steps.empty()) { g_active = false; g_out = 0; Log("bot: script finished"); }
}

// per-frame raw recording of both fighters' objects (RE helper)
static FILE* g_rec = nullptr;
static int g_recLeft = 0;
static void RecFrame(const mkx::Snapshot& sn) {
    if (!g_rec) return;
    uint64_t h[2] = {sn.frame, 0};
    fwrite(h, 8, 2, g_rec);
    for (int p = 0; p < 2; ++p) {
        uint32_t hdr[2] = {sn.p[p].stateFlags, (uint32_t)sn.p[p].meleeState};
        fwrite(hdr, 4, 2, g_rec);
        static uint8_t b[0x3000 + 0x1000];
        memset(b, 0, sizeof(b));
        SIZE_T got;
        if (sn.p[p].obj) ReadProcessMemory(GetCurrentProcess(), (void*)sn.p[p].obj, b, 0x3000, &got);
        if (sn.p[p].info) ReadProcessMemory(GetCurrentProcess(), (void*)sn.p[p].info, b + 0x3000, 0x1000, &got);
        fwrite(b, 1, sizeof(b), g_rec);
    }
    if (--g_recLeft <= 0) { fclose(g_rec); g_rec = nullptr; Log("bot: recording finished"); }
}

static FILE* g_geo = nullptr;
static int g_geoLeft = 0;
static void GeoFrame(const mkx::Snapshot& sn) {
    if (!g_geo) return;
    fprintf(g_geo, "F %llu p1 %X ms %d c %d p2 %X ms %d id %X melee %llX proj %d str %d\n", (unsigned long long)sn.frame, sn.p[0].stateFlags,
            sn.p[0].meleeState, sn.p[0].contactNew ? 1 : 0, sn.p[1].stateFlags, sn.p[1].meleeState, (unsigned)sn.p[0].moveId,
            (unsigned long long)sn.p[0].melee, sn.p[0].projActive ? 1 : 0, sn.p[0].striking ? 1 : 0);
    for (int p = 0; p < 2; ++p) {
        fprintf(g_geo, "P %d %.3f %.3f %.3f\n", p, sn.p[p].pos.x, sn.p[p].pos.y, sn.p[p].pos.z);
        for (auto& h : sn.p[p].strike) fprintf(g_geo, "S %d %.3f %.3f %.3f %.3f\n", p, h.pos.x, h.pos.y, h.pos.z, h.radius);
        for (auto& h : sn.p[p].hurt) fprintf(g_geo, "H %d %d %.3f %.3f %.3f %.3f\n", p, h.bone, h.pos.x, h.pos.y, h.pos.z, h.radius);
    }
    for (auto& sh : sn.shapes) {
        fprintf(g_geo, "X %llX k %d t %d st %d o %d pos %.3f %.3f %.3f r %.3f w %.3f l %.3f m", (unsigned long long)sh.ptr, sh.kind, sh.type,
                sh.state, sh.owner, sh.pos.x, sh.pos.y, sh.pos.z, sh.radius, sh.width, sh.length);
        for (int i = 0; i < 16; ++i) fprintf(g_geo, " %.3f", sh.mat[i]);
        fprintf(g_geo, " poly");
        for (auto& v : sh.poly) fprintf(g_geo, " %.2f,%.2f,%.2f", v.x, v.y, v.z);
        fprintf(g_geo, "\n");
    }
    if (--g_geoLeft <= 0) { fclose(g_geo); g_geo = nullptr; Log("bot: geo log finished"); }
}

void OnSimFrame(const mkx::Snapshot& sn) {
    std::lock_guard<std::mutex> l(g_mtx);
    RecFrame(sn);
    GeoFrame(sn);
    if (!g_steps.empty() && (g_steps.front().k == Step::RPad || g_steps.front().k == Step::RWait)) return;
    if (sn.p[0].valid && sn.p[1].valid && sn.camValid) {
        // camera right vector (UE3 yaw, 65536 = 360 deg); P1 faces right if P2 is to the right on screen
        float yaw = sn.camRot.yaw * (6.2831853f / 65536.f);
        float rx = -sinf(yaw), ry = cosf(yaw);
        float d = (sn.p[1].pos.x - sn.p[0].pos.x) * rx + (sn.p[1].pos.y - sn.p[0].pos.y) * ry;
        if (fabsf(d) > 1.f) g_facingRight = d > 0;
    }
    while (!g_steps.empty()) {
        Step& s = g_steps.front();
        if (!g_stepStarted) {
            g_stepStarted = true;
            g_left = s.frames;
            g_freeRun = 0;
            if (s.k == Step::Mark && s.text.rfind("@p", 0) == 0) {   // practice controls: on the game thread
                std::stringstream ps(s.text.substr(1));
                std::string c; int a = 0, b2 = 0, c2 = 0;
                ps >> c >> a >> b2 >> c2;
                bool ok = true;
                if (c == "preset") ok = mkx::PracticeReset();
                else if (c == "pgame") ok = mkx::PracticeSetGame(a, b2);
                else if (c == "pdrone") ok = mkx::PracticeSetDrone(a, b2, c2);
                else if (c == "pdump") mkx::PracticeDump();
                if (!ok) Log("bot: %s failed", s.text.c_str());
                g_steps.pop_front(); g_stepStarted = false; continue;
            }
            if (s.k == Step::Mark) {
                if (s.text == "@freeze") mkx::SetFrozen(true);
                else if (s.text == "@unfreeze") mkx::SetFrozen(false);
                else if (s.text == "@dshot") shot::RequestToGameDir();
                else Log("mark:%s", s.text.c_str());
                g_steps.pop_front(); g_stepStarted = false; continue;
            }
            if (s.k == Step::PtrScan) {
                std::stringstream ps(s.text);
                std::string a, b, label;
                ps >> a >> b >> label;
                uintptr_t lo = strtoull(a.c_str(), nullptr, 16), hi = strtoull(b.c_str(), nullptr, 16);
                FILE* f = fopen((g_dir + "MKXFrameLab_ptrscan.txt").c_str(), "a");
                if (f) {
                    fprintf(f, "== %s frame %llu P1flags %X moveId %X\n", label.c_str(), (unsigned long long)sn.frame, sn.p[0].stateFlags, (unsigned)sn.p[0].moveId);
                    uintptr_t roots[2] = {sn.p[0].obj, sn.p[0].info};
                    const char* rn[2] = {"obj", "info"};
                    static uint8_t buf[0x4000], b2[0x400];
                    for (int r = 0; r < 2; ++r) {
                        SIZE_T got = 0;
                        if (!roots[r] || !ReadProcessMemory(GetCurrentProcess(), (void*)roots[r], buf, sizeof(buf), &got)) continue;
                        for (size_t o = 0; o + 8 <= got; o += 8) {
                            uintptr_t p; memcpy(&p, buf + o, 8);
                            if (p >= lo && p < hi) fprintf(f, "%s+%zX = %llX\n", rn[r], o, (unsigned long long)p);
                            if (p < 0x10000 || (p & 7)) continue;
                            SIZE_T g2 = 0;
                            if (!ReadProcessMemory(GetCurrentProcess(), (void*)p, b2, sizeof(b2), &g2)) continue;
                            for (size_t q = 0; q + 8 <= g2; q += 8) {
                                uintptr_t p2; memcpy(&p2, b2 + q, 8);
                                if (p2 >= lo && p2 < hi) fprintf(f, "[%s+%zX]+%zX = %llX\n", rn[r], o, q, (unsigned long long)p2);
                            }
                        }
                    }
                    fclose(f);
                }
                Log("bot: ptrscan '%s' written", label.c_str());
                g_steps.pop_front(); g_stepStarted = false; continue;
            }
            if (s.k == Step::Names) {
                FILE* f = fopen((g_dir + "MKXFrameLab_names.txt").c_str(), "a");
                auto ascii = [](const uint8_t* b, int n) {
                    int k = 0;
                    while (k < n && b[k] >= 0x20 && b[k] < 0x7f) ++k;
                    return (k >= 5 && k < n && b[k] == 0) ? k : 0;
                };
                if (f) {
                    fprintf(f, "== %s frame %llu P1flags %X moveId %X\n", s.text.c_str(), (unsigned long long)sn.frame, sn.p[0].stateFlags, (unsigned)sn.p[0].moveId);
                    uintptr_t roots[2] = {sn.p[0].obj, sn.p[0].info};
                    const char* rn[2] = {"obj", "info"};
                    for (int r = 0; r < 2; ++r) {
                        static uint8_t buf[0x4000];
                        SIZE_T got = 0;
                        if (!roots[r] || !ReadProcessMemory(GetCurrentProcess(), (void*)roots[r], buf, sizeof(buf), &got)) continue;
                        for (size_t o = 0; o + 8 <= got; o += 8) {
                            uintptr_t p; memcpy(&p, buf + o, 8);
                            if (p < 0x10000 || (p & 3)) continue;
                            uint8_t t[64]; SIZE_T g2 = 0;
                            if (!ReadProcessMemory(GetCurrentProcess(), (void*)p, t, sizeof(t), &g2) || g2 != sizeof(t)) continue;
                            int k = ascii(t, 63);
                            if (k) fprintf(f, "%s+%zX -> \"%.*s\"\n", rn[r], o, k, (const char*)t);
                            // one more level: pointer to an object whose fields point to strings
                            for (int q = 0; q < 8; ++q) {
                                uintptr_t p2; memcpy(&p2, t + q * 8, 8);
                                if (p2 < 0x10000 || (p2 & 3)) continue;
                                uint8_t t2[48]; SIZE_T g3 = 0;
                                if (!ReadProcessMemory(GetCurrentProcess(), (void*)p2, t2, sizeof(t2), &g3) || g3 != sizeof(t2)) continue;
                                int k2 = ascii(t2, 47);
                                if (k2) fprintf(f, "[%s+%zX]+%X -> \"%.*s\"\n", rn[r], o, q * 8, k2, (const char*)t2);
                            }
                        }
                    }
                    fclose(f);
                }
                Log("bot: names '%s' written", s.text.c_str());
                g_steps.pop_front(); g_stepStarted = false; continue;
            }
            if (s.k == Step::Geo) {
                if (g_geo) fclose(g_geo);
                g_geo = fopen((g_dir + "MKXFrameLab_geo_" + s.text + ".txt").c_str(), "w");
                g_geoLeft = s.frames;
                g_steps.pop_front(); g_stepStarted = false; continue;
            }
            if (s.k == Step::Rec) {
                if (g_rec) fclose(g_rec);
                g_rec = fopen((g_dir + "MKXFrameLab_rec_" + s.text + ".bin").c_str(), "wb");
                g_recLeft = s.frames;
                Log("bot: recording %d frames to rec_%s", s.frames, s.text.c_str());
                g_steps.pop_front(); g_stepStarted = false; continue;
            }
            if (s.k == Step::Reload) { g_reloadRequested = true; g_steps.clear(); break; }
            if (s.k == Step::Dump) { DumpMemory(s.text); g_steps.pop_front(); g_stepStarted = false; continue; }
        }
        if (s.k == Step::Pad) {
            g_out = (uint16_t)(DirBits(s.dir) | s.buttons);
            if (--g_left <= 0) { g_steps.pop_front(); g_stepStarted = false; }
            g_status = "running";
            return;
        }
        if (s.k == Step::Wait) {
            g_out = 0;
            if (--g_left <= 0) { g_steps.pop_front(); g_stepStarted = false; }
            return;
        }
        if (s.k == Step::WaitFree) {
            g_out = 0;
            auto neutral = [](uint32_t st) {
                static const uint32_t n[] = {0, 0x100, 0x101, 0x800, 0x901, 0xA01, 0xB00, 0x2000, 0x2001, 0x2002, 0x200C, 0x200D};
                for (uint32_t v : n) if (st == v) return true;
                return false;
            };
            bool free = neutral(sn.p[0].stateFlags) && neutral(sn.p[1].stateFlags) && !sn.p[0].projAlive;
            g_freeRun = free ? g_freeRun + 1 : 0;
            if (g_freeRun >= 10 || --g_left <= 0) { g_steps.pop_front(); g_stepStarted = false; }
            return;
        }
        g_steps.pop_front(); g_stepStarted = false;
    }
    if (g_active) { g_active = false; g_out = 0; g_status = "idle"; Log("bot: script finished"); }
}

bool Running() { return g_active; }
std::string Status() { std::lock_guard<std::mutex> l(g_mtx); return g_active ? "running (" + std::to_string(g_steps.size()) + " steps left)" : "idle"; }

bool Override(uint16_t& buttons) {
    if (!g_active) return false;
    buttons = g_out;
    return true;
}

} // namespace bot
