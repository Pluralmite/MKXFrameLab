// MKXFrameLab - per-frame game state sampling and the frame meter
#pragma once
#include <cstdint>
#include <vector>
#include <deque>
#include <string>
#include <map>

enum class FState : uint8_t { None, Idle, Startup, Active, Recovery, Hitstun, Blockstun };

struct PlayerSample {
    uintptr_t object = 0;
    bool valid = false;
    float pos[3] = {0, 0, 0};
    int state = 0, moveFrame = 0, hitstun = 0, blockstun = 0;
    bool actionable = true, attackActive = false;
    bool hasState = false, hasMoveFrame = false, hasActionable = false, hasActive = false;
    int moveId = -1;
    FState fstate = FState::None;
};

struct MoveStats {
    bool valid = false;
    int startup = 0;     // frame of first active frame (1-based, counts that frame)
    int active = 0;
    int recovery = 0;
    int total = 0;
    bool fromCache = false;
    bool chained = false;    // ended by chaining / canceling into another move
    // official move-list data for this move (from the game's own table), when identified
    bool off = false;
    std::string offName;
    int offType = 0, offStartup = 0, offActive = 0, offRecovery = 0, offBlock = 0, offHit = 0, offCancel = 0;
    bool offMismatch = false;
};

struct PlayerMeter {
    std::deque<FState> history;   // newest at back
    // current move tracking
    bool inMove = false;
    bool projActive = false;      // owns a live projectile / trap hitbox right now
    bool hitThisMove = false;
    bool windup = false;          // (unused)
    std::map<uintptr_t, int> objState;   // this fighter's attack objects -> last state (segmentation)
    int sinceActive = 0;
    int firstActive = 0, lastActive = 0;   // frame numbers (within the segment) of the first / last active frame
    bool contactThisMove = false;
    bool contactNow = false;      // this frame
    uint32_t pressAtStart = 0;    // input press number that started the current busy period
    int segIndex = 0;             // hit number within the busy period (0 = first)          // non-active frames since the last active frame of the current move
    int segWindowEnd = -1;        // official end of the current hit's active window (frames into the segment), -1 unknown
    MoveStats cur;                // move in progress: official move-list data, identified at its first active frame
    std::string segName, prevSegName;   // identified move-list name of the current / previous hit
    bool noHitbox = false;        // last move ended without any active frame detected
    int lastTotal = 0;
    bool seenActive = false;
    int moveFrames = 0, activeFrames = 0, recoveryFrames = 0, startupFrames = 0;
    MoveStats last;
    uint64_t actionableSince = 0; // logical frame index when this player last became actionable
    bool wasActionable = true;
    int moveId = -1;              // move id of the move being tracked
    // per-interaction (attacker side)
    bool attacked = false;        // started a move during the current interaction
    int firstMoveId = -1;
    bool canceled = false;
    int cancelFrom = -1, cancelFrame = 0;
    MoveStats firstMove;          // stats of the first move of the interaction (before any cancel)
    // last results shown in the HUD (from this player's point of view)
    bool hitValid = false, blockValid = false, cancelValid = false, cancelUnknown = false;
    int hitAdv = 0, blockAdv = 0, cancelAdv = 0;
    int liveCell = -1;            // 0 hit, 1 block while counting
    int liveAdv = 0;
    std::vector<int> gaps;        // free frames for the defender between hits of the last string / special
    bool gapsBlocked = false;
    bool liveKD = false;          // counting through a knockdown
    bool hitKD = false;           // last hit was a knockdown
    int kdDelay = 0;              // frames the defender delayed their wakeup
    bool kdFrameKill = false;     // hitAdv is what is left after a follow-up done during the knockdown
    int kdBase = 0;               // the knockdown's own advantage before that follow-up
    std::string kdAfter;          // the follow-up's move-list name
    // defender-side knockdown tracking
    bool inKnockdown = false, sawGetup = false;
    int zeroRun = 0, groundFrames = 0;
};

struct MoveCacheEntry { int total = 0; bool hasHit = false, hasBlock = false; int hitAdv = 0, blockAdv = 0; };

struct GameState {
    uint64_t logicalFrame = 0;
    PlayerSample p[2];
    PlayerMeter meter[2];
    // advantage
    bool interaction = false;
    bool advValid = false;
    int advantageP1 = 0;  // + = P1 acts first
    bool advLive = false; // still counting
    bool locked = false;  // practice-mode lock active
    std::string lockReason;
    // pause / step
    bool paused = false;
    int stepPending = 0;
    float savedSpeed = 1.f;
    bool demoMode = false;
    std::map<int, MoveCacheEntry> moveCache[2];
    int minGround = 0;   // shortest ground time seen (= no delayed wakeup)
};

extern GameState g_game;

// Call once per rendered frame. Detects new gameplay frames and samples them.
void GameTick();
void GameTogglePause();
void GameStep();
const char* FStateName(FState s);
