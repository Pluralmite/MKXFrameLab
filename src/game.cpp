#include "game.h"
#include "bot.h"
#include "config.h"
#include "memory.h"
#include "boxes.h"
#include "mkx.h"
#include <windows.h>
#include <algorithm>
#include "log.h"
#include "movelist.h"
#include "buffer.h"
#include <xinput.h>

GameState g_game;

static const size_t kHistory = 120;

const char* FStateName(FState s) {
    switch (s) {
    case FState::Idle: return "Idle";
    case FState::Startup: return "Startup";
    case FState::Active: return "Active";
    case FState::Recovery: return "Recovery";
    case FState::Hitstun: return "Stun/Busy";
    case FState::Blockstun: return "Blockstun";
    default: return "-";
    }
}

static bool ReadI(const mem::ExprPtr& e, const mem::EvalCtx& c, int& out) {
    if (!e) return false;
    uintptr_t a = e->Eval(c);
    return a && mem::Read(a, out);
}
static bool ReadF(const mem::ExprPtr& e, const mem::EvalCtx& c, float& out) {
    if (!e) return false;
    uintptr_t a = e->Eval(c);
    return a && mem::Read(a, out);
}

static void SamplePlayer(int idx, const mem::EvalCtx& base) {
    PlayerSample& s = g_game.p[idx];
    s = PlayerSample();
    mem::EvalCtx c = base;
    c.self = idx == 0 ? base.p1 : base.p2;
    s.object = c.self;
    if (!c.self) return;
    s.valid = true;
    ReadF(g_cfg.posX, c, s.pos[0]);
    ReadF(g_cfg.posY, c, s.pos[1]);
    ReadF(g_cfg.posZ, c, s.pos[2]);
    s.hasState = ReadI(g_cfg.state, c, s.state);
    s.hasMoveFrame = ReadI(g_cfg.moveFrame, c, s.moveFrame);
    ReadI(g_cfg.hitstun, c, s.hitstun);
    ReadI(g_cfg.blockstun, c, s.blockstun);

    int v = 0;
    if (ReadI(g_cfg.actionable, c, v)) { s.actionable = v != 0; s.hasActionable = true; }
    else if (s.hasState && !g_cfg.idleStates.empty()) {
        s.actionable = std::find(g_cfg.idleStates.begin(), g_cfg.idleStates.end(), s.state) != g_cfg.idleStates.end();
        s.hasActionable = true;
    }
    if (ReadI(g_cfg.attackActive, c, v)) { s.attackActive = v != 0; s.hasActive = true; }
    else if (BoxesPlayerHasActiveHit(idx + 1, base)) { s.attackActive = true; s.hasActive = true; }
}

static void Classify(int idx) {
    PlayerSample& s = g_game.p[idx];
    PlayerMeter& m = g_game.meter[idx];
    if (!s.valid) { s.fstate = FState::None; return; }
    // A new move starts with no active frames seen yet; without this the first
    // startup frame would be misread as recovery from the previous move.
    if (!m.inMove) m.seenActive = false;

    if (s.hitstun > 0) s.fstate = FState::Hitstun;
    else if (s.blockstun > 0) s.fstate = FState::Blockstun;
    else if (s.attackActive) s.fstate = FState::Active;
    else if (!s.actionable) s.fstate = m.seenActive ? FState::Recovery : FState::Startup;
    else s.fstate = FState::Idle;

    bool attacking = s.fstate == FState::Startup || s.fstate == FState::Active || s.fstate == FState::Recovery;
    if (attacking) {
        if (!m.inMove) {
            m.inMove = true; m.seenActive = false;
            m.moveFrames = m.activeFrames = m.recoveryFrames = m.startupFrames = 0;
        }
        m.moveFrames++;
        if (s.fstate == FState::Active) {
            if (!m.seenActive) m.startupFrames = m.moveFrames; // first active frame number
            m.seenActive = true;
            m.activeFrames++;
        } else if (s.fstate == FState::Recovery) {
            m.recoveryFrames++;
        }
    } else if (m.inMove) {
        m.inMove = false;
        m.last.valid = m.seenActive;
        m.last.startup = m.startupFrames;
        m.last.active = m.activeFrames;
        m.last.recovery = m.recoveryFrames;
        m.last.total = m.moveFrames;
    }

    bool act = s.fstate == FState::Idle;
    if (act && !m.wasActionable) m.actionableSince = g_game.logicalFrame;
    m.wasActionable = act;

    m.history.push_back(s.fstate);
    while (m.history.size() > kHistory) m.history.pop_front();
}

// ---------------------------------------------------------------------------------------------
// Advantage, measured per "episode": from the moment the defender gets stunned until it is free.
//   advantage = defender free frame - attacker's FIRST free frame after the stun began
// Knockdown (defender flags with 0x8 = grounded): the game's frame data counts up to the
// wake-up start (0x6202), so KD advantage = (wake-up start - 1) - attacker free.
// Delayed wake-up only lengthens the grounded phase: delay = grounded frames - shortest seen
// for that move, reported separately.
// Short gaps (<=3 frames of 0 flags) inside a stun (knockdown -> wake-up) are not "free".
std::map<uint64_t, int> g_minGround;   // per (character, move id, pre-ground hitstun): shortest grounded time (no delay)
// ---- persistent per-character move cache (MKXFrameLab_movecache.txt next to the mod) ----
static uint32_t g_charKey[2] = {0, 0};
static std::map<uint64_t, MoveStats> g_clean;          // (charKey << 32 | moveId) -> clean whiff/block stats
static std::string CachePath() { return g_cfg.baseDir + "MKXFrameLab_movecache.txt"; }
static std::map<uint32_t, std::map<std::string, int>> g_prefixVotes;   // character key -> prefix -> votes
static void CacheLoad() {
    static bool loaded = false;
    if (loaded) return;
    loaded = true;
    FILE* f = fopen(CachePath().c_str(), "r");
    if (!f) return;
    char t;
    unsigned long long k;
    int a, b, c, d, e;
    while (fscanf(f, " %c %llx %d %d %d %d %d", &t, &k, &a, &b, &c, &d, &e) >= 3) {
        if (t == 'C') { MoveStats m; m.valid = true; m.startup = a; m.active = b; m.recovery = c; m.total = d; g_clean[k] = m; }
        else if (t == 'G') { g_minGround[k] = a; }
        else if (t == 'P') { char pre[32] = {}; int ch2; (void)ch2; if (fscanf(f, " %31s", pre) == 1) g_prefixVotes[(uint32_t)k][pre] += 1000; }
        int ch; while ((ch = fgetc(f)) != EOF && ch != '\n') {}
    }
    fclose(f);
    Log("movecache: %d moves loaded", (int)g_clean.size());
}
static void CacheAppend(char t, uint64_t k, int a, int b = 0, int c = 0, int d = 0) {
    FILE* f = fopen(CachePath().c_str(), "a");
    if (!f) return;
    fprintf(f, "%c %llx %d %d %d %d 0\n", t, (unsigned long long)k, a, b, c, d);
    fclose(f);
}


