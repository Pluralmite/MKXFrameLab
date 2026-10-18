// MKXFrameLab - the game's own move-list frame data (the numbers shown in the in-game move list),
// read from memory so measured moves can be checked against / labelled with the official values.
#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace movelist {
struct Entry {
    std::string name;      // internal attack name, e.g. MN_ATTACK2TOWARDS_1_3
    int type = 0;          // 1 high, 2 mid, 3 low, 5 throw, 6 overhead
    float damage = 0, blockDamage = 0;   // fractions of total health
    int startup = 0, active = 0, recovery = 0, blockAdv = 0, hitAdv = 0, cancel = 0;
    int category = 1;      // 1 basic attacks / strings, 2 special moves (others: character power ...)
    int flags = 0;
    std::string display;   // MKX: move-list style name / notation (Ball BF, B1,3, J2)
    uint32_t hash = 0;     // MKX attack hash (AIAttackProps key)
    int player = 0;
};
void SetPlayers(uintptr_t p1Obj, uintptr_t p2Obj);   // fighters whose attack tables the scan reads
bool ByHash(uint32_t hash, Entry& out);
void LearnName(uint32_t hash, const std::string& name);   // name an unlisted move from the string it chains into
void RequestScan();                                   // start a background scan (no-op if one is running)
bool Scanning();
int Generation();                                     // bumps after every finished scan
std::map<std::string, Entry> Table();                 // name -> entry (all characters currently loaded)
const char* TypeName(int t);
// fight-pad notation for a move-list name: ATTACK3AWAY -> B3, ATTACK1AWAY_1_3UP -> B1,1,U3, AIR_ATTACK2 -> J2
std::string Notation(const std::string& name);
}
