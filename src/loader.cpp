// MKXFrameLab.asi - tiny loader that makes the mod hot-reloadable.
// It copies MKXFrameLab_core.dll to a temporary name and loads the copy, so the real file is never locked
// and can be replaced while the game runs. "Reload mod" in the menu (or Ctrl+F5) unloads the copy and
// loads the newest core.
#include <windows.h>
#include <string>
#include <cstdio>

using StartFn = void (*)(int);
using StopFn = int (*)();
using WantFn = int (*)();

static std::string g_dir;
static HMODULE g_core = nullptr;
static std::string g_live;
static int g_gen = 0;
static FILE* g_log = nullptr;

static void L(const char* fmt, const char* a = "", int b = 0) {
    if (!g_log) g_log = fopen((g_dir + "MKXFrameLab_loader.log").c_str(), "w");
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
    fprintf(g_log, fmt, a, b);
    fputc('\n', g_log);
    fflush(g_log);
}

static void CleanOldCopies() {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((g_dir + "MKXFrameLab_core.live*.dll").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do { DeleteFileA((g_dir + fd.cFileName).c_str()); } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static bool LoadCore() {
    std::string src = g_dir + "MKXFrameLab_core.dll";
    char name[64];
    snprintf(name, sizeof(name), "MKXFrameLab_core.live%d_%lu.dll", g_gen, GetTickCount());
    std::string dst = g_dir + name;
    if (!CopyFileA(src.c_str(), dst.c_str(), FALSE)) { L("copy of %s failed (error %d)", src.c_str(), (int)GetLastError()); return false; }
    HMODULE m = LoadLibraryA(dst.c_str());
    if (!m) { L("LoadLibrary %s failed (error %d)", dst.c_str(), (int)GetLastError()); DeleteFileA(dst.c_str()); return false; }
    auto start = (StartFn)GetProcAddress(m, "CoreStart");
    if (!start || !GetProcAddress(m, "CoreStop")) { L("%s has no CoreStart/CoreStop exports", dst.c_str()); FreeLibrary(m); return false; }
    g_core = m;
    g_live = dst;
    L("loaded %s (load #%d)", name, g_gen + 1);
    start(g_gen);
    return true;
}

static bool HotkeyPressed() {
    static bool was = false;
    bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F5) & 0x8000);
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    bool ours = pid == GetCurrentProcessId();
    bool edge = down && !was && ours;
    was = down;
    return edge;
}

static DWORD WINAPI LoaderThread(LPVOID) {
    CleanOldCopies();
    LoadCore();
    for (;;) {
        Sleep(100);
        bool want = HotkeyPressed();
        if (g_core) {
            auto wantFn = (WantFn)GetProcAddress(g_core, "CoreWantsReload");
            if (wantFn && wantFn()) want = true;
        }
        if (!want) continue;
        if (g_core) {
            auto stop = (StopFn)GetProcAddress(g_core, "CoreStop");
            if (!stop()) { L("core refused to unload - restart the game to update"); continue; }
            Sleep(100);
            FreeLibrary(g_core);
            g_core = nullptr;
            DeleteFileA(g_live.c_str());
            L("unloaded previous core");
        }
        ++g_gen;
        if (!LoadCore()) L("reload failed - fix the core file and press Ctrl+F5 again");
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        char path[MAX_PATH];
        GetModuleFileNameA(inst, path, MAX_PATH);
        g_dir = path;
        g_dir = g_dir.substr(0, g_dir.find_last_of("\\/") + 1);
        HANDLE t = CreateThread(nullptr, 0, LoaderThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