// ---------------------------------------------------------------------------------------------
struct Episode {
    bool active = false;
    uint64_t start = 0;
    bool blocked = false, kd = false;
    int grounded = 0;
    uint64_t wakeStart = 0;
    bool attackerFreeKnown = false;
    uint64_t attackerFree = 0;
    bool freePending = false;
    uint64_t freeAt = 0;
    int freeRun = 0;
    int attackerMove = -1;
    int preGround = 0;        // hitstun frames before landing - identifies the knockdown together with the move id
    bool cinematic = false;   // throw / super cinematic (0x10000) - no advantage reported
    bool reported = false;    // knockdown result already shown (at wake-up start)
    bool savedFreeKnown = false;   // attacker's free frame before a new move started mid-interaction: if that move
    uint64_t savedFree = 0;        // does not connect before the knockdown (e.g. a meaty / trap), it stays the reference
    std::vector<int> gaps;    // defender free frames between consecutive stuns (string / multi-hit gaps)
    // frame kill: something the attacker does while the defender is still on the ground (trap, whiffed move,
    // dash) that never touches them moves the reference point to the end of that action
    bool fkBusy = false, fkFreeKnown = false;
    uint64_t fkFree = 0;
    int fkMove = -1;
};
static Episode g_ep[2];                  // indexed by defender
static bool g_needContact[2];            // a knockdown was closed early by a new attack: next episode needs real contact
static std::string g_flagLog[2];
static const int kGapTolerance = 3;

static void ReportEpisode(int df) {
    Episode& E = g_ep[df];
    int at = 1 - df;
    PlayerMeter& A = g_game.meter[at];
    E.reported = true;
    A.liveCell = -1;
    if (!A.attacked) return;
    int adv;
    int delay = 0;
    int kdBase = 0;
    bool frameKill = false;
    if (E.kd && E.wakeStart) {
        adv = (int)((int64_t)E.wakeStart - 1 - (int64_t)E.attackerFree);
        if (E.fkFreeKnown && E.fkFree > E.attackerFree) {   // advantage left after the follow-up action
            kdBase = adv;
            adv = (int)((int64_t)E.wakeStart - 1 - (int64_t)E.fkFree);
            frameKill = true;
        }
        uint64_t key = (((uint64_t)(uint32_t)A.moveId << 32) | (uint32_t)E.preGround) ^ ((uint64_t)g_charKey[at] * 0x9E3779B97F4A7C15ull);
        CacheLoad();
        int& mg = g_minGround[key];
        if (mg == 0 || E.grounded < mg) { mg = E.grounded; CacheAppend('G', key, mg); }
        delay = E.grounded - mg;
        adv -= delay;
        kdBase -= delay;
    } else {
        adv = (int)((int64_t)E.freeAt - (int64_t)E.attackerFree);
    }
    bool blocked = E.blocked;
    A.gaps = E.gaps;
    A.gapsBlocked = blocked;
    if (!E.gaps.empty()) {
        std::string g;
        for (int x : E.gaps) g += " " + std::to_string(x);
        Log("gaps: P%d %s string gaps (free frames):%s", at + 1, blocked ? "BLOCK" : "HIT", g.c_str());
    }
    // only the result of the latest interaction is shown: a blocked one clears the hit value and vice versa
    if (blocked) { A.blockAdv = adv; A.blockValid = true; A.hitValid = false; }
    else {
        A.hitAdv = adv; A.hitValid = true; A.hitKD = E.kd; A.kdDelay = delay; A.blockValid = false;
        A.kdFrameKill = frameKill; A.kdBase = kdBase; A.kdAfter.clear();
        if (frameKill) {
            movelist::Entry me;
            if (E.fkMove != -1 && movelist::ByHash((uint32_t)E.fkMove, me) && !me.display.empty()) A.kdAfter = me.display;
            Log("adv: P%d knockdown %+d, after %s %+d (frame kill)", at + 1, kdBase, A.kdAfter.empty() ? "follow-up" : A.kdAfter.c_str(), adv);
        }
    }
    g_game.advValid = true;
    g_game.advantageP1 = at == 0 ? adv : -adv;
    Log("adv: P%d %s%s%s %+d (delay %d, grounded %d, preground %d, attacker free @%llu, defender free @%llu, wake @%llu)%s", at + 1,
        blocked ? "BLOCK" : "HIT", E.kd ? " KD" : "", E.cinematic ? " CINEMATIC" : "", adv, delay, E.grounded, E.preGround, (unsigned long long)E.attackerFree,
        (unsigned long long)E.freeAt, (unsigned long long)E.wakeStart, A.canceled ? " canceled" : "");
    if (g_logVerbose) {
        Log("  P%d flags: %s", at + 1, g_flagLog[at].c_str());
        Log("  P%d flags: %s", df + 1, g_flagLog[df].c_str());
    }

    // move cache (for CANCEL) + cancel advantage
    auto& cache = g_game.moveCache[at];
    if (!A.canceled) {
        if (A.firstMoveId >= 0 && A.firstMove.valid) {
            MoveCacheEntry& e = cache[A.firstMoveId];
            e.total = A.firstMove.total;
            if (blocked) { e.blockAdv = adv; e.hasBlock = true; } else { e.hitAdv = adv; e.hasHit = true; }
        }
    } else {
        auto it = cache.find(A.cancelFrom);
        bool have = it != cache.end() && it->second.total > 0 && (blocked ? it->second.hasBlock : it->second.hasHit);
        if (have) {
            const MoveCacheEntry& e = it->second;
            A.cancelAdv = (blocked ? e.blockAdv : e.hitAdv) + (e.total - A.cancelFrame);
            A.cancelValid = true; A.cancelUnknown = false;
        } else { A.cancelValid = false; A.cancelUnknown = true; }
    }
    A.attacked = false;
    A.canceled = false;
    A.firstMoveId = -1;
    A.firstMove = MoveStats();
}

static void FinishEpisode(int df) {
    Episode& E = g_ep[df];
    if (!E.reported) ReportEpisode(df);
    E.active = false;
    g_game.meter[1 - df].liveCell = -1;
}

