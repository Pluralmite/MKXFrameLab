#include "movelist.h"
#include "log.h"
#include <windows.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstring>
#include <cctype>
#include <algorithm>

namespace movelist {

static std::mutex g_mtx;
static std::map<std::string, Entry> g_table;
static std::atomic<bool> g_scanning{false};
static std::atomic<int> g_gen{0};

const char* TypeName(int t) {
    switch (t) { case 1: return "High"; case 2: return "Mid"; case 3: return "Low"; case 5: return "Throw"; case 6: return "Overhead"; case 7: return "Utility"; case 8: return "Unblockable"; default: return "?"; }
}

// ---- Mortal Kombat X: the AI's attack table ----------------------------------------------------------------
// Every fighter has an AICharacterSensoryInfo (FGCharacterObj+0xB020, MKItem) whose TMap mAttackPropList (+0x38)
// maps an attack hash to AIAttackProps - the frame data the game itself uses for its move list and its AI:
//   +0 hash  +10 start collision frame  +14 end collision frame  +18 miss breakout (recovered) frame
//   +64 size  +68 damage  +6C block damage  +70 BLOCK_REQ  +AC move-list block frames  +B0 move-list hit frames
// Attack hash = (fn >> 12 ^ fn & 0xFFFFF) | ((sid << 20 ^ sid) & 0xFFF00000) for script function fn of script sid
// (MK10AISensoryInterface::CreateHashFromGenericCommand / FindMovesetCommandMatchesAttackHash).
static std::atomic<uintptr_t> g_obj[2];
void SetPlayers(uintptr_t o0, uintptr_t o1) { g_obj[0] = o0; g_obj[1] = o1; }
template <class T> static T Rd(uintptr_t a, T def = T()) { T v; SIZE_T got = 0; return (a > 0x10000 && ReadProcessMemory(GetCurrentProcess(), (void*)a, &v, sizeof(T), &got) && got == sizeof(T)) ? v : def; }
static uintptr_t Item(uintptr_t item) {   // MKItem / MKItemNoDestroy: object at +0x10, instance at +0x18
    uintptr_t o = Rd<uintptr_t>(item + 0x10);
    uint32_t inst = Rd<uint32_t>(item + 0x18, 0xFFFFFFFF);
    if (!o || inst == 0xFFFFFFFF) return 0;
    uint32_t oi = Rd<uint32_t>(o + 8);
    if ((oi & 0x40000000) || oi != inst) return 0;
    return o;
}
static uint32_t ScriptHash(uintptr_t script, uint32_t fn) {
    if (!script || !fn) return 0;
    uint32_t sid = Rd<uint32_t>(script + 0x90);
    return ((fn >> 12) ^ (fn & 0xFFFFF)) | (((sid << 20) ^ sid) & 0xFFF00000u);
}
static std::string ReadStr(uintptr_t p, size_t max = 96) {
    std::string s;
    for (size_t i = 0; i < max; ++i) { char c = Rd<char>(p + i); if (c < 32 || c > 126) break; s += c; }
    return s;
}
static const char* SwitchName(uint32_t v) {
    if (!(v & 0x80000000u)) return nullptr;
    switch (v & 0xFFFF) {
    case 1: return "F"; case 2: return "B"; case 3: return "U"; case 4: return "D";
    case 0x0F: return "1"; case 0x10: return "2"; case 0x11: return "3"; case 0x12: return "4";
    case 0x13: return "1+2"; case 0x14: return "1+3"; case 0x15: return "1+4"; case 0x16: return "2+3"; case 0x17: return "2+4"; case 0x18: return "3+4";
    case 0x19: return "MB"; case 0x1A: return "Throw"; case 0x1C: return "J"; case 0x1D: return "LT"; case 0x1E: return "RT"; case 0x1F: return "RB"; case 0x20: return "LB";
    case 0x21: return "DB"; case 0x22: return "DF"; case 0x23: return "UB"; case 0x24: return "UF";
    case 0x25: return "Turn"; case 0x26: return "JF"; case 0x27: return "JB"; case 0x28: return "JU";
    case 0x37: return "1(EX)"; case 0x38: return "2(EX)"; case 0x39: return "3(EX)"; case 0x3A: return "4(EX)"; case 0x3B: return "Super";
    case 0x35: return "Stance";
    default: return nullptr;
    }
}
static std::string SeqNotation(const uint32_t* seq, int n, std::string* raw) {
    std::string out;
    bool lastBtn = false;
    for (int i = 0; i < n; ++i) {
        uint32_t v = seq[i];
        if (raw) { char b[16]; snprintf(b, sizeof(b), "%s%X", i ? "," : "", v); *raw += b; }
        if (!v || v == 0x800000FDu || v == 0x80000000u) continue;
        const char* nm = SwitchName(v);
        bool btn = nm && ((nm[0] >= '1' && nm[0] <= '4') || !strcmp(nm, "Throw"));
        if (!out.empty()) out += (btn && lastBtn) ? "," : (lastBtn ? " " : "");
        if (nm) out += nm; else { char b[16]; snprintf(b, sizeof(b), "?%X", v & 0xFFFF); out += b; }
        lastBtn = btn;
    }
    return out;
}
static int TypeFromBlockReq(int br) {   // checked against the in-game move list (Kano 1 = High / req 0, B1 = Mid / req 1)
    switch (br) {
    case 0: return 1;                           // block high = High
    case 1: case 5: case 6: return 2;           // block high or low = Mid
    case 2: case 4: return 3;                   // Low
    case 3: return 6;                           // Overhead
    case 7: case 8: case 9: case 10: return 8;  // unblockable
    case 11: case 12: case 13: case 14: case 15: case 16: case 17: case 18: return 5;   // throw
    default: return 7;
    }
}
// one move-id element: (button group << 16) | direction  (F0000 = 1, F0002 = B1, 110001 = F3, F0026 = jump-forward 1)
static std::string MoveIdNotation(uint32_t id) {
    uint32_t g = id >> 16, d = id & 0xFFFF;
    const char* btn = SwitchName(0x80000000u | g);
    std::string dir;
    if (d == 0x26 || d == 0x27 || d == 0x28) dir = "J";
    else if (d) { const char* dn = SwitchName(0x80000000u | d); if (dn) dir = dn; }
    if (!btn) { char b[16]; snprintf(b, sizeof(b), "?%X", id); return b; }
    return dir + btn;
}
static std::string ChainNotation(const uint32_t* seq, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) {
        if (!seq[i] || seq[i] == 0xFFFFFFFFu) break;
        if (!out.empty()) out += ",";
        out += MoveIdNotation(seq[i]);
    }
    return out;
}
static std::string Words(const std::string& t);
// KA_BallBF -> Ball BF ; KA_KnifeToss_TRAIT2 -> Knife Toss ; KA_UpEyeLaserMB_TRAIT2 -> Up Eye Laser MB
static std::string CommandName(std::string n) {
    size_t u = n.find('_');
    if (u != std::string::npos && u <= 4) {
        bool caps = true;
        for (size_t i = 0; i < u; ++i) if (!(n[i] >= 'A' && n[i] <= 'Z')) caps = false;
        if (caps) n = n.substr(u + 1);
    }
    size_t t = n.find("_TRAIT");
    if (t != std::string::npos) n = n.substr(0, t);
    if (n.compare(0, 4, "gCom") == 0) n = n.substr(4);
    for (char& c : n) if (c == '_') c = ' ';
    return Words(n);
}
static std::map<uint32_t, Entry> g_byHash;
static std::map<uint32_t, std::string> g_learned;
static void ScanThread() {
    ULONGLONG t0 = GetTickCount64();
    std::map<std::string, Entry> found;
    std::map<uint32_t, Entry> byHash;
    std::vector<std::string> dump;
    for (int p = 0; p < 2; ++p) {
        uintptr_t obj = g_obj[p];
        if (!obj) continue;
        uintptr_t sens = Item(obj + 0xB010);
        std::map<uint32_t, std::string> names;     // hash -> move name
        std::map<uint32_t, int> cats;
        // special moves / command attacks: the fighter's two AttackCommands lists
        for (uintptr_t list : {obj + 0x9458, obj + 0x1D6E0}) {
            uint32_t n = Rd<uint32_t>(list + 0x258);
            if (n > 75) continue;
            for (uint32_t i = 0; i < n; ++i) {
                uintptr_t c = Rd<uintptr_t>(list + i * 8);
                if (!c) continue;
                uint32_t h = ScriptHash(Item(c + 0x28), Rd<uint32_t>(c + 0x50));
                std::string nm = ReadStr(Rd<uintptr_t>(c));
                std::string raw;
                uintptr_t pad = Rd<uintptr_t>(c + 0x10);
                uint32_t seq[12] = {};
                int k = 0;
                for (; pad && k < 12; ++k) { seq[k] = Rd<uint32_t>(pad + k * 4); if (!seq[k] || seq[k] == 0x800000FDu) break; }
                std::string inp = SeqNotation(seq, k, &raw);
                char line[400];
                snprintf(line, sizeof(line), "P%d cmd %08X %-40s in %-16s raw %s flags %X btn %X", p + 1, h, nm.c_str(), inp.c_str(), raw.c_str(), Rd<uint32_t>(c + 0x20), Rd<uint32_t>(c + 0xC));
                dump.push_back(line);
                if (h && !names.count(h)) { names[h] = nm.empty() ? inp : CommandName(nm); cats[h] = 2; }
            }
        }
        // normals and strings: the active moveset's move lists (four button groups)
        uintptr_t ms = Rd<uintptr_t>(obj + 0xB100);
        if (!ms) ms = obj + 0xB108;
        for (int g = 0; g < 5; ++g) {
            uint32_t n = Rd<uint32_t>(ms + 0x3C9C + g * 4);
            if (n > 60) continue;
            for (uint32_t i = 0; i < n; ++i) {
                uintptr_t mv = ms + 0x3CB0 + g * 0x10E0 + i * 0x48;
                uint32_t fn = Rd<uint32_t>(mv + 4);
                uint32_t h = ScriptHash(Item(mv + 0x20), fn);
                uint32_t seq[5];
                for (int k = 0; k < 5; ++k) seq[k] = Rd<uint32_t>(mv + 8 + k * 4);
                std::string raw, inp = SeqNotation(seq, 5, &raw);
                char line[300];
                snprintf(line, sizeof(line), "P%d move g%d %08X flags %X in %-16s raw %s", p + 1, g, h, Rd<uint32_t>(mv), inp.c_str(), raw.c_str());
                dump.push_back(line);
                if (h && !names.count(h)) { names[h] = ChainNotation(seq, 5); cats[h] = 1; }
            }
        }
        // the attack table itself (TSet elements: 0xC8 bytes = key, pad, AIAttackProps, hash links)
        if (sens) {
            uintptr_t data = Rd<uintptr_t>(sens + 0x38);
            int num = Rd<int>(sens + 0x40);
            char hdr[200];
            snprintf(hdr, sizeof(hdr), "P%d sensory %llX: %d attack props, current hash %08X id %X last move %X", p + 1, (unsigned long long)sens, num,
                     Rd<uint32_t>(sens + 0x108), Rd<uint32_t>(sens + 0x118), Rd<uint32_t>(sens + 0x120));
            dump.push_back(hdr);
            for (int i = 0; data && i < num && i < 4000; ++i) {
                uintptr_t e = data + (uintptr_t)i * 0xC8;
                uint32_t key = Rd<uint32_t>(e);
                uintptr_t v = e + 8;
                if (!key || Rd<uint32_t>(v) != key) continue;
                Entry en;
                int s0 = Rd<int>(v + 0x10), s1 = Rd<int>(v + 0x14), miss = Rd<int>(v + 0x18);
                // the move list's own arithmetic (checked: Kano 1 = 8/3/15, block 0, hit +5, cancel 14; B1 = 9/6/10, -8, +24, 20)
                en.startup = s1 > 0 ? s0 + 1 : 0; en.active = s1 > s0 ? s1 - s0 : 0; en.recovery = s1 > 0 ? miss - s1 : 0;   // projectiles: negative, like the game shows
                en.blockAdv = Rd<int>(v + 0xAC) - Rd<int>(v + 0x20); en.hitAdv = Rd<int>(v + 0xB0) - Rd<int>(v + 0x1C);
                en.cancel = Rd<int>(v + 0x94);
                en.damage = Rd<float>(v + 0x68); en.blockDamage = Rd<float>(v + 0x6C);
                int br = Rd<int>(v + 0x70);
                en.type = TypeFromBlockReq(br);
                en.flags = (int)Rd<uint32_t>(v + 8);
                auto it = names.find(key);
                if (it == names.end() && br >= 11 && br <= 18 && s1 > 0) { names[key] = "Throw"; cats[key] = 1; it = names.find(key); }
                en.category = it != names.end() ? cats[key] : 3;
                char nm[160];
                snprintf(nm, sizeof(nm), "%s|%08X", it != names.end() ? it->second.c_str() : "?", key);
                en.display = it != names.end() ? it->second : std::string();
                en.name = nm;
                en.hash = key;
                en.player = p;
                char line[400];
                snprintf(line, sizeof(line), "P%d prop %08X %-30s start %d end %d miss %d hitbrk %d blkbrk %d vhit %d vblk %d | ml blk %d hit %d | dmg %.1f/%.1f req %d ncol %d flags %llX",
                         p + 1, key, it != names.end() ? it->second.c_str() : "?", s0, s1, miss, Rd<int>(v + 0x1C), Rd<int>(v + 0x20), Rd<int>(v + 0x24), Rd<int>(v + 0x28),
                         en.blockAdv, en.hitAdv, en.damage, en.blockDamage, br, Rd<int>(v + 0x2C), (unsigned long long)Rd<uint64_t>(v + 8));
                dump.push_back(line);
                {
                    std::string ex = "      ";
                    for (int o = 0x74; o < 0xB8; o += 4) { char b[24]; snprintf(b, sizeof(b), "%X:%d ", o, Rd<int>(v + o)); ex += b; }
                    dump.push_back(ex);
                }
                byHash[key] = en;
                found[en.name] = en;
            }
        } else dump.push_back("no sensory info");
    }
    {
        std::lock_guard<std::mutex> l(g_mtx);
        for (auto& kv : g_learned) {
            auto it = byHash.find(kv.first);
            if (it == byHash.end() || !it->second.display.empty()) continue;
            char nm[160];
            snprintf(nm, sizeof(nm), "%s|%08X", kv.second.c_str(), kv.first);
            it->second.display = kv.second; it->second.name = nm; it->second.category = 1;
        }
        g_table.swap(found);
        g_byHash.swap(byHash);
    }
    {
        char path[MAX_PATH];
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&ScanThread, &self);
        GetModuleFileNameA(self, path, MAX_PATH);
        std::string dir = path;
        dir = dir.substr(0, dir.find_last_of("\\/") + 1);
        FILE* f = g_logVerbose ? fopen((dir + "MKXFrameLab_movelist.txt").c_str(), "w") : nullptr;
        if (f) { for (auto& l : dump) fprintf(f, "%s\n", l.c_str()); fclose(f); }
    }
    Log("movelist: %d attack props read in %.2f s", (int)g_table.size(), (GetTickCount64() - t0) / 1000.0);
    g_gen++;
    g_scanning = false;
}
// names learned at runtime: an unnamed move (no moveset entry of its own, e.g. the first hit of a string) that
// chains straight into "F2,1" is the "F2"
void LearnName(uint32_t h, const std::string& name) {
    std::lock_guard<std::mutex> l(g_mtx);
    auto it = g_byHash.find(h);
    if (it == g_byHash.end() || !it->second.display.empty() || g_learned.count(h)) return;
    g_learned[h] = name;
    char nm[160];
    snprintf(nm, sizeof(nm), "%s|%08X", name.c_str(), h);
    it->second.display = name;
    it->second.name = nm;
    it->second.category = 1;
}
bool ByHash(uint32_t h, Entry& out) {
    std::lock_guard<std::mutex> l(g_mtx);
    auto it = g_byHash.find(h);
    if (it == g_byHash.end()) return false;
    out = it->second;
    return true;
}
void RequestScan() {
    bool expected = false;
    if (!g_scanning.compare_exchange_strong(expected, true)) return;
    std::thread(ScanThread).detach();
}
bool Scanning() { return g_scanning; }
int Generation() { return g_gen; }
std::map<std::string, Entry> Table() { std::lock_guard<std::mutex> l(g_mtx); return g_table; }

} // namespace movelist

