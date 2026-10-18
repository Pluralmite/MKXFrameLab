// MKXFrameLab - hardware data-breakpoint tracer (reverse-engineering aid)
#pragma once
#include <cstdint>
#include <string>

namespace trace {
// Arm up to 4 hardware breakpoints (read/write, 4 bytes) for `seconds`.
// Every hit records the instruction address and registers; results are
// written to MKXFrameLab_trace_<label>.txt next to the plugin.
bool Start(const uintptr_t addrs[4], const char* const names[4], int seconds, const std::string& label);
bool Active();
std::string LastFile();
void Poll();
void Shutdown();                // clear debug registers, remove the exception handler (hot reload)
// Thread-safe: queue a Start() to be performed on the next Poll() (e.g. from the game thread).
void Request(const uintptr_t addrs[4], const char* const names[4], int seconds, const std::string& label); // call regularly (render thread): disarms and writes the report when time is up
}