static void UpdateAdvantage() {
    if (!g_game.p[0].valid || !g_game.p[1].valid) return;
    uint64_t now = g_game.logicalFrame;
    for (int df = 0; df < 2; ++df) {
        int at = 1 - df;
        Episode& E = g_ep[df];
        const PlayerSample& D = g_game.p[df];
        PlayerMeter& A = g_game.meter[at];
        bool dStunned = D.fstate == FState::Hitstun || D.fstate == FState::Blockstun;
        bool aFree = A.wasActionable;

        if (!E.active) {
            // (a move already running when the previous knockdown was reported - a meaty - still counts)
            if (!dStunned || !(A.attacked || (g_needContact[df] && A.inMove))) continue;
            // right after a knockdown the defender still reads as stunned while getting up: only a real
            // contact (not a meaty whiff) opens the next interaction
            // (the wake-up animation, 0x2000 states, is invulnerable: a projectile or trap overlapping it is not a hit)
            if (g_needContact[df] && (!A.contactThisMove || (D.state & 0x2000))) continue;
            if (g_needContact[df]) A.attacked = true;   // the meaty move started before the knockdown result cleared it
            g_needContact[df] = false;
            E = Episode();
            E.active = true;
            E.start = now;
            E.attackerMove = A.moveId;
            g_flagLog[0].clear(); g_flagLog[1].clear();
            if (aFree) { E.attackerFreeKnown = true; E.attackerFree = A.actionableSince; }   // e.g. projectile hit after recovery
        }
        // per-frame bookkeeping
        for (int i = 0; i < 2; ++i) {
            if (g_flagLog[i].size() < 6000) {
                char t[16];
                const char* st = "-";
                switch (g_game.p[i].fstate) {
                case FState::Startup: st = "S"; break; case FState::Active: st = "A"; break; case FState::Recovery: st = "R"; break;
                case FState::Hitstun: st = "H"; break; case FState::Blockstun: st = "B"; break; case FState::Idle: st = "."; break; default: break;
                }
                snprintf(t, sizeof(t), "%s%X ", st, (unsigned)g_game.p[i].state);
                g_flagLog[i] += t;
            }
        }
        if (D.fstate == FState::Blockstun) E.blocked = true;
        bool grounded = (D.state & 0x8) && (D.state & 0x4);
        if ((D.state & 0x10000) || (g_game.p[at].state & 0x10000)) E.cinematic = true;
        if (grounded) { E.kd = true; E.grounded++; E.wakeStart = 0; }
        else if (!E.kd && dStunned) E.preGround++;
        else if (E.kd && E.grounded > 0 && E.wakeStart == 0 && D.state != 0) E.wakeStart = now;   // first wake-up frame
        {   // frame kill tracking (see Episode::fkBusy)
            FState af = g_game.p[at].fstate;
            if (E.kd && !E.reported && E.attackerFreeKnown && !E.wakeStart && af == FState::Startup && !E.fkBusy) {
                E.fkBusy = true; E.fkFreeKnown = false; E.fkMove = A.moveId;
            }
            // it connected with a standing defender: a meaty, not a frame kill (touching them on the ground or
            // during the invulnerable wake-up does nothing)
            if (E.fkBusy && A.contactNow && !(D.state & 0x8) && !(D.state & 0x2000)) { E.fkBusy = false; E.fkFreeKnown = false; }
            else if (E.fkBusy && aFree && af != FState::Startup) { E.fkBusy = false; E.fkFreeKnown = true; E.fkFree = A.actionableSince; }
        }
        {
            FState af = g_game.p[at].fstate;
            // combo / string continues: the attacker's free frame has to come after their last move
            // (not once the defender is on the ground: a meaty / OTG attempt during the knockdown must not
            //  move the knockdown's reference point)
            if (!E.reported && !E.kd && (af == FState::Startup || af == FState::Active)) {
                if (E.attackerFreeKnown) { E.savedFreeKnown = true; E.savedFree = E.attackerFree; }
                E.attackerFreeKnown = false;
            }
            // the new move connected while the defender was still in the air / in hitstun: a real combo continuation
            if (!E.kd && A.contactThisMove && (af == FState::Startup || af == FState::Active || af == FState::Recovery)) E.savedFreeKnown = false;
            // knocked down and getting up while the attacker is busy with something that never touched them
            // (meaty projectile, trap, whiffed follow-up): the knockdown counts from when they were free before it
            if (E.kd && E.wakeStart && !E.reported && !E.attackerFreeKnown && E.savedFreeKnown) {
                E.attackerFreeKnown = true;
                E.attackerFree = E.savedFree;
            }
        }
        if (!E.attackerFreeKnown && aFree) { E.attackerFreeKnown = true; E.attackerFree = A.actionableSince; }

        // knockdown: the result is final as soon as the defender starts getting up (first non-grounded frame)
        // (a follow-up still running at the wake-up is waited for: the result is how far behind / ahead it leaves the attacker)
        if (E.kd && E.wakeStart && !E.reported && E.attackerFreeKnown && !E.fkBusy) ReportEpisode(df);
        // knockdown already reported and the attacker starts something new while the defender is still getting
        // up: close it now, so the next combo / string gets its own result instead of being swallowed
        if (E.reported && E.kd && E.wakeStart) {
            FState af = g_game.p[at].fstate;
            if (af == FState::Startup || af == FState::Active) { FinishEpisode(df); g_needContact[df] = true; continue; }
        }
        if (dStunned) {
            if (E.freePending && E.freeRun > 0) E.gaps.push_back(E.freeRun);   // defender was free, then got hit / blocked again
            E.freePending = false;
            E.freeRun = 0;
            A.liveKD = E.kd;
            if (E.attackerFreeKnown && !E.reported) { A.liveCell = E.blocked ? 1 : 0; A.liveAdv = (int)(now - E.attackerFree) + 1; }
        } else {
            // the attacker was free, then started something new (buffered special, whiffed follow-up) that has
            // not touched the defender: the advantage counts from that earlier free frame
            if (!E.attackerFreeKnown && E.savedFreeKnown && !E.kd && !A.contactThisMove) { E.attackerFreeKnown = true; E.attackerFree = E.savedFree; }
            if (!E.freePending) { E.freePending = true; E.freeAt = now; E.freeRun = 0; }
            E.freeRun++;
            // a new hit from the attacker restarts naturally (dStunned again); short gaps are ignored
            // both fighters free at the same time = the interaction is over (a new attack after that is a new one)
            // ...unless the attacker still has a projectile on its way (Sektor's Lock-On Missiles after a string):
            // if it lands, the defender's free frames are a gap and the result is the advantage after it
            if (E.attackerFreeKnown && aFree && !A.projActive && !E.fkBusy) FinishEpisode(df);
            else if (E.freeRun > kGapTolerance && E.attackerFreeKnown && !A.projActive && !E.fkBusy) FinishEpisode(df);
            else if (E.freeRun > 240) FinishEpisode(df);
        }
    }
}

// ---- demo: scripted pattern so the overlay can be checked with no offsets
static void DemoSample() {
    static const struct { FState s; int n; } p1seq[] = {
        {FState::Idle, 20}, {FState::Startup, 9}, {FState::Active, 3}, {FState::Recovery, 20}, {FState::Idle, 30}};
    static const struct { FState s; int n; } p2seq[] = {
        {FState::Idle, 29}, {FState::Blockstun, 19}, {FState::Idle, 34}};
    auto pick = [](auto& seq, int len, uint64_t f) {
        int total = 0;
        for (int i = 0; i < len; ++i) total += seq[i].n;
        int t = (int)(f % total);
        for (int i = 0; i < len; ++i) { if (t < seq[i].n) return seq[i].s; t -= seq[i].n; }
        return FState::Idle;
    };
    for (int i = 0; i < 2; ++i) {
        PlayerSample& s = g_game.p[i];
        s = PlayerSample();
        s.valid = true;
        FState st = i == 0 ? pick(p1seq, 5, g_game.logicalFrame) : pick(p2seq, 3, g_game.logicalFrame);
        s.attackActive = st == FState::Active;
        s.actionable = st == FState::Idle;
        s.blockstun = st == FState::Blockstun ? 1 : 0;
        Classify(i);
    }
    UpdateAdvantage();
}

