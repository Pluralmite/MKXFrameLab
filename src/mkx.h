// MKXFrameLab - native Mortal Kombat X bindings (Steam build), layouts from MK10.pdb, MKXHook (ermaccer) as reference
#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <mutex>

namespace mkx {

struct Vec3 { float x, y, z; };
struct Rot3 { int pitch, yaw, roll; };

enum ObjKind : int { OK_CollisionObj = 0, OK_Melee, OK_Projectile, OK_PlayerCollisionInfo, OK_Resolution, OK_Unknown };
const char* KindName(int k);

struct Tracked {
    uintptr_t ptr = 0;
    uintptr_t vtable = 0;
    int kind = OK_Unknown;
    uint64_t createdTick = 0;
    uintptr_t callerRva = 0;
    uintptr_t arg1 = 0, arg2 = 0, arg3 = 0;
};

struct BoneSample { int nameIndex; Vec3 pos; };

struct HurtPoint { int id, bone, part; Vec3 pos; float radius; };

struct PlayerSnap {
    uintptr_t info = 0, obj = 0, pci = 0;
    std::vector<HurtPoint> hurt;     // hit-test hurt spheres ([pci+0x48], count pci+0x40)
    std::vector<HurtPoint> strike;   // attack strike spheres ([pci+0x38], count +4)
    uintptr_t strikeList = 0;
    uint32_t stateFlags = 0;         // obj+0xA34 : 0 = free to act
    int32_t moveId = -1;             // move id (button group << 16 | slot), -1 for specials
    uint32_t attackHash = 0;         // AI sensory info: current attack hash (AIAttackProps key), 0 when idle
    uint32_t attackId = 0, lastMoveId = 0;
    uintptr_t melee = 0;             // newest live MeleeCollisionObj owned by this fighter
    int meleeState = -1;             // 0 startup, 1 active, 2 recovery, 4 connected, -1 none
    bool striking = false;
    bool projAlive = false, projActive = false;
    bool contactNew = false;
    uint32_t charKey = 0;            // which character (hash of the character asset name, e.g. char_kano_a)
    std::string charName;            // character asset name (info+0xC0)
    std::string moveName;            // special move being performed, e.g. ST_LogKick ([[obj+0xDD0]]), else empty
    std::vector<std::pair<uintptr_t, int>> hitObjs;   // this fighter's attack objects (melee / shaped / projectile) and their state         // an attack connected this frame (new CollisionResolutionObj)   // owns a live projectile / non-melee attack object           // the game hit-tested this fighter's strike spheres this frame
    bool valid = false;
    Vec3 pos{};
    std::vector<BoneSample> bones;
};

struct ShapeSnap {
    uintptr_t ptr = 0;
    int kind = 0;          // ObjKind
    int type = 0;          // 1 sphere, 2 capsule, 3 box, 4 swept capsule
    int state = -1;        // +0x380 (1 = active)
    int owner = -1;        // 0/1 = P1/P2, -1 unknown
    Vec3 pos{};
    float radius = 0, width = 0, length = 0;
    bool hasMat = false;
    float mat[16] = {};    // rows: X axis, Y axis, Z axis, origin
    std::vector<Vec3> poly;  // exact hit area the game tests (world space, in the fight plane), when available
};

struct Snapshot {
    uint64_t tick = 0;              // our MKProcDispatch call counter
    uint64_t frame = 0;             // game simulation frame (GameInfo+0xA0) - stops while frozen / slowed
    bool advanced = false;          // simulation advanced since the previous snapshot
    PlayerSnap p[2];
    std::string gameMode;           // e.g. "practice_mode" ([[[P1 info+0x88]]])
    bool camValid = false;
    Vec3 camPos{};
    Rot3 camRot{};
    float camFov = 0;
    uintptr_t camPtr = 0;
    std::vector<Tracked> live;      // live collision objects (vtable still valid)
    std::vector<ShapeSnap> shapes;  // shaped hitboxes (projectiles, special attacks)
};

bool Init();                       // resolve signatures, install hooks; call once from init thread
bool Ready();
std::string Status();              // human-readable resolution summary

// Thread-safe copy of the most recent tick snapshot (taken on the game thread).
Snapshot GetSnapshot();
std::vector<Snapshot> TakeFrames();   // every simulated frame since the last call (oldest first)
std::string BoneName(int nameIndex);

// Game-thread helpers (valid only inside the tick callback)
uintptr_t GameInfo();
bool GamePaused();                 // pause menu open
uintptr_t GetInfo(int plr);
uintptr_t GetObj(int plr);

// Pause / step (implemented with the game's own dispatch freeze)
void SetFrozen(bool f);
bool PracticeReset();
bool PracticeSetGame(int setting, int value);
bool PracticeSetDrone(int player, int setting, int value);
void PracticeDump();
void RequestPadFix();
bool Frozen();
void StepOnce();
void PollStep();                    // call from the render thread too (step must work while frozen)

// Recorder: captures raw memory of both fighters, their PlayerInfo, GameInfo,
// the camera and every live collision object for N ticks, then writes a .bin
// and a .txt index next to the plugin.
void StartRecording(int ticks, const std::string& label);
void ArmProjDump();   // per-frame memory dump of P1's next projectile (diagnostics)
bool Recording();
int RecordProgress();
std::string LastRecordingPath();

// Text event log of collision object creation (ring)
std::vector<std::string> RecentEvents();

uintptr_t Base();

// Arm the hardware tracer on the next MeleeCollisionObj created by P1.
void ArmMeleeTrace();
void ArmShapeTrace();             // hardware-trace the next P1 shaped collision object
bool MeleeTraceArmed();
} // namespace mkx
