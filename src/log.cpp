#include "log.h"
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <mutex>

static FILE* g_log = nullptr;
bool g_logVerbose = true;
static std::mutex g_mtx;

void LogInit(const std::string& path, bool append) {
    std::lock_guard<std::mutex> l(g_mtx);
    if (!g_log) g_log = fopen(path.c_str(), append ? "a" : "w");
}

void LogClose() {
    std::lock_guard<std::mutex> l(g_mtx);
    if (g_log) { fclose(g_log); g_log = nullptr; }
}

void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> l(g_mtx);
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a;
    va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fputc('\n', g_log);
    fflush(g_log);
}