// ---- native Mortal Kombat X path
static int g_prevMelee[2] = {-1, -1};
static uintptr_t g_prevMeleePtr[2] = {0, 0};
static uint64_t g_lastGameTick = 0;

// ---- official move-list data -------------------------------------------------------------------------
// Normal attacks have move ids (button group << 16) | slot; group 0xF/0x10/0x11/0x12 = buttons 1-4 and the
// slot picks the direction variant. Names in the game's table: <CHAR>_ATTACK<button><AWAY|TOWARDS|DOWN|UP>.
static std::map<std::string, movelist::Entry> g_table;
static int g_tableGen = -1;
static std::string PrefixOf(const std::string& n) {
    size_t p = n.find('_');
    return p == std::string::npos ? std::string() : n.substr(0, p);
}
static const char* kSlotWord[] = {"", "TOWARDS", "AWAY", "UP", "DOWN"};   // slot 4 = down (d1 = 0xF0004)
static std::string BestPrefix(uint32_t ck) {
    auto it = g_prefixVotes.find(ck);
    if (it == g_prefixVotes.end()) return "";
    std::string best; int bv = 0;
    for (auto& kv : it->second) if (kv.second > bv) { bv = kv.second; best = kv.first; }
    return best;
}
static std::string g_specName[2];   // the game's own name of the special in progress (ST_LogKick ...)
static void ResolveOfficial(int idx, MoveStats& ms, int moveId, bool hit, bool vote = true) {
    ms.off = false;
    if (movelist::Generation() != g_tableGen) { g_table = movelist::Table(); g_tableGen = movelist::Generation(); }
    {   // MKX: the move id is the game's own attack hash, so the move-list entry is an exact lookup
        movelist::Entry me;
        if (ms.valid && moveId != -1 && moveId != 0 && movelist::ByHash((uint32_t)moveId, me) && me.startup > 0) {
            ms.off = true;
            ms.offName = me.name;
            ms.offType = me.type; ms.offStartup = me.startup; ms.offActive = me.active; ms.offRecovery = me.recovery;
            ms.offBlock = me.blockAdv; ms.offHit = me.hitAdv; ms.offCancel = me.cancel;
            ms.offMismatch = std::abs(ms.startup - 1 - me.startup) > 1 || (!hit && !ms.fromCache && !ms.chained && me.recovery > 0 && ms.active != me.active);
            return;
        }
    }
    if (g_table.empty() || !ms.valid) return;
    uint32_t ck = g_charKey[idx];
    if (moveId == -1) {
        // special moves (no move id): the character's special-move entries, matched on start-up, then (on a
        // whiff, when the whole move was seen) active / recovery; meter-burn versions lose ties
        // exact: the game tells us which special it is
        const std::string& gn = g_specName[idx];
        if (!gn.empty()) {
            auto it = g_table.find(gn);
            std::string gp = PrefixOf(gn);
            if (vote && !gp.empty()) g_prefixVotes[ck][gp] += 5;
            if (it != g_table.end()) {
                const movelist::Entry& e = it->second;
                ms.off = true;
                ms.offName = gn.substr(gp.size() + 1);
                ms.offType = e.type; ms.offStartup = e.startup; ms.offActive = e.active; ms.offRecovery = e.recovery;
                ms.offBlock = e.blockAdv; ms.offHit = e.hitAdv; ms.offCancel = e.cancel;
                ms.offMismatch = std::abs(ms.startup - 1 - e.startup) > 1;
                return;
            }
        }
        std::string pre = BestPrefix(ck);
        if (pre.empty()) return;
        int measS = ms.startup - 1;
        const movelist::Entry* best = nullptr;
        int bestScore = 1 << 30;
        for (auto& kv : g_table) {
            const movelist::Entry& e = kv.second;
            if (e.category != 2 || e.active <= 0 || PrefixOf(kv.first) != pre) continue;
            int ds = std::abs(e.startup - measS);
            if (ds > 1) continue;
            int score = ds * 100;
            if (!hit && !ms.chained) score += std::abs(e.active - ms.active) * 4 + std::min(20, std::abs(e.recovery - (ms.recovery + 1)));
            bool mb = kv.first.size() > 2 && kv.first.find("MB") != std::string::npos;
            score = score * 2 + (mb ? 1 : 0);
            if (score < bestScore) { bestScore = score; best = &e; }
        }
        if (!best) return;
        ms.off = true;
        ms.offName = best->name.substr(pre.size() + 1);
        ms.offType = best->type; ms.offStartup = best->startup; ms.offActive = best->active; ms.offRecovery = best->recovery;
        ms.offBlock = best->blockAdv; ms.offHit = best->hitAdv; ms.offCancel = best->cancel;
        ms.offMismatch = std::abs(measS - best->startup) > 1;
        return;
    }
    int group = (moveId >> 16) & 0xFFFF, slot = moveId & 0xFFFF;
    int button = (group >= 0xF && group <= 0x12) ? group - 0xE : 0;
    int measS = ms.startup - 1;
    // 1) normals: exact name from the id; also learn which name prefix belongs to this character
    if (button && slot < 5) {
        std::string tail = std::string("_ATTACK") + char('0' + button) + kSlotWord[slot];
        // vote only when the move points at a single character: normals that share a startup between the
        // loaded characters (Hellboy f2 = Green Arrow f2 = 13f) would otherwise hand out the wrong prefix
        std::vector<std::string> pres;
        for (auto& kv : g_table) {
            const std::string& n = kv.first;
            if (n.size() > tail.size() && n.compare(n.size() - tail.size(), tail.size(), tail) == 0 &&
                n.find('_') == n.size() - tail.size() && std::abs(kv.second.startup - measS) <= 1 && vote)
                pres.push_back(PrefixOf(n));
        }
        uint32_t ok2 = g_charKey[idx ^ 1];
        if (pres.size() > 1 && ok2 && ok2 != ck) {   // the other player's (established) character is not this one
            std::string op = BestPrefix(ok2);
            if (!op.empty() && g_prefixVotes[ok2][op] >= 3) pres.erase(std::remove(pres.begin(), pres.end(), op), pres.end());
        }
        if (pres.size() == 1) g_prefixVotes[ck][pres[0]]++;
    }
    std::string pre = BestPrefix(ck);
    {
        static std::string loggedPre[2];
        static std::map<uint32_t, std::string> saved;
        if (pre != loggedPre[idx]) { loggedPre[idx] = pre; Log("movelist: P%d character prefix %s", idx + 1, pre.c_str()); }
        if (!pre.empty() && saved[ck] != pre && g_prefixVotes[ck][pre] >= 3) {
            saved[ck] = pre;
            FILE* f = fopen(CachePath().c_str(), "a");
            if (f) { fprintf(f, "P %x 0 0 0 0 0 %s\n", ck, pre.c_str()); fclose(f); }
        }
    }
    const movelist::Entry* e = nullptr;
    if (!pre.empty() && button && slot < 5) {
        auto it = g_table.find(pre + "_ATTACK" + char('0' + button) + kSlotWord[slot]);
        if (it != g_table.end()) e = &it->second;
    }
    // 2) strings / anything else: unique match on startup (+ active when not cut short by a hit) within the
    //    character's entries whose last button is the same button
    if (!e && !pre.empty()) {
        const movelist::Entry* cand = nullptr;
        int n = 0;
        for (auto& kv : g_table) {
            const std::string& nm = kv.first;
            if (kv.second.category != 1) continue;   // specials are matched separately
            if (PrefixOf(nm) != pre) continue;
            if (kv.second.startup != measS) continue;
            if (!hit && kv.second.active != ms.active) continue;
            if (button) {
                char last = 0;
                for (char c : nm) if (c >= '1' && c <= '4') last = c;
                if (last && last != char('0' + button)) continue;
            }
            cand = &kv.second; ++n;
        }
        if (n == 1) e = cand;
    }
    if (!e) return;
    ms.off = true;
    ms.offName = e->name.substr(pre.size() + 1);
    ms.offType = e->type; ms.offStartup = e->startup; ms.offActive = e->active; ms.offRecovery = e->recovery;
    ms.offBlock = e->blockAdv; ms.offHit = e->hitAdv; ms.offCancel = e->cancel;
    ms.offMismatch = std::abs(measS - e->startup) > 1 || (!hit && !ms.fromCache && (ms.active != e->active || std::abs(ms.recovery + 1 - e->recovery) > 1));
}