namespace movelist {
static const char* DirLetter(const std::string& w) {
    if (w == "AWAY") return "B";
    if (w == "TOWARDS") return "F";
    if (w == "DOWN") return "D";
    if (w == "UP") return "U";
    if (w == "DOWNTOWARDS" || w == "TOWARDSDOWN") return "DF";
    if (w == "DOWNAWAY" || w == "AWAYDOWN") return "DB";
    if (w == "UPTOWARDS" || w == "TOWARDSUP") return "UF";
    if (w == "UPAWAY" || w == "AWAYUP") return "UB";
    return nullptr;
}
// ForceOfMindMB -> Force Of Mind MB
static std::string Words(const std::string& t) {
    if (t == "SUPERMOVE") return "X-Ray";
    bool lower = false;
    for (char c : t) if (c >= 'a' && c <= 'z') lower = true;
    if (!lower) {   // ALLCAPS word: title case, short ones (MB) as they are
        if (t.size() <= 3) return t;
        std::string w = t;
        for (size_t i = 1; i < w.size(); ++i) w[i] = (char)tolower((unsigned char)w[i]);
        return w;
    }
    std::string w;
    for (size_t i = 0; i < t.size(); ++i) {
        char c = t[i];
        bool up = c >= 'A' && c <= 'Z', dig = c >= '0' && c <= '9';
        if (i && (up || dig)) {
            char p = t[i - 1];
            bool pUp = p >= 'A' && p <= 'Z', pDig = p >= '0' && p <= '9';
            bool nLow = i + 1 < t.size() && t[i + 1] >= 'a' && t[i + 1] <= 'z';
            if ((up && (!pUp || nLow)) || (dig && !pDig)) w += ' ';
        }
        w += c;
    }
    return w;
}
std::string Notation(const std::string& name) {
    {   // MKX table keys are "<display>|<hash>"
        size_t bar = name.find('|');
        if (bar != std::string::npos) return name.substr(0, bar);
    }
    std::vector<std::string> tok;
    size_t a = 0;
    for (size_t i = 0; i <= name.size(); ++i)
        if (i == name.size() || name[i] == '_') { if (i > a) tok.push_back(name.substr(a, i - a)); a = i + 1; }
    // drop a character prefix (WW_, MN_ ...) if one is still attached
    if (tok.size() > 1 && tok[0].size() <= 4 && tok[0] != "AIR" && !DirLetter(tok[0]) && tok[0].find("ATTACK") != 0 &&
        tok[0].find("THROW") != 0 && !(tok[0][0] >= '1' && tok[0][0] <= '4'))
        tok.erase(tok.begin());
    std::string out;
    bool lastHit = false, air = false;
    auto add = [&](const std::string& piece, bool hit) {
        if (!out.empty()) out += (hit && lastHit) ? "," : " ";
        out += piece; lastHit = hit;
    };
    for (const std::string& t : tok) {
        std::string body = t;
        if (body.compare(0, 6, "ATTACK") == 0) body = body.substr(6);
        if (!body.empty() && body[0] >= '1' && body[0] <= '4') {           // a button press, maybe with a direction
            std::string rest = body.substr(1), btn(1, body[0]);
            while (rest.size() >= 2 && rest[0] == 'N' && rest[1] >= '1' && rest[1] <= '4') { btn += '+'; btn += rest[1]; rest = rest.substr(2); }   // 1N3 = 1+3
            const char* d = DirLetter(rest);
            add(std::string(air ? "J" : "") + (d ? d : "") + btn, true);
            air = false;
            continue;
        }
        if (t.compare(0, 5, "THROW") == 0) {
            const char* d = DirLetter(t.substr(5));
            add(std::string(d ? d : "") + (d ? " Throw" : "Throw"), false);
            continue;
        }
        if (t == "AIR") { air = true; continue; }
        if (const char* d = DirLetter(t)) { add(d, false); continue; }
        add(Words(t), false);
    }
    return out.empty() ? name : out;
}
}
