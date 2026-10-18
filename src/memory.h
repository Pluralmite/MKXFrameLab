// MKXFrameLab - safe memory access, signature scanning and address expressions
#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace mem {

uintptr_t ModuleBase();
size_t    ModuleSize();

// Reads through ReadProcessMemory on our own process, so a bad pointer
// returns false instead of crashing the game.
bool ReadRaw(uintptr_t addr, void* out, size_t len);
bool WriteRaw(uintptr_t addr, const void* in, size_t len);

template <class T> inline bool Read(uintptr_t addr, T& out) { return ReadRaw(addr, &out, sizeof(T)); }
template <class T> inline T ReadOr(uintptr_t addr, T fallback) { T v; return Read(addr, v) ? v : fallback; }
template <class T> inline bool Write(uintptr_t addr, const T& v) { return WriteRaw(addr, &v, sizeof(T)); }

bool LooksLikePointer(uint64_t v);

// IDA-style pattern, e.g. "48 8B 05 ?? ?? ?? ?? 48 85 C0". Searches the main module.
uintptr_t FindPattern(const std::string& pattern);

// ---------------------------------------------------------------------------
// Address expressions (Cheat Engine style), used everywhere in the .ini:
//
//   rva:2F0A3C8                module base + 0x2F0A3C8
//   abs:7FF612340000           absolute address
//   sig("48 8B 05 ?? ?? ?? ??",3)   pattern match; the 4 bytes at +3 are a
//                              RIP-relative displacement (target = match+3+4+disp).
//                              Omit ",3" to use the match address itself.
//   self / p1 / p2             current owner object, player 1 object, player 2 object
//   [ expr ]                   dereference (read 8-byte pointer)
//   expr + 1A0 / expr - 8      add / subtract hex offset
//
// Examples:
//   [[rva:2F0A3C8]+10]+88      == CE pointer: "MK10.exe"+2F0A3C8 -> 10 -> 88
//   self+1A4                   field at offset 0x1A4 of the owner object
//   [self+40]+10
// All numbers are hex (an optional 0x prefix is accepted).
// ---------------------------------------------------------------------------
struct EvalCtx {
    uintptr_t self = 0;
    uintptr_t p1 = 0;
    uintptr_t p2 = 0;
};

class Expr {
public:
    static std::shared_ptr<Expr> Parse(const std::string& text, std::string* error);
    // Returns 0 on any failure (unresolved signature, bad pointer).
    uintptr_t Eval(const EvalCtx& ctx) const;
    const std::string& Text() const { return text_; }

    struct Node;
private:
    std::string text_;
    std::shared_ptr<Node> root_;
};
using ExprPtr = std::shared_ptr<Expr>;

} // namespace mem