// Identify a hit as early as possible so its official active window can separate it from the next string hit.
static const movelist::Entry* FindEntry(const std::string& full) {
    auto it = g_table.find(full);
    return it == g_table.end() ? nullptr : &it->second;
}
static std::string NormalName(int idx, int moveId) {
    CacheLoad();
    int group = (moveId >> 16) & 0xFFFF, slot = moveId & 0xFFFF;
    int button = (group >= 0xF && group <= 0x12) ? group - 0xE : 0;
    std::string pre = BestPrefix(g_charKey[idx]);
    if (!button || slot >= 5 || pre.empty()) return "";
    std::string n = pre + "_ATTACK" + char('0' + button) + kSlotWord[slot];
    return FindEntry(n) ? n : "";
}
// a follow-up hit of a string: <previous>_<button><dir>, chosen by its measured startup; ties broken by the
// button last pressed on the pad (X=1 Y=2 A=3 B=4) and, at the end of the hit, by active / recovery
static std::vector<std::string> FollowUps(const std::string& prev, int measStartup) {
    std::vector<std::string> out;
    if (prev.empty()) return out;
    for (auto& kv : g_table) {
        const std::string& n = kv.first;
        if (kv.second.category != 1) continue;
        if (n.size() <= prev.size() + 1 || n.compare(0, prev.size() + 1, prev + "_") != 0) continue;
        if (n.find('_', prev.size() + 1) != std::string::npos) continue;   // direct follow-up only
        if (std::abs(kv.second.startup - measStartup) <= 1) out.push_back(n);
    }
    return out;
}
static std::string FollowUpName(const std::string& prev, int measStartup, uint32_t pressNo) {
    std::vector<std::string> c = FollowUps(prev, measStartup);
    if (c.size() <= 1) return c.empty() ? "" : c[0];
    uint16_t b = 0, d = 0;
    if (!ibuf::GetPress(pressNo, b, d)) ibuf::LastPress(b, d);
    char digit = (b & XINPUT_GAMEPAD_X) ? '1' : (b & XINPUT_GAMEPAD_Y) ? '2' : (b & XINPUT_GAMEPAD_A) ? '3' : (b & XINPUT_GAMEPAD_B) ? '4' : 0;
    bool down = d & XINPUT_GAMEPAD_DPAD_DOWN, up = d & XINPUT_GAMEPAD_DPAD_UP;
    for (auto& n : c) {
        std::string tok = n.substr(prev.size() + 1);
        if (tok.empty() || tok[0] != digit) continue;
        bool tDown = tok.find("DOWN") != std::string::npos, tUp = tok.find("UP") != std::string::npos;
        if (tDown == down && tUp == up) return n;
    }
    for (auto& n : c) if (n.size() > prev.size() + 1 && n[prev.size() + 1] == digit) return n;
    return c[0];
}


