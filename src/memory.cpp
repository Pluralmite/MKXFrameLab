#include "memory.h"
#include <psapi.h>
#include <cctype>
#include <cstring>

namespace mem {

static uintptr_t g_base = 0;
static size_t g_size = 0;

static void InitModule() {
    if (g_base) return;
    HMODULE h = GetModuleHandleW(nullptr);
    MODULEINFO mi{};
    if (GetModuleInformation(GetCurrentProcess(), h, &mi, sizeof(mi))) {
        g_base = (uintptr_t)mi.lpBaseOfDll;
        g_size = mi.SizeOfImage;
    } else {
        g_base = (uintptr_t)h;
        g_size = 0;
    }
}

uintptr_t ModuleBase() { InitModule(); return g_base; }
size_t ModuleSize() { InitModule(); return g_size; }

bool ReadRaw(uintptr_t addr, void* out, size_t len) {
    if (addr < 0x10000 || addr > 0x7FFFFFFFFFFFull) return false;
    SIZE_T n = 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)addr, out, len, &n) && n == len;
}

bool WriteRaw(uintptr_t addr, const void* in, size_t len) {
    if (addr < 0x10000 || addr > 0x7FFFFFFFFFFFull) return false;
    SIZE_T n = 0;
    return WriteProcessMemory(GetCurrentProcess(), (LPVOID)addr, in, len, &n) && n == len;
}

bool LooksLikePointer(uint64_t v) {
    if (v < 0x10000 || v > 0x7FFFFFFFFFFFull) return false;
    unsigned char b;
    return ReadRaw((uintptr_t)v, &b, 1);
}

uintptr_t FindPattern(const std::string& pattern) {
    std::vector<int> bytes; // -1 = wildcard
    for (size_t i = 0; i < pattern.size();) {
        if (isspace((unsigned char)pattern[i])) { ++i; continue; }
        if (pattern[i] == '?') {
            bytes.push_back(-1);
            ++i;
            if (i < pattern.size() && pattern[i] == '?') ++i;
            continue;
        }
        if (i + 1 >= pattern.size()) break;
        char hex[3] = {pattern[i], pattern[i + 1], 0};
        bytes.push_back((int)strtoul(hex, nullptr, 16));
        i += 2;
    }
    if (bytes.empty()) return 0;

    uintptr_t base = ModuleBase();
    size_t size = ModuleSize();
    if (!base || !size) return 0;

    // Walk committed, readable regions of the module only.
    uintptr_t p = base, end = base + size;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi))) break;
        uintptr_t rstart = (uintptr_t)mbi.BaseAddress;
        uintptr_t rend = rstart + mbi.RegionSize;
        if (rend > end) rend = end;
        bool readable = mbi.State == MEM_COMMIT &&
                        !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && mbi.Protect != 0;
        if (readable && rend - rstart >= bytes.size()) {
            const unsigned char* d = (const unsigned char*)rstart;
            size_t n = rend - rstart - bytes.size();
            for (size_t i = 0; i <= n; ++i) {
                bool ok = true;
                for (size_t j = 0; j < bytes.size(); ++j) {
                    if (bytes[j] >= 0 && d[i + j] != (unsigned char)bytes[j]) { ok = false; break; }
                }
                if (ok) return rstart + i;
            }
        }
        p = rend;
    }
    return 0;
}

// --------------------------- expression parser -----------------------------

struct Expr::Node {
    enum Kind { Num, Rva, Abs, Sig, Self, P1, P2, Deref, Add } kind = Num;
    uint64_t value = 0;
    std::string pattern;
    int ripPos = -1;
    mutable bool sigResolved = false;
    mutable uintptr_t sigAddr = 0;
    std::shared_ptr<Node> a, b;
    bool subtract = false;
};

namespace {
struct Parser {
    const std::string& s;
    size_t i = 0;
    std::string err;

    explicit Parser(const std::string& str) : s(str) {}

    void ws() { while (i < s.size() && isspace((unsigned char)s[i])) ++i; }

    bool match(const char* kw) {
        ws();
        size_t n = strlen(kw);
        if (s.size() - i >= n && _strnicmp(s.c_str() + i, kw, n) == 0) { i += n; return true; }
        return false;
    }

    bool hex(uint64_t& out) {
        ws();
        if (s.size() - i >= 2 && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) i += 2;
        size_t start = i;
        out = 0;
        while (i < s.size() && isxdigit((unsigned char)s[i])) {
            char c = (char)tolower(s[i]);
            out = out * 16 + (uint64_t)(isdigit((unsigned char)c) ? c - '0' : c - 'a' + 10);
            ++i;
        }
        return i > start;
    }

