#include "trace.h"
#include "log.h"
#include "memory.h"
#include <windows.h>
#include <tlhelp32.h>
#include <atomic>
#include <mutex>
#include <map>
#include <vector>
#include <cstdio>

namespace trace {

struct HitInfo {
    uint64_t count = 0;
    CONTEXT first;
    uint64_t stack[8];
};

static std::atomic<bool> g_active{false};
static uintptr_t g_addr[4] = {};
static std::string g_name[4];
static std::mutex g_mtx;
static std::map<std::pair<int, uintptr_t>, HitInfo> g_hits; // (dr index, rip) -> info
static ULONGLONG g_endTick = 0;
static std::string g_label, g_last;
static PVOID g_veh = nullptr;
static std::atomic<uint64_t> g_total{0};

static LONG CALLBACK Handler(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    DWORD64 dr6 = c->Dr6;
    if (!(dr6 & 0xF)) return EXCEPTION_CONTINUE_SEARCH;
    if (g_active) {
        for (int i = 0; i < 4; ++i) {
            if (!(dr6 & (1ull << i))) continue;
            std::lock_guard<std::mutex> l(g_mtx);
            auto& h = g_hits[{i, (uintptr_t)c->Rip}];
            if (h.count++ == 0) {
                h.first = *c;
                for (int k = 0; k < 8; ++k) h.stack[k] = mem::ReadOr<uint64_t>(c->Rsp + 8 * k, 0);
            }
            g_total++;
        }
    }
    c->Dr6 = 0;
    c->EFlags |= 0x10000; // RF
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void ApplyToAllThreads(bool arm) {
    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{sizeof(te)};
    int n = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!th) continue;
        if (SuspendThread(th) != (DWORD)-1) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(th, &c)) {
                DWORD64 dr7 = c.Dr7;
                for (int i = 0; i < 4; ++i) {
                    dr7 &= ~(3ull << (i * 2));
                    dr7 &= ~(0xFull << (16 + i * 4));
                    (&c.Dr0)[i] = 0;
                    if (arm && g_addr[i]) {
                        (&c.Dr0)[i] = g_addr[i];
                        dr7 |= 1ull << (i * 2);              // local enable
                        dr7 |= (3ull | (3ull << 2)) << (16 + i * 4); // RW=11 (read/write), LEN=11 (4 bytes)
                    }
                }
                c.Dr7 = dr7;
                c.Dr6 = 0;
                if (SetThreadContext(th, &c)) n++;
            }
            ResumeThread(th);
        }
        CloseHandle(th);
    }
    CloseHandle(snap);
    Log("trace: %s debug registers on %d threads", arm ? "armed" : "cleared", n);
}

bool Start(const uintptr_t addrs[4], const char* const names[4], int seconds, const std::string& label) {
    if (g_active) return false;
    {
        std::lock_guard<std::mutex> l(g_mtx);
        g_hits.clear();
    }
    for (int i = 0; i < 4; ++i) { g_addr[i] = addrs[i] & ~3ull; g_name[i] = names[i] ? names[i] : ""; }
    g_label = label;
    g_total = 0;
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, Handler);
    g_active = true;
    ApplyToAllThreads(true);
    g_endTick = GetTickCount64() + (ULONGLONG)seconds * 1000;
    Log("trace '%s' started for %ds", label.c_str(), seconds);
    return true;
}

bool Active() { return g_active; }
std::string LastFile() { std::lock_guard<std::mutex> l(g_mtx); return g_last; }

static std::mutex g_reqMtx;
static bool g_req = false;
static uintptr_t g_reqAddr[4];
static std::string g_reqName[4];
static int g_reqSec = 0;
static std::string g_reqLabel;

void Request(const uintptr_t addrs[4], const char* const names[4], int seconds, const std::string& label) {
    std::lock_guard<std::mutex> l(g_reqMtx);
    if (g_req || g_active) return;
    for (int i = 0; i < 4; ++i) { g_reqAddr[i] = addrs[i]; g_reqName[i] = names[i] ? names[i] : ""; }
    g_reqSec = seconds;
    g_reqLabel = label;
    g_req = true;
}

void Poll() {
    {
        std::lock_guard<std::mutex> l(g_reqMtx);
        if (g_req && !g_active) {
            const char* n[4] = {g_reqName[0].c_str(), g_reqName[1].c_str(), g_reqName[2].c_str(), g_reqName[3].c_str()};
            g_req = false;
            Start(g_reqAddr, n, g_reqSec, g_reqLabel);
            return;
        }
    }
    if (!g_active || GetTickCount64() < g_endTick) return;
    ApplyToAllThreads(false);
    g_active = false;
    char path[MAX_PATH];
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&Poll, &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    std::string dir = path;
    dir = dir.substr(0, dir.find_last_of("\\/") + 1);
    std::string file = dir + "MKXFrameLab_trace_" + g_label + ".txt";
    FILE* f = fopen(file.c_str(), "w");
    uintptr_t base = mem::ModuleBase(), end = base + mem::ModuleSize();
    auto rva = [&](uint64_t v) -> unsigned long long { return (v >= base && v < end) ? v - base : v; };
    if (f) {
        fprintf(f, "base %llX total hits %llu\n", (unsigned long long)base, (unsigned long long)g_total.load());
        for (int i = 0; i < 4; ++i) fprintf(f, "DR%d %llX %s\n", i, (unsigned long long)g_addr[i], g_name[i].c_str());
        std::lock_guard<std::mutex> l(g_mtx);
        for (auto& kv : g_hits) {
            const CONTEXT& c = kv.second.first;
            fprintf(f, "\nDR%d rip rva %llX  count %llu\n", kv.first.first, rva(kv.first.second), (unsigned long long)kv.second.count);
            fprintf(f, "  rax %llX rbx %llX rcx %llX rdx %llX\n  rsi %llX rdi %llX rbp %llX rsp %llX\n  r8 %llX r9 %llX r10 %llX r11 %llX\n  r12 %llX r13 %llX r14 %llX r15 %llX\n",
                    c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi, c.Rbp, c.Rsp, c.R8, c.R9, c.R10, c.R11, c.R12, c.R13, c.R14, c.R15);
            fprintf(f, "  stack:");
            for (int k = 0; k < 8; ++k) fprintf(f, " %llX", rva(kv.second.stack[k]));
            fprintf(f, "\n");
        }
        fclose(f);
    }
    {
        std::lock_guard<std::mutex> l(g_mtx);
        g_last = file;
    }
    Log("trace written: %s (%llu hits)", file.c_str(), (unsigned long long)g_total.load());
}

void Shutdown() {
    if (g_active) { g_active = false; ApplyToAllThreads(false); }
    if (g_veh) { RemoveVectoredExceptionHandler(g_veh); g_veh = nullptr; }
}

} // namespace trace