// Store the stats of the move that just ended (or was chained / canceled into the next one).
static void FinalizeMove(int idx, PlayerMeter& m, bool chained) {
    m.last.valid = m.seenActive;
    m.last.startup = m.startupFrames;
    // active = first to last active frame (moves made of several hitboxes have gaps inside their active window)
    m.last.active = m.seenActive ? m.lastActive - m.firstActive + 1 : 0;
    m.last.recovery = m.seenActive ? m.moveFrames - m.lastActive : 0;
    m.last.total = m.moveFrames;
    m.last.fromCache = false;
    m.last.chained = chained;
    if (m.firstMoveId < 0) { m.firstMoveId = m.moveId; m.firstMove = m.last; }
    // On hit the game truncates active at contact and adds hitstop to recovery; show the clean
    // numbers from a whiff/block of the same move when we have them.
    // Kept per character and saved to disk, so a move only has to be seen clean once - ever.
    CacheLoad();
    uint64_t ck = ((uint64_t)g_charKey[idx] << 32) | (uint32_t)m.moveId;
    if (!m.segName.empty() || !m.prevSegName.empty() || chained) {
        // identified string hits / chained hits: never substitute (all hits of a string share one move id)
    } else if (!m.hitThisMove && m.last.valid && m.moveId != -1 && g_charKey[idx]) {
        auto it = g_clean.find(ck);
        bool changed = it == g_clean.end() || it->second.startup != m.last.startup || it->second.active != m.last.active ||
                       it->second.recovery != m.last.recovery;
        g_clean[ck] = m.last;
        if (changed) CacheAppend('C', ck, m.last.startup, m.last.active, m.last.recovery, m.last.total);
    } else if (m.hitThisMove && m.moveId != -1) {
        auto it = g_clean.find(ck);
        if (it != g_clean.end()) { m.last = it->second; m.last.fromCache = true; m.last.chained = chained; }
    }
    bool wasHit = m.hitThisMove || m.contactThisMove;
    m.hitThisMove = false;
    m.noHitbox = !m.last.valid;
    ResolveOfficial(idx, m.last, m.moveId, wasHit);
    if (m.moveId == -1) m.segName.clear();
    if (!m.segName.empty() && !m.prevSegName.empty() && !wasHit && m.last.valid) {
        std::vector<std::string> c = FollowUps(m.prevSegName, m.last.startup - 1);
        int bestD = 1 << 30;
        std::string best = m.segName;
        for (auto& n : c) {
            const movelist::Entry* e = FindEntry(n);
            int d = std::abs(e->active - m.last.active) * 4 + (chained ? 0 : std::abs(e->recovery - (m.last.recovery + 1)));
            if (d < bestD) { bestD = d; best = n; }
        }
        m.segName = best;
    }
    if (!m.segName.empty()) {
        if (const movelist::Entry* e = FindEntry(m.segName)) {
            std::string pre = BestPrefix(g_charKey[idx]);
            MoveStats& ms = m.last;
            ms.off = true;
            ms.offName = m.segName.size() > pre.size() + 1 ? m.segName.substr(pre.size() + 1) : m.segName;
            ms.offType = e->type; ms.offStartup = e->startup; ms.offActive = e->active; ms.offRecovery = e->recovery;
            ms.offBlock = e->blockAdv; ms.offHit = e->hitAdv; ms.offCancel = e->cancel;
            int measS = ms.startup - 1;
            ms.offMismatch = std::abs(measS - e->startup) > 1 || (!wasHit && !ms.fromCache && (ms.active != e->active ||
                             (!chained && std::abs(ms.recovery + 1 - e->recovery) > 1)));
        }
    }
    m.prevSegName = m.segName;
    char offs[160] = "";
    if (m.last.off)
        snprintf(offs, sizeof(offs), " | list %s %d/%d/%d blk %d hit %d cancel %d%s", m.last.offName.c_str(), m.last.offStartup, m.last.offActive,
                 m.last.offRecovery, m.last.offBlock, m.last.offHit, m.last.offCancel, m.last.offMismatch ? " MISMATCH" : "");
    Log("move: P%d id %X startup %d active %d recovery %d total %d%s%s%s", idx + 1, (unsigned)m.moveId, m.last.startup, m.last.active,
        m.last.recovery, m.last.total, chained ? " (chained)" : "", wasHit ? " (hit)" : "", offs);
}

// MY_STATES a fighter can act from: neutral, crouching, holding block (stand / crouch), walking.
// (0x800 alone = holding block; real blockstun is 0x4A00 / 0x4B00, i.e. with the 0x4000 reaction bit.)
static bool Neutral(uint32_t st) {
    switch (st) {
    case 0: case 0x100: case 0x101: case 0x800: case 0x901: case 0xA01: case 0xB00:
    case 0x2000: case 0x2001: case 0x2002: case 0x200C: case 0x200D: return true;
    default: return false;
    }
}