    std::shared_ptr<Expr::Node> atom() {
        ws();
        auto n = std::make_shared<Expr::Node>();
        if (match("[")) {
            n->kind = Expr::Node::Deref;
            n->a = expr();
            if (!n->a) return nullptr;
            if (!match("]")) { err = "missing ']'"; return nullptr; }
            return n;
        }
        if (match("rva:")) { n->kind = Expr::Node::Rva; if (!hex(n->value)) { err = "bad rva"; return nullptr; } return n; }
        if (match("abs:")) { n->kind = Expr::Node::Abs; if (!hex(n->value)) { err = "bad abs"; return nullptr; } return n; }
        if (match("sig(")) {
            n->kind = Expr::Node::Sig;
            ws();
            if (i >= s.size() || s[i] != '"') { err = "sig needs a quoted pattern"; return nullptr; }
            size_t close = s.find('"', i + 1);
            if (close == std::string::npos) { err = "unterminated sig pattern"; return nullptr; }
            n->pattern = s.substr(i + 1, close - i - 1);
            i = close + 1;
            if (match(",")) {
                uint64_t pos;
                if (!hex(pos)) { err = "bad sig rip offset"; return nullptr; }
                n->ripPos = (int)pos;
            }
            if (!match(")")) { err = "missing ')' after sig"; return nullptr; }
            return n;
        }
        if (match("self")) { n->kind = Expr::Node::Self; return n; }
        if (match("p1")) { n->kind = Expr::Node::P1; return n; }
        if (match("p2")) { n->kind = Expr::Node::P2; return n; }
        n->kind = Expr::Node::Num;
        if (!hex(n->value)) { err = "unexpected text at position " + std::to_string(i); return nullptr; }
        return n;
    }

    std::shared_ptr<Expr::Node> expr() {
        auto left = atom();
        if (!left) return nullptr;
        for (;;) {
            ws();
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
                bool sub = s[i] == '-';
                ++i;
                auto right = atom();
                if (!right) return nullptr;
                auto n = std::make_shared<Expr::Node>();
                n->kind = Expr::Node::Add;
                n->a = left;
                n->b = right;
                n->subtract = sub;
                left = n;
            } else break;
        }
        return left;
    }
};

uintptr_t EvalNode(const Expr::Node* n, const EvalCtx& c) {
    if (!n) return 0;
    switch (n->kind) {
    case Expr::Node::Num: return (uintptr_t)n->value;
    case Expr::Node::Rva: return ModuleBase() + (uintptr_t)n->value;
    case Expr::Node::Abs: return (uintptr_t)n->value;
    case Expr::Node::Self: return c.self;
    case Expr::Node::P1: return c.p1;
    case Expr::Node::P2: return c.p2;
    case Expr::Node::Sig: {
        if (!n->sigResolved) {
            n->sigResolved = true;
            uintptr_t m = FindPattern(n->pattern);
            if (m && n->ripPos >= 0) {
                int32_t disp = 0;
                if (Read(m + n->ripPos, disp)) m = m + n->ripPos + 4 + disp; else m = 0;
            }
            n->sigAddr = m;
        }
        return n->sigAddr;
    }
    case Expr::Node::Deref: {
        uintptr_t a = EvalNode(n->a.get(), c);
        if (!a) return 0;
        uint64_t v = 0;
        return Read(a, v) ? (uintptr_t)v : 0;
    }
    case Expr::Node::Add: {
        uintptr_t a = EvalNode(n->a.get(), c);
        if (!a) return 0;
        uintptr_t b = EvalNode(n->b.get(), c);
        return n->subtract ? a - b : a + b;
    }
    }
    return 0;
}
} // namespace

std::shared_ptr<Expr> Expr::Parse(const std::string& text, std::string* error) {
    std::string t = text;
    while (!t.empty() && isspace((unsigned char)t.back())) t.pop_back();
    size_t f = t.find_first_not_of(" \t");
    if (f == std::string::npos) return nullptr;
    t = t.substr(f);
    Parser p(t);
    auto root = p.expr();
    p.ws();
    if (root && p.i != t.size()) { p.err = "trailing text at position " + std::to_string(p.i); root = nullptr; }
    if (!root) {
        if (error) *error = p.err.empty() ? "parse error" : p.err;
        return nullptr;
    }
    auto e = std::make_shared<Expr>();
    e->text_ = t;
    e->root_ = root;
    return e;
}

uintptr_t Expr::Eval(const EvalCtx& ctx) const { return EvalNode(root_.get(), ctx); }

} // namespace mem
