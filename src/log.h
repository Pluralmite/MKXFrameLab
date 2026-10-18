#pragma once
#include <string>
void LogInit(const std::string& path, bool append = false);
void LogClose();
void Log(const char* fmt, ...);
extern bool g_logVerbose;   // per-object / per-hit diagnostics (VerboseLog in the ini)