static void ClassifyGame(int idx, const mkx::PlayerSnap& ps) {
    PlayerSample& s = g_game.p[idx];
    s = PlayerSample();
    s.valid = ps.valid;
    s.object = ps.obj;
    s.pos[0] = ps.pos.x; s.pos[1] = ps.pos.y; s.pos[2] = ps.pos.z;
    s.state = (int)ps.stateFlags; s.hasState = true;
    s.moveFrame = ps.meleeState; s.hasMoveFrame = true;
    s.actionable = Neutral(ps.stateFlags); s.hasActionable = true;
    s.moveId = ps.moveId;
    PlayerMeter& m = g_game.meter[idx];
    if (!ps.valid) { s.fstate = FState::None; return; }

    // Knockdowns: 0x2000 = airborne, 0x4 = hit reaction -> 0x2004/0x2005 = knocked down; 0x100 = get-up.
    // (0x2001 alone is just a jump.) Between knockdown and get-up the flags read 0 for a couple
    // of frames - that is NOT free to act.
    bool kdForceIdleNow = false;
    if (false) m.inKnockdown = true;   // knockdown gaps are handled by the advantage episode logic
    if (m.inKnockdown) {
        if (ps.stateFlags & 0x100) m.sawGetup = true;
        if ((ps.stateFlags & 0x2000) && (ps.stateFlags & 0x1)) m.groundFrames++;
        if (ps.stateFlags == 0) {
            m.zeroRun++;
            if (m.sawGetup) { m.inKnockdown = false; m.sawGetup = false; m.zeroRun = 0; }
            else if (m.zeroRun >= 6) { m.inKnockdown = false; m.zeroRun = 0; kdForceIdleNow = true; } // no get-up phase seen
        } else {
            m.zeroRun = 0;
        }
    }
    // ---- attack state from this fighter's own attack objects (melee hitboxes, shaped hitboxes, projectiles) ----
    // state: 0 = startup, 1 = active, 2 = finished, 4 = connected
    bool attackFlag = (ps.stateFlags & 0x1000) != 0;
    bool anyActive = false, newObj = false;
    {
        std::map<uintptr_t, int> cur;
        for (auto& h : ps.hitObjs) {
            auto it = m.objState.find(h.first);
            int prevSt = it == m.objState.end() ? -99 : it->second;
            bool fresh = (prevSt == -99 && (h.second == 0 || h.second == 1 || (h.second == 4 && attackFlag))) ||
                         ((prevSt == 2 || prevSt == 4) && (h.second == 0 || h.second == 1));   // pooled object re-used
            if (fresh && attackFlag) newObj = true;
            if (h.second == 1) anyActive = true;
            // 4 = finished. Straight from startup (0) or brand new = it connected on its very first active frame;
            // from 1 it simply ended (with or without a hit) and this frame is no longer active.
            if (h.second == 4 && (prevSt == 0 || prevSt == -99) && attackFlag) anyActive = true;
            cur[h.first] = h.second;
        }
        m.objState.swap(cur);
    }
    if (ps.striking || (ps.contactNew && attackFlag)) anyActive = true;

    // a projectile / trap of this fighter connecting while they are not attacking (e.g. Sektor's Up Missile
    // landing after he has recovered): that is a new interaction of its own
    if (ps.contactNew && !attackFlag) m.attacked = true;
    m.contactNow = ps.contactNew;
    m.projActive = ps.projActive && !attackFlag;   // free to act, but something of theirs is still flying

    // MKX: the game's attack hash changes on the exact frame the next move starts. If the previous move had
    // run its full official length (start-up + active + recover from the game's move list), the new one was
    // buffered and comes out on the first free frame: that frame is when the attacker became free. Before
    // that it is a cancel / the next hit of a string.
    bool naturalFree = false;
    if (m.inMove && m.seenActive && attackFlag && ps.moveId != -1 && m.moveId != -1 && ps.moveId != m.moveId) {
        movelist::Entry pe;
        bool natural = false;
        if (movelist::ByHash((uint32_t)m.moveId, pe) && pe.startup > 0 && pe.recovery > 0) {
            int T = pe.startup - 1 + pe.active + pe.recovery;   // the move's total frames (Kano 1: 7 + 3 + 15 = 25)
            natural = m.moveFrames >= T;
        }
        if (!natural && !m.canceled) { m.canceled = true; m.cancelFrom = m.moveId; m.cancelFrame = m.moveFrames; }
        if (!natural) {
            movelist::Entry ne;
            bool haveNext = movelist::ByHash((uint32_t)ps.moveId, ne);
            // an unlisted first hit chaining into a listed string: it is that string minus its last press
            if (pe.display.empty() && haveNext) {
                size_t c = ne.display.rfind(',');
                if (c != std::string::npos && c > 0) movelist::LearnName((uint32_t)m.moveId, ne.display.substr(0, c));
            }
            // a listed string chaining into an unlisted hit (an ender kept outside the moveset tables, e.g. Sektor's
            // F2,1,2,B4): it is that string plus the press that brought it out
            if (idx == 0 && !pe.display.empty() && haveNext && ne.display.empty() && ne.category != 2) {
                uint16_t btn = 0, dirs = 0;
                ibuf::LastPress(btn, dirs);
                const char* b = (btn & 0x4000) ? "1" : (btn & 0x8000) ? "2" : (btn & 0x1000) ? "3" : (btn & 0x2000) ? "4" : nullptr;
                if (b) {
                    bool right = bot::FacingRight();
                    bool up = dirs & 1, down = dirs & 2, l = dirs & 4, r = dirs & 8;
                    bool fwd = right ? r : l, back = right ? l : r;
                    std::string d = std::string(down ? "D" : up ? "U" : "") + (fwd ? "F" : back ? "B" : "");
                    movelist::LearnName((uint32_t)ps.moveId, pe.display + "," + d + b);
                }
            }
        }
        FinalizeMove(idx, m, !natural);
        m.seenActive = false; m.hitThisMove = false; m.contactThisMove = false;
        m.moveFrames = m.activeFrames = m.recoveryFrames = m.startupFrames = 0;
        m.moveId = ps.moveId;
        m.segName.clear();
        m.segWindowEnd = -1;
        m.sinceActive = 0;
        m.prevSegName.clear();
        m.pressAtStart = ibuf::PressSeq();
        m.segIndex = natural ? 0 : m.segIndex + 1;
        if (natural) { naturalFree = true; m.attacked = true; }
        newObj = false;   // this frame's new hitbox belongs to the new move
    }

    // a new hitbox after this hit's active frames = the next hit of a string / a cancel. With the move-list
    // window known this is exact; otherwise require a few non-active frames in between (multi-part hitboxes).
    if (m.inMove && m.seenActive && newObj && attackFlag) {
        bool split = m.segWindowEnd > 0 ? (m.moveFrames >= m.segWindowEnd) : (m.sinceActive >= 3);
        if (split) {
            if (!m.canceled) { m.canceled = true; m.cancelFrom = m.moveId; m.cancelFrame = m.moveFrames; }
            FinalizeMove(idx, m, true);
            bool newMove = ps.moveId != m.moveId;   // canceled / chained into a different move (jump-in -> string, normal -> special)
            m.seenActive = false; m.hitThisMove = false; m.contactThisMove = false;
            m.moveFrames = m.activeFrames = m.recoveryFrames = m.startupFrames = 0;
            m.moveId = ps.moveId;
            m.segName.clear();
            m.segWindowEnd = -1;
            m.sinceActive = 0;
            m.segIndex++;
            if (newMove) {
                // a fresh move, not the next hit of the same string: identify it from its own id
                m.prevSegName.clear();
                m.pressAtStart = ibuf::PressSeq();
                m.segIndex = 0;
                m.segName = NormalName(idx, ps.moveId);
                const movelist::Entry* e = m.segName.empty() ? nullptr : FindEntry(m.segName);
                m.segWindowEnd = e ? e->startup + e->active : -1;
            }
        }
    }

    // getting hit / blocking (0x4000: 0x46xx hit, 0x4Axx/0x4Bxx block, 0x62xx knockdown) always ends own move
    bool beingHit = (ps.stateFlags & 0x4000) != 0 && !attackFlag;
    if (beingHit && m.inMove) {
        m.inMove = false;
        FinalizeMove(idx, m, false);
        m.segName.clear();
        m.segWindowEnd = -1;
    }
    FState f;
    if (beingHit) f = (ps.stateFlags & 0x800) ? FState::Blockstun : FState::Hitstun;
    else if (anyActive) f = FState::Active;
    else if (Neutral(ps.stateFlags) && !m.inKnockdown) f = FState::Idle;
    else if (Neutral(ps.stateFlags)) f = FState::Hitstun;
    else if (attackFlag) f = (m.inMove && m.seenActive) ? FState::Recovery : FState::Startup;
    else if (m.inMove) f = FState::Recovery;   // e.g. landing after an attack
    else if ((ps.stateFlags & 0x4800) == 0x4800) f = FState::Blockstun;
    else f = FState::Hitstun;   // hitstun / other busy states (jump, dash, ...)
    g_prevMelee[idx] = ps.melee ? ps.meleeState : -1;
    g_prevMeleePtr[idx] = ps.melee;
    s.fstate = f;
    s.attackActive = f == FState::Active;

    bool attacking = f == FState::Startup || f == FState::Active || f == FState::Recovery;
    if (attacking && (!m.inMove || ps.moveId != -1)) g_specName[idx].clear();
    if (attacking && ps.moveId == -1 && !ps.moveName.empty()) g_specName[idx] = ps.moveName;   // latest (MB versions switch name)
    if (attacking) {
        if (!m.inMove) {
            m.inMove = true; m.seenActive = false; m.hitThisMove = false; m.contactThisMove = false;
            m.moveFrames = m.activeFrames = m.recoveryFrames = m.startupFrames = 0;
            m.moveId = ps.moveId;
            m.attacked = true;
            m.sinceActive = 0;
            m.prevSegName.clear();
            m.pressAtStart = ibuf::PressSeq();
            m.segIndex = 0;
            if (movelist::Generation() != g_tableGen) { g_table = movelist::Table(); g_tableGen = movelist::Generation(); }
            m.segName = NormalName(idx, ps.moveId);
            const movelist::Entry* e = m.segName.empty() ? nullptr : FindEntry(m.segName);
            m.segWindowEnd = e ? e->startup + e->active : -1;
        }
        m.moveFrames++;
        if (f == FState::Active) {
            if (!m.seenActive) {
                m.startupFrames = m.moveFrames;
                m.firstActive = m.moveFrames;
                if (m.segName.empty() && !m.prevSegName.empty() && ps.moveId != -1) {   // string follow-up (specials have id -1)
                    m.segName = FollowUpName(m.prevSegName, m.startupFrames - 1, m.pressAtStart + (uint32_t)m.segIndex);
                    const movelist::Entry* e = m.segName.empty() ? nullptr : FindEntry(m.segName);
                    m.segWindowEnd = e ? e->startup + e->active : -1;
                }
                // show the official data right away instead of when the move has fully recovered
                m.cur = MoveStats();
                m.cur.valid = true;
                m.cur.startup = m.startupFrames;
                const movelist::Entry* ce = m.segName.empty() ? nullptr : FindEntry(m.segName);
                std::string cpre = BestPrefix(g_charKey[idx]);
                if (ce) {
                    m.cur.off = true;
                    m.cur.offName = m.segName.size() > cpre.size() + 1 ? m.segName.substr(cpre.size() + 1) : m.segName;
                    m.cur.offType = ce->type; m.cur.offStartup = ce->startup; m.cur.offActive = ce->active; m.cur.offRecovery = ce->recovery;
                    m.cur.offBlock = ce->blockAdv; m.cur.offHit = ce->hitAdv; m.cur.offCancel = ce->cancel;
                } else {
                    // first hit of a move: may also teach the character prefix, so the rest of a string can follow it
                    ResolveOfficial(idx, m.cur, ps.moveId, true, m.segIndex == 0);
                    if (m.segIndex == 0 && m.segName.empty() && ps.moveId != -1) {
                        m.segName = NormalName(idx, ps.moveId);
                        const movelist::Entry* e = m.segName.empty() ? nullptr : FindEntry(m.segName);
                        if (e) m.segWindowEnd = e->startup + e->active;
                    }
                }
            }
            m.seenActive = true;
            m.activeFrames++;
            m.lastActive = m.moveFrames;
            m.sinceActive = 0;
            if (ps.contactNew) m.contactThisMove = true;
            if (g_game.p[1 - idx].fstate == FState::Hitstun) m.hitThisMove = true;
        } else if (f == FState::Recovery && m.seenActive) {
            if (g_game.p[1 - idx].fstate == FState::Hitstun && m.recoveryFrames < 2) m.hitThisMove = true;
            m.recoveryFrames++;
            m.sinceActive++;
        }
    } else if (m.inMove) {
        m.inMove = false;
        FinalizeMove(idx, m, false);
        m.segName.clear();
        m.segWindowEnd = -1;
    }
    // free to act = the fighter's own state is neutral, even while a lingering projectile / trap of theirs is
    // still active (Swamp Thing's log, Deadshot's turret ...)
    bool act = f == FState::Idle || (Neutral(ps.stateFlags) && !m.inKnockdown && !beingHit) || naturalFree;
    if (act && !m.wasActionable) m.actionableSince = kdForceIdleNow ? g_game.logicalFrame - 5 : g_game.logicalFrame;
    m.wasActionable = act;
    m.history.push_back(f);
    while (m.history.size() > kHistory) m.history.pop_front();
}

static bool GameTickNative() {
    if (!mkx::Ready() || g_cfg.demo) return false;
    // every simulated frame, even when the game renders slower than it simulates
    for (mkx::Snapshot& sn : mkx::TakeFrames()) {
        if (sn.frame == g_lastGameTick) continue;
        g_lastGameTick = sn.frame;
        if (!sn.p[0].valid && !sn.p[1].valid) continue;
        g_game.logicalFrame++;
        for (int i = 0; i < 2; ++i)
            if (sn.p[i].charKey && sn.p[i].charKey != g_charKey[i]) { g_charKey[i] = sn.p[i].charKey; movelist::RequestScan(); }
        ClassifyGame(0, sn.p[0]);
        ClassifyGame(1, sn.p[1]);
        UpdateAdvantage();
    }
    return true;
}

void GameTick() {
    if (GameTickNative()) return;
    g_game.demoMode = g_cfg.demo;
    mem::EvalCtx ctx;
    if (g_cfg.p1) ctx.p1 = g_cfg.p1->Eval(ctx);
    if (g_cfg.p2) ctx.p2 = g_cfg.p2->Eval(ctx);

    // practice-only lock
    g_game.locked = false;
    if (g_cfg.modeCheck) {
        int mode = 0;
        uintptr_t a = g_cfg.modeCheck->Eval(ctx);
        if (!a || !mem::Read(a, mode) || mode != g_cfg.modeValue) {
            g_game.locked = true;
            g_game.lockReason = "Overlay locked: not in the configured game mode (Practice).";
        }
    }

    // Decide whether a new gameplay frame happened.
    bool newFrame = false;
    static int lastCounter = INT32_MIN;
    static LARGE_INTEGER freq{}, last{};
    if (!freq.QuadPart) { QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&last); }

    int counter = 0;
    bool haveCounter = false;
    if (g_cfg.frameCounter && !g_game.demoMode) {
        uintptr_t a = g_cfg.frameCounter->Eval(ctx);
        haveCounter = a && mem::Read(a, counter);
    }
    bool stepping = g_game.paused && g_game.stepPending > 0;
    if (haveCounter) {
        // Exact: the game's own counter moved.
        if (counter != lastCounter) { newFrame = lastCounter != INT32_MIN; lastCounter = counter; }
        if (newFrame && stepping) g_game.stepPending--;
    } else {
        // Fallback: fixed 60 Hz clock (MKX simulates at 60 Hz). Drifts with slow-motion.
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double dt = double(now.QuadPart - last.QuadPart) / double(freq.QuadPart);
        if (g_game.paused) {
            last = now;
            if (stepping) { newFrame = true; g_game.stepPending--; }
        } else if (dt >= 1.0 / 60.0) {
            newFrame = true;
            last = now;
        }
    }

    // Pause / step through the game-speed float: hold it at 0 while paused,
    // let it run at the saved speed while a step is pending.
    if (g_cfg.gameSpeed && !g_game.demoMode && g_game.paused) {
        uintptr_t a = g_cfg.gameSpeed->Eval(ctx);
        if (a) mem::Write(a, g_game.stepPending > 0 || stepping ? g_game.savedSpeed : 0.0f);
    }

    if (!newFrame) return;
    g_game.logicalFrame++;

    if (g_game.demoMode) { DemoSample(); return; }

    SamplePlayer(0, ctx);
    SamplePlayer(1, ctx);
    Classify(0);
    Classify(1);
    UpdateAdvantage();
}

void GameTogglePause() {
    if (!g_cfg.gameSpeed) { g_game.paused = !g_game.paused; return; } // meter-only pause
    mem::EvalCtx ctx;
    if (g_cfg.p1) ctx.p1 = g_cfg.p1->Eval(ctx);
    if (g_cfg.p2) ctx.p2 = g_cfg.p2->Eval(ctx);
    uintptr_t a = g_cfg.gameSpeed->Eval(ctx);
    if (!a) return;
    if (!g_game.paused) {
        float cur = 1.f;
        if (mem::Read(a, cur) && cur > 0.f) g_game.savedSpeed = cur;
        g_game.paused = true;
    } else {
        g_game.paused = false;
        g_game.stepPending = 0;
        mem::Write(a, g_game.savedSpeed);
    }
}

void GameStep() {
    if (!g_game.paused) GameTogglePause();
    g_game.stepPending = 1;
}
