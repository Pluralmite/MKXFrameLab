#include <map>
#include <cstring>
#include <cmath>
#include "mkx.h"
#include "memory.h"
#include "log.h"
#include "MinHook.h"
#include "trace.h"
#include "bot.h"
#include "buffer.h"
#include "movelist.h"
namespace dib { void Init(); }
#include <windows.h>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <deque>
#include <thread>
#include <algorithm>

namespace mkx {

// ----------------------------------------------------------------------------- signatures
// MK10.exe (final Steam build). Function names come from the game's own MK10.pdb, which ships in Binaries\Retail;
// MKXHook by ermaccer (https://github.com/ermaccer/MKXHook) was the reference for the game tick, players,
// camera, game speed and the freeze-world hook.
struct Sig { const char* name; const char* pat; int off; bool rip; uintptr_t addr; };
enum {
    S_MKProcDispatch, S_Dispatch, S_FindGameInfo, S_GetObj, S_GetPos, S_CamSetPos,
    S_GetBonePos, S_GetBoneName, S_GetNames, S_SlowGameTime,
    S_CollCtor0, S_CollCtor1, S_MeleeCtor, S_ProjCtor, S_PCICtor, S_ResCtor, S_StrikeTest, S_VictimRegion, S_TightRope, S_AttachMatrix,
    S_CheckColChar, S_Sphere2D, S_Sphere3D, S_Cyl2D, S_Cyl3D, S_Box2D, S_Box3D, S_RotVector, S_MatrixCtor,
    S_COUNT
};
static Sig g_sigs[S_COUNT] = {
    {"MKProcDispatch", "48 8B C4 41 56 48 81 EC 40 06 00 00 48 C7 44 24 50 FE FF FF FF", 0, false, 0},
    {"GAME_INFO::UpdateGamePauseState", "8B 51 18 39 51 14 74 ? 48 8B 01 89 51 14 48 FF A0 D0 00 00 00", 0, false, 0},
    {"GFGGameInfo", "48 83 EC 28 8B D1 48 8B 0D ? ? ? ? E8 ? ? ? ? 48 85 C0 75 ?", 9, true, 0},
    {"GetFGCharacterObj", "48 83 EC 28 8B D1 48 8B 0D ? ? ? ? E8 ? ? ? ? 48 85 C0 75 ?", 0, false, 0},
    {"MKObj::GetWorldLocation", "4C 8B 41 20 4D 85 C0 74 ? 48 85 D2 74 ? 41 8B 80 E8 00 00 00", 0, false, 0},
    {"AActor::SetLocation", "48 83 EC 28 8B 02 89 81 E8 00 00 00 8B 42 04 89 81 EC 00 00 00 8B 42 08 BA 01 00 00 00 89 81 F0 00 00 00 E8", 0, false, 0},
    {"FGCharacterObj::GetBoneWorldPos", "4D 85 C0 74 ? 53 48 83 EC 60 48 8B 01 49 8B D8 4C 8D 44 24 20 FF 90 38 02 00 00 F3 0F 10 44 24 50", 0, false, 0},
    {"USkeletalMeshComponent::GetBoneName", "40 53 48 83 EC 20 48 8B 89 C0 02 00 00 48 8B DA", 0, false, 0},
    {"FName::Names", "48 89 5C 24 18 55 48 83 EC 30 48 83 B9 B0 0F 00 00 00", 0x54 + 3, true, 0},
    {"SlowGameTimeForXTicks", "40 57 48 83 EC 50 48 C7 44 24 30 FE FF FF FF 48 89 5C 24 60 0F 29 74 24 40", 0, false, 0},
    {"CollisionObj::CollisionObj(actor)", "48 89 4C 24 08 56 57 41 56 48 83 EC 40 48 C7 44 24 30 FE FF FF FF 48 89 5C 24 68 48 89 6C 24 70 49 8B F9 49 8B F0 4C 8B F1", 0, false, 0},
    {"CollisionObj::CollisionObj", "48 89 4C 24 08 57 41 56 41 57 48 83 EC 40 48 C7 44 24 30 FE FF FF FF 48 89 5C 24 68 48 89 6C 24 70 48 89 74 24 78 41 8B F9", 0, false, 0},
    {"MeleeCollisionObj::MeleeCollisionObj", "48 89 4C 24 08 57 48 83 EC 40 48 C7 44 24 30 FE FF FF FF 48 89 5C 24 58 48 89 74 24 60 49 8B D8 48 8B F2", 0, false, 0},
    {"ProjectileCollisionObj::ProjectileCollisionObj", "48 89 4C 24 08 57 48 83 EC 40 48 C7 44 24 30 FE FF FF FF 48 89 5C 24 58 49 8B F8 48 8B D9", 0, false, 0},
    {"PlayerCollisionInfo::PlayerCollisionInfo", "48 89 4C 24 08 53 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 8B D9 45 33 C0 41 8D 50 01 E8 ? ? ? ? 90 48 8D 05 ? ? ? ? 48 89 03 48 8D 4B 18 E8 ? ? ? ? 45 33 D2", 0, false, 0},
    {"CollisionResolutionObj::CollisionResolutionObj", "48 89 4C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 60 EB FF FF", 0, false, 0},
    {"FGCollision::CheckPlayerCollision", "48 89 4C 24 08 53 57 41 55 41 56 48 81 EC 98 00 00 00", 0, false, 0},
    {"FGCharacterObj::GetModifiedVictimRegionForBone", "48 89 5C 24 18 55 48 83 EC 30 48 83 B9 B0 0F 00 00 00", 0, false, 0},
    {"CollideSpheres2DOnTightRope", "48 83 EC 58 44 0F 29 5C 24 30 F3 44 0F 10 59 0C", 0, false, 0},
    {"ObjAttachmentData::GetAttachmentMatrix", "48 89 5C 24 18 56 48 83 EC 60 48 8B DA 48 8B F1 48 85 D2", 0, false, 0},
    {"CollisionObj::CheckCollisionAgainstCharacter", "48 89 5C 24 10 48 89 74 24 18 4C 89 74 24 20 55 48 8D 6C 24 A9", 0, false, 0},
    {"FGCollision::CollideSphereVsPlayer2D", "48 89 6C 24 18 56 57 41 56 48 81 EC A0 00 00 00 48 8B E9", 0, false, 0},
    {"FGCollision::CollideSphereVsPlayer3D", "48 89 6C 24 18 56 57 41 56 48 81 EC C0 00 00 00", 0, false, 0},
    {"collide_cylinder_vs_character_2D", "48 8B C4 48 89 50 10 55 56 57 41 54 41 55 41 56 41 57 48 8D 68 B8", 0, false, 0},
    {"collide_cylinder_vs_character_3D", "48 8B C4 48 89 58 20 55 56 41 57 48 8D 68 B9 48 81 EC F0 00 00 00", 0, false, 0},
    {"collide_box_vs_character_2D", "48 8B C4 57 41 54 41 55 41 56 41 57 48 81 EC 10 01 00 00", 0, false, 0},
    {"collide_box_vs_character_3D", "4C 8B DC 55 53 56 57 41 54 41 57 49 8D AB C8 FE FF FF", 0, false, 0},
    {"FRotator::Vector", "40 53 48 83 EC 60 48 8B DA 48 8B D1 48 8D 4C 24 20 E8 ? ? ? ? 8B 08", 0, false, 0},
    {"FMatrix::FMatrix(loc, rot)", "48 8B C4 53 48 81 EC 00 01 00 00 0F 29 70 E8 0F 29 78 D8 44 0F 29 40 C8 48 8B D9", 0, false, 0},
};

// vtable RVAs for this build (MK10.pdb), verified at runtime through RTTI names
struct VtDef { int kind; uintptr_t rva; const char* rtti; uintptr_t va; };
static VtDef g_vts[] = {
    {OK_CollisionObj, 0x25FB688, ".?AVCollisionObj@@", 0},
    {OK_Melee, 0x25FBC88, ".?AVMeleeCollisionObj@@", 0},
    {OK_Projectile, 0x25FB888, ".?AVProjectileCollisionObj@@", 0},
    {OK_PlayerCollisionInfo, 0x25FB3E8, ".?AUPlayerCollisionInfo@@", 0},
    {OK_Resolution, 0x25FC098, ".?AVCollisionResolutionObj@@", 0},
};

// ----------------------------------------------------------------------------- MKX layouts (from MK10.pdb)
namespace off {
// FG_GAME_INFO
constexpr int GI_PausedFlag = 0x10, GI_PausedWhy = 0x14, GI_TickCtr = 0xD0, GI_Players = 0x1968, GI_GameMode = 0x198C;
constexpr int GM_Practice = 2;
// PLAYER_INFO
constexpr int PI_CharName = 0x128;   // mCharacterDefinition.mCharName (FString: char* data, int count)
// FGCharacterObj
constexpr int CO_MoveID = 0x54, CO_SkelComp = 0x338, CO_PlayerFlags = 0xA28, CO_State = 0xA2C, CO_CollisionInfo = 0x1BE0;
// PlayerCollisionInfo
constexpr int PCI_Owner = 0x28, PCI_Attack = 0x38, PCI_NumVictim = 0x40, PCI_Victim = 0x48;
constexpr int NODE_SIZE = 0x30;   // CollisionNode: FName bone_name, int bone_index, int enabled, {FVector center, float radius}, prev shape
// PlayerAttackCollisionInfo: uint count, uint count_acd, CollisionNode attack_regions[36]
constexpr int PACI_Regions = 0x8;
// CollisionObj
constexpr int COL_Owner = 0x340, COL_State = 0x390, COL_Type = 0x39C, COL_Width = 0x3FC, COL_Length = 0x400, COL_Height = 0x404;
// ACamera: CameraCache.POV
constexpr int CAM_Pos = 0x414, CAM_Rot = 0x420, CAM_Fov = 0x42C;
}

const char* KindName(int k) {
    switch (k) {
    case OK_CollisionObj: return "CollisionObj";
    case OK_Melee: return "MeleeCollisionObj";
    case OK_Projectile: return "ProjectileCollisionObj";
    case OK_PlayerCollisionInfo: return "PlayerCollisionInfo";
    case OK_Resolution: return "CollisionResolutionObj";
    default: return "?";
    }
}

static uintptr_t g_base = 0;
static bool g_ready = false;
static std::string g_status;
uintptr_t Base() { return g_base; }

// ----------------------------------------------------------------------------- game function types
using VoidFn = void (*)();
using DispatchFn = void (*)(uintptr_t, int);
using GetObjFn = uintptr_t (*)(int);
using GetPosFn = void (*)(uintptr_t, Vec3*);
using CamSetPosFn = int (*)(uintptr_t, Vec3*);
using GetBonePosFn = void (*)(uintptr_t, const uint64_t*, Vec3*);
using GetBoneNameFn = void (*)(uintptr_t, uint64_t*, int);
using SlowFn = void (*)(float, int, int);
using Ctor7 = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

using StrikeTestFn = int (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
static StrikeTestFn o_StrikeTest = nullptr;
static std::atomic<uintptr_t> g_pciOf[2], g_objOf[2];
struct StrikeCap { uint64_t seq = 0; std::vector<HurtPoint> pts; uintptr_t victim = 0; };
static std::mutex g_capMtx;
static StrikeCap g_cap[2];
static uint64_t g_capSeq = 0, g_capSeen[2] = {0, 0};

// the attacker's attack regions (bone spheres) exactly as FGCollision::CheckPlayerCollision tests them
static Vec3 OnFightLine(const Vec3& p);
static void ToPlane(const float* p, float& x, float& y);
static float HurtRadius(uintptr_t defender, uintptr_t node, float r);
static void ReadStrikeList(uintptr_t pci, std::vector<HurtPoint>& out) {
    uintptr_t S = mem::ReadOr<uintptr_t>(pci + off::PCI_Attack, 0);
    uint32_t sn = S ? mem::ReadOr<uint32_t>(S, 0) : 0;
    if (sn == 0 || sn > 36) return;
    static uint8_t buf[off::NODE_SIZE * 36 + 0x10];
    if (!mem::ReadRaw(S + off::PACI_Regions, buf, off::NODE_SIZE * sn)) return;
    for (uint32_t i = 0; i < sn; ++i) {
        const uint8_t* e = buf + off::NODE_SIZE * i;
        HurtPoint h{};
        memcpy(&h.id, e + 0x00, 4);
        memcpy(&h.bone, e + 0x08, 4);
        memcpy(&h.pos, e + 0x10, 12);
        memcpy(&h.radius, e + 0x1C, 4);
        h.pos = OnFightLine(h.pos);   // the game tests strikes in 2D on the fight line: draw them where it tests them
        h.part = (int)i;
        out.push_back(h);
    }
}

// FGCollision::CheckPlayerCollision(attacker, victim, result): the strike-vs-hurt test for melee attacks
static int h_StrikeTest(uintptr_t attacker, uintptr_t victim, uintptr_t result, uintptr_t a4) {
    for (int p = 0; p < 2; ++p) {
        if (attacker && attacker == g_objOf[p].load() && g_pciOf[p].load()) {
            std::vector<HurtPoint> pts;
            ReadStrikeList(g_pciOf[p].load(), pts);
            std::lock_guard<std::mutex> l(g_capMtx);
            g_cap[p].pts.swap(pts);
            g_cap[p].victim = victim;
            g_cap[p].seq = ++g_capSeq;
        }
    }
    char chk[200] = "";
    if (g_logVerbose) {
        // accuracy check: the 2D gap between the strike spheres we draw and the victim's hurt spheres (with the
        // radius the game uses), next to the game's own answer. Exact geometry = every hit has gap <= 0.
        for (int p = 0; p < 2; ++p) {
            if (!attacker || attacker != g_objOf[p].load() || !g_pciOf[p].load() || !g_pciOf[1 - p].load()) continue;
            std::vector<HurtPoint> pts;
            ReadStrikeList(g_pciOf[p].load(), pts);
            uintptr_t vp = g_pciOf[1 - p].load();
            int n = mem::ReadOr<int>(vp + off::PCI_NumVictim, 0);
            uintptr_t arr = mem::ReadOr<uintptr_t>(vp + off::PCI_Victim, 0);
            float best = 1e9f;
            int ba = -1, bv = -1;
            for (int v = 0; arr && v < n && v < 64; ++v) {
                uintptr_t node = arr + (uintptr_t)v * off::NODE_SIZE;
                if (!mem::ReadOr<int>(node + 0xC, 0)) continue;
                float c[3] = {mem::ReadOr<float>(node + 0x10, 0), mem::ReadOr<float>(node + 0x14, 0), mem::ReadOr<float>(node + 0x18, 0)};
                float vr = HurtRadius(victim, node, mem::ReadOr<float>(node + 0x1C, 0));
                float vx, vy;
                ToPlane(c, vx, vy);
                for (size_t a = 0; a < pts.size(); ++a) {
                    float ax, ay;
                    ToPlane(&pts[a].pos.x, ax, ay);
                    float d = sqrtf((ax - vx) * (ax - vx) + (ay - vy) * (ay - vy)) - pts[a].radius - vr;
                    if (d < best) { best = d; ba = (int)a; bv = v; }
                }
            }
            uintptr_t gi = GameInfo();
            snprintf(chk, sizeof(chk), "strikechk: P%d f%u gap %.2f (strike %d vs hurt %d, %d strike spheres)", p + 1,
                     gi ? mem::ReadOr<uint32_t>(gi + off::GI_TickCtr, 0) : 0, best, ba, bv, (int)pts.size());
        }
    }
    int r = o_StrikeTest(attacker, victim, result, a4);
    if (chk[0]) Log("%s  game %s", chk, r ? "HIT" : "miss");
    return r;
}

using AttachMatFn = int (*)(uintptr_t, float*);
using GetMatVFn = float* (*)(uintptr_t, float*);

// The PlayerCollisionInfo is created when the match loads. After a hot reload we missed its constructor,
// so look for it through pointers held by the fighter object / player info (1 and 2 levels deep).
static uintptr_t FindPciByScan(uintptr_t obj, uintptr_t info) {
    static uintptr_t cached[2] = {0, 0};
    static uintptr_t cachedObj[2] = {0, 0};
    static ULONGLONG lastTry = 0;
    uintptr_t vtPci = 0;
    for (auto& v : g_vts) if (v.kind == OK_PlayerCollisionInfo) vtPci = v.va;
    if (!vtPci || !obj) return 0;
    for (int i = 0; i < 2; ++i)
        if (cachedObj[i] == obj && cached[i] && mem::ReadOr<uintptr_t>(cached[i], 0) == vtPci &&
            mem::ReadOr<uintptr_t>(cached[i] + off::PCI_Owner, 0) == obj) return cached[i];
    ULONGLONG now = GetTickCount64();
    if (now - lastTry < 200) return 0;
    lastTry = now;
    auto isPci = [&](uintptr_t p) { return p && mem::ReadOr<uintptr_t>(p, 0) == vtPci && mem::ReadOr<uintptr_t>(p + off::PCI_Owner, 0) == obj; };
    uintptr_t roots[2] = {obj, info};
    static uint8_t buf[0x4000], buf2[0x1000];
    for (uintptr_t root : roots) {
        if (!root || !mem::ReadRaw(root, buf, sizeof(buf))) continue;
        for (int o = 0; o < (int)sizeof(buf); o += 8) {
            uintptr_t p; memcpy(&p, buf + o, 8);
            if (p < 0x10000 || (p & 7)) continue;
            if (isPci(p)) goto found1;
            continue;
        found1:
            for (int i = 0; i < 2; ++i) if (!cached[i] || cachedObj[i] == obj) { cached[i] = p; cachedObj[i] = obj; break; }
            Log("pci: found by scan at root+0x%X (%s)", o, root == obj ? "obj" : "info");
            return p;
        }
    }
    for (uintptr_t root : roots) {
        if (!root || !mem::ReadRaw(root, buf, sizeof(buf))) continue;
        for (int o = 0; o < 0x1000; o += 8) {
            uintptr_t p; memcpy(&p, buf + o, 8);
            if (p < 0x10000 || (p & 7) || !mem::ReadRaw(p, buf2, sizeof(buf2))) continue;
            for (int o2 = 0; o2 < (int)sizeof(buf2); o2 += 8) {
                uintptr_t q; memcpy(&q, buf2 + o2, 8);
                if (q < 0x10000 || (q & 7) || !isPci(q)) continue;
                for (int i = 0; i < 2; ++i) if (!cached[i] || cachedObj[i] == obj) { cached[i] = q; cachedObj[i] = obj; break; }
                Log("pci: found by scan at [root+0x%X]+0x%X (%s)", o, o2, root == obj ? "obj" : "info");
                return q;
            }
        }
    }
    return 0;
}


// ---- exact geometry through the game's own functions ----
using VictimRegionFn = float (*)(uintptr_t, float, uintptr_t);
using AttachMatrixFn = int (*)(uintptr_t, float*);
static AttachMatrixFn g_attachMatrix = nullptr;
static VictimRegionFn g_victimRegion = nullptr;
static uintptr_t g_planeO = 0, g_planeA = 0;
static uintptr_t RipTarget(uintptr_t insn, int dispAt, int len) { return insn + len + mem::ReadOr<int32_t>(insn + dispAt, 0); }
static void InitExact() {
    g_victimRegion = (VictimRegionFn)g_sigs[S_VictimRegion].addr;
    g_attachMatrix = (AttachMatrixFn)g_sigs[S_AttachMatrix].addr;
    if (uintptr_t f = g_sigs[S_TightRope].addr) {
        // CollideSpheres2DOnTightRope: subss xmm2,[O.x] at +0x88, mulss xmm2,[S.x] at +0xA0 (2D = (dot(p - O, S), z))
        if (mem::ReadOr<uint32_t>(f + 0x88, 0) == 0x155C0FF3u) g_planeO = RipTarget(f + 0x88, 4, 8);
        if (mem::ReadOr<uint32_t>(f + 0xA0, 0) == 0x15590FF3u) g_planeA = RipTarget(f + 0xA0, 4, 8);
    }
    Log("exact geometry: victim radius %s, fight line %s", g_victimRegion ? "ok" : "MISSING", (g_planeO && g_planeA) ? "ok" : "MISSING");
}
// world point from a 2D fight-line point (x along the line, y = height)
static Vec3 PlaneToWorld(float x, float y) {
    float O[3] = {0, 0, 0}, A[3] = {0, 1, 0};
    if (g_planeO) mem::ReadRaw(g_planeO, O, 12);
    if (g_planeA) mem::ReadRaw(g_planeA, A, 12);
    float n2 = A[0] * A[0] + A[1] * A[1] + A[2] * A[2];
    if (n2 < 1e-6f) n2 = 1;
    return Vec3{O[0] + A[0] * x / n2, O[1] + A[1] * x / n2, y};
}
// CheckPlayerCollision / CollideSpheres2DOnTightRope test strike and hurt spheres in 2D: (distance along the
// tightrope, height). A sphere's depth off that line does not matter, so draw every sphere on the line itself.
static Vec3 OnFightLine(const Vec3& p) {
    if (!g_planeO || !g_planeA) return p;
    float O[3], A[3];
    if (!mem::ReadRaw(g_planeO, O, 12) || !mem::ReadRaw(g_planeA, A, 12)) return p;
    float n2 = A[0] * A[0] + A[1] * A[1] + A[2] * A[2];
    if (n2 < 1e-6f) return p;
    float x = (p.x - O[0]) * A[0] + (p.y - O[1]) * A[1] + (p.z - O[2]) * A[2];
    return Vec3{O[0] + A[0] * x / n2, O[1] + A[1] * x / n2, p.z};
}
// ---- exact shaped / projectile hit areas: what CollisionObj::CheckCollisionAgainstCharacter actually tests.
// It dispatches on the object's type: sphere (swept along its velocity), cylinder (centred on the object along
// its rotation), box (half extents length / width / height along the object's axes) and beam (a cylinder from
// the attachment point to the object). Each test projects the shape into the 2D fight plane (along the
// tightrope, height = z) and checks it against the victim's hurt spheres. The hooks below record every shape
// tested for an object during one check, build the same 2D outline and keep it for drawing.
using CheckCharFn = int (*)(uintptr_t, uintptr_t, uintptr_t);
using SphereTestFn = int (*)(uintptr_t, const float*, float, uintptr_t);
using CylTestFn = int (*)(uintptr_t, const float*, const int*, float, float, uintptr_t, uintptr_t);
using BoxTestFn = int (*)(uintptr_t, const float*, const int*, float, float, float, uintptr_t, uintptr_t);
using RotVectorFn = float* (*)(const int*, float*);
using MatrixCtorFn = void* (*)(float*, const float*, const int*);
static CheckCharFn o_CheckChar = nullptr;
static SphereTestFn o_Sphere2D = nullptr, o_Sphere3D = nullptr;
static CylTestFn o_Cyl2D = nullptr, o_Cyl3D = nullptr;
static BoxTestFn o_Box2D = nullptr, o_Box3D = nullptr;
static thread_local uintptr_t t_curCol = 0;
static thread_local std::vector<float> t_pts2;   // 2D points (x along the fight line, y = height) of this check
struct TestedShape { std::vector<Vec3> poly; uint32_t frame = 0; int kind = 0; };   // kind: 1 sphere 2 cylinder 3 box
static std::mutex g_testMtx;
static std::map<uintptr_t, TestedShape> g_tested;
static void ToPlane(const float* p, float& x, float& y) {
    float O[3] = {0, 0, 0}, A[3] = {0, 1, 0};
    if (g_planeO) mem::ReadRaw(g_planeO, O, 12);
    if (g_planeA) mem::ReadRaw(g_planeA, A, 12);
    x = (p[0] - O[0]) * A[0] + (p[1] - O[1]) * A[1] + (p[2] - O[2]) * A[2];
    y = p[2];
}
static void AddCircle2(float cx, float cy, float r) {
    for (int i = 0; i < 28; ++i) {
        float a = i * 6.2831853f / 28.f;
        t_pts2.push_back(cx + cosf(a) * r);
        t_pts2.push_back(cy + sinf(a) * r);
    }
}
static void AddPoint3(const float* p) { float x, y; ToPlane(p, x, y); t_pts2.push_back(x); t_pts2.push_back(y); }
static int t_kind = 0;
static int h_Sphere2D(uintptr_t victim, const float* pos, float r, uintptr_t res) {
    if (t_curCol && pos) { float x, y; ToPlane(pos, x, y); AddCircle2(x, y, r); t_kind = 1; }
    return o_Sphere2D(victim, pos, r, res);
}
static int h_Sphere3D(uintptr_t victim, const float* pos, float r, uintptr_t res) {
    if (t_curCol && pos) { float x, y; ToPlane(pos, x, y); AddCircle2(x, y, r); t_kind = 1; }
    return o_Sphere3D(victim, pos, r, res);
}
static void RecordCylinderDir(const float* start, const float* dir, float r, float len);
static void RecordCylinder(const float* start, const int* rot, float r, float len) {
    if (!t_curCol || !start || !rot || !g_sigs[S_RotVector].addr) return;
    float d[3] = {1, 0, 0};
    ((RotVectorFn)g_sigs[S_RotVector].addr)(rot, d);
    RecordCylinderDir(start, d, r, len);
}
static void RecordCylinderDir(const float* start, const float* dir, float r, float len) {
    float d[3] = {dir[0], dir[1], dir[2]};
    float n = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (n < 1e-6f) return;
    for (float& v : d) v /= n;
    // two unit vectors across the axis: the end discs of the cylinder
    float t[3] = {fabsf(d[2]) < 0.9f ? 0.f : 1.f, 0.f, fabsf(d[2]) < 0.9f ? 1.f : 0.f};
    float u[3] = {d[1] * t[2] - d[2] * t[1], d[2] * t[0] - d[0] * t[2], d[0] * t[1] - d[1] * t[0]};
    float un = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    for (float& v : u) v /= un;
    float w[3] = {d[1] * u[2] - d[2] * u[1], d[2] * u[0] - d[0] * u[2], d[0] * u[1] - d[1] * u[0]};
    for (int e = 0; e < 2; ++e) {
        float c[3] = {start[0] + d[0] * len * e, start[1] + d[1] * len * e, start[2] + d[2] * len * e};
        for (int i = 0; i < 24; ++i) {
            float a = i * 6.2831853f / 24.f, ca = cosf(a) * r, sa = sinf(a) * r;
            float p[3] = {c[0] + u[0] * ca + w[0] * sa, c[1] + u[1] * ca + w[1] * sa, c[2] + u[2] * ca + w[2] * sa};
            AddPoint3(p);
        }
    }
    t_kind = 2;
}
static int h_Cyl2D(uintptr_t victim, const float* start, const int* rot, float r, float len, uintptr_t a6, uintptr_t res) {
    RecordCylinder(start, rot, r, len);
    if (t_curCol && g_logVerbose && start && rot)
        Log("cyl2d: start %.1f %.1f %.1f rot %d %d %d r %.1f len %.1f", start[0], start[1], start[2], rot[0], rot[1], rot[2], r, len);
    return o_Cyl2D(victim, start, rot, r, len, a6, res);
}
static int h_Cyl3D(uintptr_t victim, const float* start, const int* rot, float r, float len, uintptr_t a6, uintptr_t res) {
    RecordCylinder(start, rot, r, len);
    if (t_curCol && g_logVerbose && start && rot)
        Log("cyl3d: start %.1f %.1f %.1f rot %d %d %d r %.1f len %.1f", start[0], start[1], start[2], rot[0], rot[1], rot[2], r, len);
    return o_Cyl3D(victim, start, rot, r, len, a6, res);
}
static void RecordBox(const float* loc, const int* rot, float length, float width, float height) {
    if (!t_curCol || !loc || !rot || !g_sigs[S_MatrixCtor].addr) return;
    alignas(16) float m[16] = {};
    ((MatrixCtorFn)g_sigs[S_MatrixCtor].addr)(m, loc, rot);   // rows: X, Y, Z axes, origin
    float hx = length * 0.5f, hy = width * 0.5f, hz = height * 0.5f;
    for (int i = 0; i < 8; ++i) {
        float sx = (i & 1) ? hx : -hx, sy = (i & 2) ? hy : -hy, sz = (i & 4) ? hz : -hz;
        float p[3];
        for (int k = 0; k < 3; ++k) p[k] = loc[k] + m[k] * sx + m[4 + k] * sy + m[8 + k] * sz;
        AddPoint3(p);
    }
    t_kind = 3;
}
static int h_Box2D(uintptr_t victim, const float* loc, const int* rot, float length, float width, float height, uintptr_t a7, uintptr_t res) {
    RecordBox(loc, rot, length, width, height);
    return o_Box2D(victim, loc, rot, length, width, height, a7, res);
}
static int h_Box3D(uintptr_t victim, const float* loc, const int* rot, float length, float width, float height, uintptr_t a7, uintptr_t res) {
    RecordBox(loc, rot, length, width, height);
    return o_Box3D(victim, loc, rot, length, width, height, a7, res);
}
// 2D convex hull (monotone chain) of t_pts2, returned as world points in the fight plane
static std::vector<Vec3> HullToWorld(const std::vector<float>& p2) {
    struct P { float x, y; };
    std::vector<P> p;
    for (size_t i = 0; i + 1 < p2.size(); i += 2) p.push_back({p2[i], p2[i + 1]});
    std::sort(p.begin(), p.end(), [](const P& a, const P& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    if (p.size() < 3) return {};
    std::vector<P> h(p.size() * 2);
    size_t k = 0;
    auto cross = [](const P& o, const P& a, const P& b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); };
    for (size_t i = 0; i < p.size(); ++i) { while (k >= 2 && cross(h[k - 2], h[k - 1], p[i]) <= 0) --k; h[k++] = p[i]; }
    for (size_t i = p.size() - 1, t = k + 1; i > 0; --i) { while (k >= t && cross(h[k - 2], h[k - 1], p[i - 1]) <= 0) --k; h[k++] = p[i - 1]; }
    h.resize(k > 1 ? k - 1 : k);
    std::vector<Vec3> out;
    size_t step = h.size() > 60 ? (h.size() + 59) / 60 : 1;   // the drawer takes up to 64 points
    for (size_t i = 0; i < h.size(); i += step) out.push_back(PlaneToWorld(h[i].x, h[i].y));
    return out;
}
// The outline a CollisionObj will be tested with, built exactly like CheckCollisionAgainstCharacter does
// (for shapes that are armed but not tested yet, and for drawing them faintly before they go live).
static std::vector<Vec3> ComputeShape(uintptr_t col, int type, float w, float l, float h) {
    std::vector<Vec3> out;
    uintptr_t actor = mem::ReadOr<uintptr_t>(col + 0x20, 0);
    if (!actor || !g_planeO || !g_planeA) return out;
    float loc[3];
    int rot[3];
    if (!mem::ReadRaw(actor + 0xE8, loc, 12) || !mem::ReadRaw(actor + 0xF4, rot, 12)) return out;   // MKObj::GetWorldLocation / Rotation
    uintptr_t saved = t_curCol;
    t_curCol = col;
    t_pts2.clear();
    if (type == 0) {
        float x, y; ToPlane(loc, x, y); AddCircle2(x, y, w);
    } else if (type == 1 && g_sigs[S_RotVector].addr) {   // cylinder centred on the object along its rotation
        float d[3] = {1, 0, 0};
        ((RotVectorFn)g_sigs[S_RotVector].addr)(rot, d);
        float st[3] = {loc[0] - d[0] * h * 0.5f, loc[1] - d[1] * h * 0.5f, loc[2] - d[2] * h * 0.5f};
        RecordCylinder(st, rot, w, h);
    } else if (type == 2) {   // box: half extents length / width / height along the object's axes
        RecordBox(loc, rot, l, w, h);
    } else if (type == 3) {   // beam: cylinder from the attachment point to the object
        alignas(16) float am[16];
        if (g_attachMatrix && g_attachMatrix(col + 0x3A0, am)) {
            float d[3] = {loc[0] - am[12], loc[1] - am[13], loc[2] - am[14]};
            float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (len > 1e-3f) RecordCylinderDir(&am[12], d, w, len);
        } else {
            float x, y; ToPlane(loc, x, y); AddCircle2(x, y, w);
        }
    }
    if (!t_pts2.empty()) out = HullToWorld(t_pts2);
    t_pts2.clear();
    t_curCol = saved;
    return out;
}
static int h_CheckChar(uintptr_t col, uintptr_t victim, uintptr_t result) {
    if (t_curCol) return o_CheckChar(col, victim, result);   // nested: leave it to the outer call
    t_curCol = col;
    t_pts2.clear();
    t_kind = 0;
    int r = o_CheckChar(col, victim, result);
    t_curCol = 0;
    if (g_logVerbose) Log("chk: col %llX state %d kind %d pts %zu -> %d", (unsigned long long)col,
                          mem::ReadOr<int>(col + off::COL_State, -1), t_kind, t_pts2.size() / 2, r);
    if (!t_pts2.empty()) {
        TestedShape ts;
        ts.poly = HullToWorld(t_pts2);
        if (g_logVerbose && ts.poly.size() >= 3) {
            // accuracy check: 2D gap between the outline we draw and the victim's hurt spheres vs the game's answer
            std::vector<std::pair<float, float>> P;
            for (auto& v : ts.poly) { float x, y, p[3] = {v.x, v.y, v.z}; ToPlane(p, x, y); P.push_back({x, y}); }
            float best = 1e9f;
            for (int q = 0; q < 2; ++q) {
                if (g_objOf[q].load() != victim || !g_pciOf[q].load()) continue;
                uintptr_t vp = g_pciOf[q].load();
                int n = mem::ReadOr<int>(vp + off::PCI_NumVictim, 0);
                uintptr_t arr = mem::ReadOr<uintptr_t>(vp + off::PCI_Victim, 0);
                for (int v = 0; arr && v < n && v < 64; ++v) {
                    uintptr_t node = arr + (uintptr_t)v * off::NODE_SIZE;
                    if (!mem::ReadOr<int>(node + 0xC, 0)) continue;
                    float c[3] = {mem::ReadOr<float>(node + 0x10, 0), mem::ReadOr<float>(node + 0x14, 0), mem::ReadOr<float>(node + 0x18, 0)};
                    float vr = HurtRadius(victim, node, mem::ReadOr<float>(node + 0x1C, 0)), cx, cy;
                    ToPlane(c, cx, cy);
                    // distance from the circle centre to the convex outline (negative inside)
                    bool inside = true;
                    float dmin = 1e9f;
                    for (size_t i = 0; i < P.size(); ++i) {
                        auto a = P[i], b = P[(i + 1) % P.size()];
                        float ex = b.first - a.first, ey = b.second - a.second, wx = cx - a.first, wy = cy - a.second;
                        float cr = ex * wy - ey * wx;
                        if (cr < 0) inside = false;
                        float t = (ex * wx + ey * wy) / (ex * ex + ey * ey + 1e-9f);
                        t = t < 0 ? 0 : t > 1 ? 1 : t;
                        float dx = a.first + ex * t - cx, dy = a.second + ey * t - cy;
                        float d = sqrtf(dx * dx + dy * dy);
                        if (d < dmin) dmin = d;
                    }
                    float g = (inside ? -dmin : dmin) - vr;
                    if (g < best) best = g;
                }
            }
            Log("shapechk: col %llX type %d gap %.2f  game %s", (unsigned long long)col, t_kind, best, r ? "HIT" : "miss");
        }
        ts.kind = t_kind;
        uintptr_t gi = GameInfo();
        ts.frame = gi ? mem::ReadOr<uint32_t>(gi + off::GI_TickCtr, 0) : 0;
        std::lock_guard<std::mutex> l(g_testMtx);
        g_tested[col] = std::move(ts);
        if (g_tested.size() > 256) g_tested.clear();
    }
    return r;
}

// the hurt-sphere size the game actually uses: moves can scale or replace victim regions per bone
// (FGCharacterObj::GetModifiedVictimRegionForBone, called with the node in game memory)
static float HurtRadius(uintptr_t defender, uintptr_t node, float r) {
    if (!g_victimRegion || !defender || !node) return r;
    return g_victimRegion(defender, r, node);
}
// Projectiles / traps can exist (state 1) before and after they can hit: +0x384 is their "armed" flag
// (e.g. a projectile spawned before it becomes armed). Unarmed = startup (0) before, finished (2) after.
struct ProjArm { bool everArmed = false; int age = 0, first = -1, last = -1; uint64_t seenTick = 0; };
static std::map<std::pair<uintptr_t, int64_t>, ProjArm> g_projArm;
static int ProjState(const Tracked& t, int st, uint64_t tick, bool advance) {
    if (t.kind != OK_Projectile) return st;
    ProjArm& a = g_projArm[{t.ptr, (int64_t)t.createdTick}];
    int armed = 1;   // MKX: no armed flag found yet - the scan state decides
    if (advance && a.seenTick != tick) {
        a.seenTick = tick;
        if (armed && st == 1) { if (a.first < 0) a.first = a.age; a.last = a.age; }
        a.age++;
    }
    if (st != 1) return st;
    if (armed) { a.everArmed = true; return 1; }
    return a.everArmed ? 2 : 0;
}
static void ProjArmPrune(const Snapshot& s) {
    for (auto it = g_projArm.begin(); it != g_projArm.end();) {
        bool alive = false;
        for (auto& t : s.live) if (t.ptr == it->first.first && (int64_t)t.createdTick == it->first.second) { alive = true; break; }
        if (!alive) {
            if (g_logVerbose) Log("projarm: %llX lived %d frames, armed %d..%d", (unsigned long long)it->first.first, it->second.age, it->second.first, it->second.last);
            it = g_projArm.erase(it);
        } else ++it;
    }
}
static std::atomic<bool> g_armShapeTrace{false};
// diagnostics: per-frame memory of the next projectile P1 fires (armed with the 'projdump' command)
static std::atomic<bool> g_projDumpArm{false};
void ArmProjDump() { g_projDumpArm = true; }
static void ProjDump(const Snapshot& s) {
    static uintptr_t cur = 0;
    static FILE* f = nullptr;
    static int age = 0;
    if (!cur && g_projDumpArm) {
        for (auto& t : s.live) {
            if (t.kind != OK_Projectile) continue;
            uintptr_t owner = mem::ReadOr<uintptr_t>(t.ptr + off::COL_Owner, 0);
            if (!owner || owner != s.p[0].obj) continue;
            cur = t.ptr; age = 0; g_projDumpArm = false;
            char path[MAX_PATH];
            HMODULE self = nullptr;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&ProjDump, &self);
            GetModuleFileNameA(self, path, MAX_PATH);
            std::string dir = path;
            dir = dir.substr(0, dir.find_last_of("\\/") + 1);
            f = fopen((dir + "MKXFrameLab_projdump.txt").c_str(), "w");
            Log("projdump: following %llX", (unsigned long long)cur);
            break;
        }
    }
    if (!cur) return;
    bool alive = false;
    for (auto& t : s.live) if (t.ptr == cur) alive = true;
    if (!alive || !f) {
        if (f) fclose(f);
        f = nullptr; cur = 0;
        Log("projdump: done (%d frames)", age);
        return;
    }
    if (!s.advanced) return;
    static uint32_t buf[0x600 / 4];
    mem::ReadRaw(cur, buf, sizeof(buf));
    fprintf(f, "age %d frame %llu P1 %X P2 %X :", age++, (unsigned long long)s.frame, s.p[0].stateFlags, s.p[1].stateFlags);
    for (uint32_t v : buf) fprintf(f, " %X", v);
    fprintf(f, "\n");
}
// UE3 rotator (65536 = 360 degrees) -> axes (rows X, Y, Z) like FRotationMatrix
static void RotToAxes(const int r[3], float* m) {
    const float k = 6.28318530718f / 65536.f;
    float sp = sinf((r[0] & 0xFFFF) * k), cp = cosf((r[0] & 0xFFFF) * k);
    float sy = sinf((r[1] & 0xFFFF) * k), cy = cosf((r[1] & 0xFFFF) * k);
    float sr = sinf((r[2] & 0xFFFF) * k), cr = cosf((r[2] & 0xFFFF) * k);
    m[0] = cp * cy; m[1] = cp * sy; m[2] = sp; m[3] = 0;
    m[4] = sr * sp * cy - cr * sy; m[5] = sr * sp * sy + cr * cy; m[6] = -sr * cp; m[7] = 0;
    m[8] = -(cr * sp * cy + sr * sy); m[9] = cy * sr - cr * sp * sy; m[10] = cr * cp; m[11] = 0;
}
// MKX collision objects (CollisionObj::CheckCollisionAgainstCharacter): mType 0 sphere (radius +0x3FC),
// 1 cylinder (radius +0x3FC, height +0x404, along the object's rotation), 2 box (+0x3FC / +0x400 / +0x404),
// 3 beam (from the attachment point +0x3A0 to the object, radius +0x3FC).
static void SampleShapes(Snapshot& s) {
    ProjDump(s);
    for (auto& t : s.live) {
        if (t.kind != OK_CollisionObj && t.kind != OK_Melee && t.kind != OK_Projectile) continue;
        int ctype = mem::ReadOr<int>(t.ptr + off::COL_Type, -1);
        if (ctype < 0 || ctype > 3) continue;
        ShapeSnap sh;
        sh.ptr = t.ptr; sh.kind = t.kind;
        sh.state = ProjState(t, mem::ReadOr<int>(t.ptr + off::COL_State, -1), s.tick, false);
        uintptr_t owner = mem::ReadOr<uintptr_t>(t.ptr + off::COL_Owner, 0);
        for (int p = 0; p < 2; ++p) if (owner && owner == s.p[p].obj) sh.owner = p;
        float w = mem::ReadOr<float>(t.ptr + off::COL_Width, 0), l = mem::ReadOr<float>(t.ptr + off::COL_Length, 0),
              h = mem::ReadOr<float>(t.ptr + off::COL_Height, 0);
        uintptr_t actor = mem::ReadOr<uintptr_t>(t.ptr + 0x20, 0);
        if (!actor) continue;
        ((GetPosFn)g_sigs[S_GetPos].addr)(t.ptr, &sh.pos);
        int rot[3] = {0, 0, 0};
        mem::ReadRaw(actor + 0xF4, rot, sizeof(rot));
        alignas(16) float m[16] = {};
        RotToAxes(rot, m);
        m[12] = sh.pos.x; m[13] = sh.pos.y; m[14] = sh.pos.z; m[15] = 1;
        if (ctype == 0) {
            sh.type = 1; sh.radius = w;
        } else if (ctype == 2) {
            sh.type = 3; sh.radius = h; sh.width = w; sh.length = l;
            memcpy(sh.mat, m, sizeof(m)); sh.hasMat = true;
        } else if (ctype == 1) {
            // cylinder: drawn as a capsule from the object along its facing for its height
            sh.type = 4; sh.radius = w; sh.length = h;
            memcpy(sh.mat, m, sizeof(m)); sh.hasMat = true;
            Vec3 tip{sh.pos.x + m[0] * h, sh.pos.y + m[1] * h, sh.pos.z + m[2] * h};
            sh.mat[12] = sh.pos.x; sh.mat[13] = sh.pos.y; sh.mat[14] = sh.pos.z;
            sh.pos = tip;
        } else {
            sh.type = 4; sh.radius = w;
            alignas(16) float am[16];
            if (g_attachMatrix && g_attachMatrix(t.ptr + 0x3A0, am)) { memcpy(sh.mat, am, sizeof(am)); sh.hasMat = true; }
        }
        bool tested = false;
        {   // the outline the game itself tested for this object (this frame or the one before)
            std::lock_guard<std::mutex> l(g_testMtx);
            auto it = g_tested.find(t.ptr);
            if (it != g_tested.end() && s.frame >= it->second.frame && s.frame - it->second.frame <= 1 && it->second.poly.size() >= 3) {
                sh.poly = it->second.poly;
                sh.state = 1;   // it was hit-tested: live
                tested = true;
            }
        }
        if (!tested && t.kind != OK_Projectile) {
            // Attack collision objects are spawned at the start of the move and wait (state 0) until the
            // game starts hit-testing them; after they connect they linger spent (state 4) until the move
            // ends. Only the frames the game actually tests are live. A waiting object is kept as an
            // inactive preview, a spent one is not drawn at all.
            if (sh.state != 0) continue;
            sh.state = 0;
        }
        if (sh.poly.empty()) sh.poly = ComputeShape(t.ptr, ctype, w, l, h);   // not tested this frame: the same outline, computed
        if (g_armShapeTrace && sh.owner == 0 && (sh.state == 0 || sh.state == 1)) {
            g_armShapeTrace = false;
            uintptr_t a[4] = {t.ptr + off::COL_Width, t.ptr + off::COL_Height, t.ptr + off::COL_Type, t.ptr + off::COL_State};
            const char* n[4] = {"col+3FC (width/radius)", "col+404 (height)", "col+39C (type)", "col+390 (state)"};
            trace::Request(a, n, 2, std::string("shape_") + KindName(t.kind));
            Log("shape trace armed on %s %llX type %d w %.1f l %.1f h %.1f", KindName(t.kind), (unsigned long long)t.ptr, ctype, w, l, h);
        }
        s.shapes.push_back(sh);
    }
}

static VoidFn o_ProcDispatch = nullptr;
static DispatchFn o_Dispatch = nullptr;
static CamSetPosFn o_CamSetPos = nullptr;
static Ctor7 o_Ctor[6] = {};

static uintptr_t g_pGameInfo = 0;   // address of the global pointer
static uintptr_t g_pNames = 0;      // TArray<FNameEntry*>

static std::atomic<uintptr_t> g_camera{0};
static std::atomic<bool> g_frozen{false};
static std::atomic<int> g_stepPending{0};

// ----------------------------------------------------------------------------- tracking
static std::mutex g_trackMtx;
static std::vector<Tracked> g_tracked;
static std::deque<std::string> g_events;
static std::atomic<uint64_t> g_tick{0};
static std::atomic<bool> g_armMelee{false};
static int g_meleeTraceN = 0;
void ArmShapeTrace() { g_armShapeTrace = true; }
void ArmMeleeTrace() { g_armMelee = true; }
bool MeleeTraceArmed() { return g_armMelee; }

static void AddEvent(const std::string& s) {
    g_events.push_back(s);
    while (g_events.size() > 400) g_events.pop_front();
}

// Subclasses (weapon / special attack collision objects) have their own vtables: classify them
// through their RTTI names, cached per vtable.
static std::map<uintptr_t, int> g_vtKindCache;
static int KindOfVtable(uintptr_t vt) {
    for (auto& v : g_vts) if (v.va && v.va == vt) return v.kind;
    if (vt < g_base || vt >= g_base + mem::ModuleSize()) return OK_Unknown;
    auto it = g_vtKindCache.find(vt);
    if (it != g_vtKindCache.end()) return it->second;
    int k = OK_Unknown;
    uintptr_t col = mem::ReadOr<uintptr_t>(vt - 8, 0);
    char name[96] = {};
    if (col && mem::ReadOr<uint32_t>(col, 0) == 1) {
        uint32_t tdRva = mem::ReadOr<uint32_t>(col + 12, 0);
        if (tdRva && tdRva < mem::ModuleSize() && mem::ReadRaw(g_base + tdRva + 0x10, name, sizeof(name) - 1)) {
            name[sizeof(name) - 1] = 0;
            if (strstr(name, "PlayerCollisionInfo")) k = OK_PlayerCollisionInfo;
            else if (strstr(name, "Melee")) k = OK_Melee;
            else if (strstr(name, "Projectile")) k = OK_Projectile;
            else if (strstr(name, "CollisionResolution")) k = OK_Resolution;
            else if (strstr(name, "Collision")) k = OK_CollisionObj;
        }
    }
    g_vtKindCache[vt] = k;
    Log("vtable %llX rtti '%s' -> %s", (unsigned long long)(vt - g_base), name, k == OK_Unknown ? "ignored" : KindName(k));
    return k;
}

static void Track(int ctorIdx, uintptr_t self, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t ret) {
    std::lock_guard<std::mutex> l(g_trackMtx);
    uintptr_t rva = (ret >= g_base && ret < g_base + mem::ModuleSize()) ? ret - g_base : ret;
    for (auto& t : g_tracked) {
        if (t.ptr == self) {
            // outer (derived) constructors return last - keep their caller + args
            t.callerRva = rva; t.arg1 = a1; t.arg2 = a2; t.arg3 = a3;
            t.createdTick = g_tick.load();
            return;
        }
    }
    Tracked t;
    t.ptr = self; t.callerRva = rva; t.arg1 = a1; t.arg2 = a2; t.arg3 = a3;
    t.createdTick = g_tick.load();
    g_tracked.push_back(t);
    if (g_tracked.size() > 4096) g_tracked.erase(g_tracked.begin());
    (void)ctorIdx;
}

#define CTOR_HOOK(I)                                                                                  \
    static uintptr_t h_Ctor##I(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g) { \
        uintptr_t r = o_Ctor[I](a, b, c, d, e, f, g);                                                 \
        Track(I, a, b, c, d, (uintptr_t)__builtin_return_address(0));                                 \
        return r;                                                                                     \
    }
CTOR_HOOK(0) CTOR_HOOK(1) CTOR_HOOK(2) CTOR_HOOK(3) CTOR_HOOK(4) CTOR_HOOK(5)
static Ctor7 g_ctorHooks[6] = {h_Ctor0, h_Ctor1, h_Ctor2, h_Ctor3, h_Ctor4, h_Ctor5};

// ----------------------------------------------------------------------------- game accessors
// GameInfo+0x10: 1 while the game's pause menu is open (found by diffing memory paused vs running)
bool GamePaused() {
    // GI+0x10 = paused, GI+0x14 = why: 0x200 pause menu ... 7 = halted by our own freeze / frame step
    // (F5 / F6), which must keep the overlay visible
    uintptr_t gi = GameInfo();
    if (!gi || mem::ReadOr<int>(gi + off::GI_PausedFlag, 0) == 0) return false;
    return mem::ReadOr<int>(gi + off::GI_PausedWhy, 0) != 7 && !g_frozen.load() && g_stepPending.load() == 0;
}

uintptr_t GameInfo() {
    if (!g_pGameInfo) return 0;
    return mem::ReadOr<uintptr_t>(g_pGameInfo, 0);
}

uintptr_t GetInfo(int plr) {
    uintptr_t gi = GameInfo();
    if (!gi || plr < 0 || plr > 1) return 0;
    return mem::ReadOr<uintptr_t>(gi + off::GI_Players + plr * 8, 0);   // FG_GAME_INFO::mPlayers
}

uintptr_t GetObj(int plr) {
    if (!GetInfo(plr) || !g_sigs[S_GetObj].addr) return 0;
    return ((GetObjFn)g_sigs[S_GetObj].addr)(plr);   // GetFGCharacterObj(PLAYER_NUM)
}

static std::mutex g_nameMtx;
std::string BoneName(int idx) {
    if (!g_pNames || idx < 0) return "?";
    uintptr_t data = mem::ReadOr<uintptr_t>(g_pNames, 0);
    int count = mem::ReadOr<int>(g_pNames + 8, 0);
    if (!data || idx >= count) return "?";
    uintptr_t e = mem::ReadOr<uintptr_t>(data + (uintptr_t)idx * 8, 0);
    if (!e) return "?";
    char buf[129] = {};
    if (!mem::ReadRaw(e + 0x10, buf, 128)) return "?";
    buf[128] = 0;
    return buf;
}

// bone FName cache per character
struct BoneCache { uintptr_t obj = 0, comp = 0; int count = 0; std::vector<uint64_t> names; uint32_t charKey = 0; };
static BoneCache g_boneCache[2];
static std::atomic<bool> g_sampleBones{true};

static void RefreshBones(int p, uintptr_t obj) {
    BoneCache& c = g_boneCache[p];
    uintptr_t comp = mem::ReadOr<uintptr_t>(obj + off::CO_SkelComp, 0);
    uintptr_t mesh = comp ? mem::ReadOr<uintptr_t>(comp + 0x2C0, 0) : 0;      // SkeletalMesh
    uintptr_t skel = mesh ? mem::ReadOr<uintptr_t>(mesh + 0x290, 0) : 0;      // USkeleton
    int count = skel ? mem::ReadOr<int>(skel + 0xB0 + 8, 0) : 0;            // RefSkeleton.BoneNames.Num
    if (c.obj == obj && c.comp == comp && c.count == count) return;
    c.obj = obj; c.comp = comp; c.count = count; c.names.clear();
    if (!comp || count <= 0 || count > 400 || !g_sigs[S_GetBoneName].addr) return;
    for (int i = 0; i < count; ++i) {
        uint64_t fn = 0;
        ((GetBoneNameFn)g_sigs[S_GetBoneName].addr)(comp, &fn, i);
        c.names.push_back(fn);
    }
    // character identity that survives restarts: hash of the skeleton's bone names
    uint32_t h = 2166136261u;
    for (uint64_t fn : c.names) {
        std::string n = BoneName((int)(fn & 0xFFFFFFFF));
        for (char ch : n) { h ^= (uint8_t)ch; h *= 16777619u; }
        h ^= '|'; h *= 16777619u;
    }
    c.charKey = h;
    Log("P%d: %d bones cached (comp %llX), character key %08X", p + 1, count, (unsigned long long)comp, h);
}

// ----------------------------------------------------------------------------- snapshot
static std::mutex g_snapMtx;
static Snapshot g_snap;

Snapshot GetSnapshot() {
    std::lock_guard<std::mutex> l(g_snapMtx);
    return g_snap;
}
static std::deque<Snapshot> g_frameQ;   // every simulated frame, consumed by the frame meter (render thread)
std::vector<Snapshot> TakeFrames() {
    std::lock_guard<std::mutex> l(g_snapMtx);
    std::vector<Snapshot> v(std::make_move_iterator(g_frameQ.begin()), std::make_move_iterator(g_frameQ.end()));
    g_frameQ.clear();
    return v;
}

// ----------------------------------------------------------------------------- recorder
static const size_t REC_OBJ = 0x20000, REC_INFO = 0x2000, REC_GI = 0x2000, REC_CAM = 0x800, REC_COBJ = 0x1000;
static std::mutex g_recMtx;
static std::atomic<bool> g_recording{false};
static std::atomic<int> g_recLeft{0}, g_recTotal{0};
static std::vector<uint8_t> g_recBuf;
static std::string g_recLabel, g_recLast;
static std::vector<std::string> g_recEvents;
static std::string g_dir;

static void Put(std::vector<uint8_t>& b, const void* p, size_t n) {
    const uint8_t* s = (const uint8_t*)p;
    b.insert(b.end(), s, s + n);
}
template <class T> static void PutV(std::vector<uint8_t>& b, const T& v) { Put(b, &v, sizeof(T)); }
static void PutMem(std::vector<uint8_t>& b, uintptr_t addr, size_t n) {
    size_t at = b.size();
    b.resize(at + n, 0);
    if (!addr) return;
    // page-wise so a hole doesn't zero the whole block
    for (size_t o = 0; o < n;) {
        size_t chunk = 0x1000 - ((addr + o) & 0xFFF);
        if (chunk > n - o) chunk = n - o;
        mem::ReadRaw(addr + o, &b[at + o], chunk);
        o += chunk;
    }
}

void StartRecording(int ticks, const std::string& label) {
    std::lock_guard<std::mutex> l(g_recMtx);
    if (g_recording) return;
    g_recBuf.clear();
    g_recBuf.reserve((size_t)ticks * (2 * (REC_OBJ + REC_INFO) + REC_GI + REC_CAM + 8 * REC_COBJ));
    g_recEvents.clear();
    g_recLabel = label.empty() ? "rec" : label;
    g_recTotal = ticks;
    g_recLeft = ticks;
    const char magic[8] = {'I', 'J', '2', 'R', 'E', 'C', '0', '2'};
    Put(g_recBuf, magic, 8);
    PutV(g_recBuf, (uint64_t)g_base);
    uint32_t sizes[5] = {(uint32_t)REC_OBJ, (uint32_t)REC_INFO, (uint32_t)REC_GI, (uint32_t)REC_CAM, (uint32_t)REC_COBJ};
    Put(g_recBuf, sizes, sizeof(sizes));
    g_recording = true;
    Log("Recording %d ticks (%s)", ticks, g_recLabel.c_str());
}
bool Recording() { return g_recording; }
int RecordProgress() { return g_recTotal ? (int)(100 * (g_recTotal - g_recLeft) / g_recTotal) : 0; }
std::string LastRecordingPath() { std::lock_guard<std::mutex> l(g_recMtx); return g_recLast; }

static void FinishRecording() {
    std::vector<uint8_t> buf;
    std::vector<std::string> events;
    std::string label;
    {
        std::lock_guard<std::mutex> l(g_recMtx);
        buf.swap(g_recBuf);
        events.swap(g_recEvents);
        label = g_recLabel;
        g_recording = false;
    }
    // bone names for both players
    std::string names;
    for (int p = 0; p < 2; ++p) {
        names += "P" + std::to_string(p + 1) + " bones:\n";
        for (size_t i = 0; i < g_boneCache[p].names.size(); ++i)
            names += "  " + std::to_string(i) + " " + std::to_string((int)(g_boneCache[p].names[i] & 0xFFFFFFFF)) + " " +
                     BoneName((int)(g_boneCache[p].names[i] & 0xFFFFFFFF)) + "\n";
    }
    std::thread([buf = std::move(buf), events = std::move(events), names, label]() {
        SYSTEMTIME t;
        GetLocalTime(&t);
        char stamp[64];
        snprintf(stamp, sizeof(stamp), "%02d%02d%02d", t.wHour, t.wMinute, t.wSecond);
        std::string bin = g_dir + "MKXFrameLab_" + label + "_" + stamp + ".bin";
        std::string txt = g_dir + "MKXFrameLab_" + label + "_" + stamp + ".txt";
        FILE* f = fopen(bin.c_str(), "wb");
        if (f) { fwrite(buf.data(), 1, buf.size(), f); fclose(f); }
        FILE* g = fopen(txt.c_str(), "w");
        if (g) {
            fprintf(g, "base %llX\n", (unsigned long long)g_base);
            fputs(names.c_str(), g);
            fputs("events:\n", g);
            for (auto& e : events) fprintf(g, "%s\n", e.c_str());
            fclose(g);
        }
        Log("Recording written: %s (%zu bytes)", bin.c_str(), buf.size());
        std::lock_guard<std::mutex> l(g_recMtx);
        g_recLast = bin;
    }).detach();
}

static void RecordTick(const Snapshot& s) {
    std::lock_guard<std::mutex> l(g_recMtx);
    auto& b = g_recBuf;
    PutV(b, (uint32_t)0x4B434954); // 'TICK'
    PutV(b, (uint64_t)s.tick);
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    PutV(b, (uint64_t)q.QuadPart);
    for (int p = 0; p < 2; ++p) {
        const PlayerSnap& ps = s.p[p];
        PutV(b, (uint64_t)ps.info);
        PutV(b, (uint64_t)ps.obj);
        PutV(b, ps.pos);
        PutV(b, (uint32_t)ps.bones.size());
        for (auto& bs : ps.bones) { PutV(b, (int32_t)bs.nameIndex); PutV(b, bs.pos); }
        PutMem(b, ps.obj, REC_OBJ);
        PutMem(b, ps.info, REC_INFO);
        PutV(b, (uint64_t)ps.strikeList);
        PutMem(b, ps.strikeList, 0x400);
    }
    uintptr_t gi = GameInfo();
    PutV(b, (uint64_t)gi);
    PutMem(b, gi, REC_GI);
    PutV(b, (uint64_t)s.camPtr);
    PutV(b, s.camPos);
    PutV(b, s.camRot);
    PutV(b, s.camFov);
    PutMem(b, s.camPtr, REC_CAM);
    PutV(b, (uint32_t)s.live.size());
    for (auto& t : s.live) {
        PutV(b, (uint64_t)t.ptr);
        PutV(b, (uint64_t)t.vtable);
        PutV(b, (int32_t)t.kind);
        PutV(b, (uint64_t)t.createdTick);
        PutV(b, (uint64_t)t.callerRva);
        PutV(b, (uint64_t)t.arg1);
        PutV(b, (uint64_t)t.arg2);
        PutV(b, (uint64_t)t.arg3);
        PutMem(b, t.ptr, REC_COBJ);
    }
}

// ----------------------------------------------------------------------------- tick
void PollStep();
static void DoPadFix();
static std::atomic<bool> g_padFixReq{false};
static void OnTick() {
    if (g_padFixReq.exchange(false)) DoPadFix();
    uint64_t tick = ++g_tick;
    Snapshot s;
    s.tick = tick;
    static uint32_t lastGf = 0;
    uint32_t gf = 0;
    if (uintptr_t gi = GameInfo()) gf = mem::ReadOr<uint32_t>(gi + off::GI_TickCtr, 0);
    s.frame = gf;
    s.advanced = gf != lastGf;
    lastGf = gf;
    {   // game mode (FG_GAME_INFO::mGameMode): only Practice unlocks the overlay
        uintptr_t gi = GameInfo();
        int gm = gi ? mem::ReadOr<int>(gi + off::GI_GameMode, -1) : -1;
        if (gm == off::GM_Practice) s.gameMode = "practice";
        else if (gi) s.gameMode = "mode" + std::to_string(gm);
    }
    for (int p = 0; p < 2; ++p) {
        PlayerSnap& ps = s.p[p];
        ps.info = GetInfo(p);
        ps.obj = ps.info ? GetObj(p) : 0;
        if (!ps.obj) continue;
        ps.valid = true;
        ((GetPosFn)g_sigs[S_GetPos].addr)(ps.obj, &ps.pos);
        if (g_sampleBones && g_sigs[S_GetBonePos].addr) {
            RefreshBones(p, ps.obj);
            ps.charKey = g_boneCache[p].charKey;
            {   // character identity: the character's asset name (skeletons are shared between characters)
                static uintptr_t lastInfo[2], lastStr[2];
                static std::string name[2];
                uintptr_t sp = mem::ReadOr<uintptr_t>(ps.info + off::PI_CharName, 0);
                if (ps.info != lastInfo[p] || sp != lastStr[p]) {
                    lastInfo[p] = ps.info; lastStr[p] = sp;
                    char b[64] = {};
                    SIZE_T got = 0;
                    std::string n;
                    if (sp > 0x10000 && ReadProcessMemory(GetCurrentProcess(), (void*)sp, b, sizeof(b) - 1, &got)) {
                        int k = 0;
                        while (k < 63 && b[k] > 32 && b[k] < 127) ++k;
                        if (k >= 3 && b[k] == 0) n.assign(b, k);
                    }
                    if (n != name[p]) { name[p] = n; Log("P%d: character %s", p + 1, n.empty() ? "?" : n.c_str()); }
                }
                ps.charName = name[p];
                {   // name of the special move in progress (the game keeps a pointer to the move's script entry)
                    uintptr_t e = 0;   // MKX: special move name not mapped yet
                    uintptr_t np = e > 0x10000 ? mem::ReadOr<uintptr_t>(e, 0) : 0;
                    char b[64] = {};
                    if (np > 0x10000 && mem::ReadRaw(np, b, sizeof(b) - 1)) {
                        int k = 0;
                        while (k < 63 && b[k] > 32 && b[k] < 127) ++k;
                        if (k >= 4 && b[k] == 0 && strchr(b, '_')) ps.moveName.assign(b, k);
                    }
                }
                if (!name[p].empty()) {
                    uint32_t h = 2166136261u;
                    for (char ch : name[p]) { h ^= (uint8_t)ch; h *= 16777619u; }
                    ps.charKey = h;
                }
            }
            const BoneCache& c = g_boneCache[p];
            ps.bones.reserve(c.names.size());
            for (uint64_t fn : c.names) {
                Vec3 v{};
                ((GetBonePosFn)g_sigs[S_GetBonePos].addr)(ps.obj, &fn, &v);
                ps.bones.push_back({(int)(fn & 0xFFFFFFFF), v});
            }
        }
    }
    uintptr_t cam = g_camera.load();
    if (cam) {
        s.camPtr = cam;
        s.camValid = mem::Read(cam + off::CAM_Pos, s.camPos) && mem::Read(cam + off::CAM_Rot, s.camRot) && mem::Read(cam + off::CAM_Fov, s.camFov);
    }
    {
        std::lock_guard<std::mutex> l(g_trackMtx);
        // drop dead objects, classify live ones
        std::vector<Tracked> keep;
        keep.reserve(g_tracked.size());
        for (auto& t : g_tracked) {
            uintptr_t vt = 0;
            if (!mem::Read(t.ptr, vt)) continue;
            int k = KindOfVtable(vt);
            if (k == OK_Unknown) {
                if (tick - t.createdTick > 2) continue; // vtable not one of ours (destroyed / reused)
            } else if (t.kind != k) {
                t.kind = k;
                char e[256];
                snprintf(e, sizeof(e), "tick %llu  new %-22s %llX  caller rva %llX  args %llX %llX %llX",
                         (unsigned long long)tick, KindName(k), (unsigned long long)t.ptr, (unsigned long long)t.callerRva,
                         (unsigned long long)t.arg1, (unsigned long long)t.arg2, (unsigned long long)t.arg3);
                AddEvent(e);
                if (k == OK_Melee && g_armMelee) {
                    g_armMelee = false;
                    uintptr_t a[4] = {t.ptr + off::COL_State, t.ptr + 0x380, t.ptr + off::COL_Type, t.ptr + 0x54};
                    const char* n[4] = {"melee+390 (state)", "melee+380 (spec obj)", "melee+39C (type)", "melee+54 (move id)"};
                    trace::Request(a, n, 2, "melee" + std::to_string(++g_meleeTraceN));
                }
                if (g_recording) { std::lock_guard<std::mutex> rl(g_recMtx); g_recEvents.push_back(e); }
            }
            t.vtable = vt;
            keep.push_back(t);
        }
        g_tracked.swap(keep);
        for (auto& t : g_tracked) if (t.kind != OK_Unknown) s.live.push_back(t);
    }
    ProjArmPrune(s);
    movelist::SetPlayers(s.p[0].obj, s.p[1].obj);
    // per-fighter collision data
    for (int p = 0; p < 2; ++p) {
        PlayerSnap& ps = s.p[p];
        if (!ps.obj) continue;
        ps.stateFlags = mem::ReadOr<uint32_t>(ps.obj + off::CO_State, 0);
        ps.moveId = mem::ReadOr<int32_t>(ps.obj + off::CO_MoveID, -1);
        {   // the AI's view of the fighter: current attack hash / id (FGCharacterObj+0xB010 -> AICharacterSensoryInfo)
            uintptr_t so = mem::ReadOr<uintptr_t>(ps.obj + 0xB020, 0);
            uint32_t inst = mem::ReadOr<uint32_t>(ps.obj + 0xB028, 0xFFFFFFFFu);
            if (so && inst != 0xFFFFFFFFu && mem::ReadOr<uint32_t>(so + 8, 0) == inst) {
                ps.attackHash = mem::ReadOr<uint32_t>(so + 0x108, 0);
                ps.attackId = mem::ReadOr<uint32_t>(so + 0x118, 0);
                ps.lastMoveId = mem::ReadOr<uint32_t>(so + 0x120, 0);
            }
            // the attack hash identifies the move (and each hit of a string) exactly: use it as the move id
            ps.moveId = ps.attackHash ? (int32_t)ps.attackHash : -1;
            // the move list is read when a fighter appears, which can be before the game has filled in their
            // attack table (character select -> fight): read it again whenever a move is not in it
            if (ps.attackHash) {
                static ULONGLONG lastRescan = 0;
                movelist::Entry e;
                ULONGLONG now = GetTickCount64();
                if (!movelist::ByHash(ps.attackHash, e) && now - lastRescan > 1000 && !movelist::Scanning()) {
                    lastRescan = now;
                    movelist::RequestScan();
                }
            }
            static uint32_t lh[2], li[2], lm[2];
            if (g_logVerbose && (ps.attackHash != lh[p] || ps.attackId != li[p] || ps.lastMoveId != lm[p])) {
                lh[p] = ps.attackHash; li[p] = ps.attackId; lm[p] = ps.lastMoveId;
                Log("ai: P%d f%llu hash %08X attack id %X last move %X state %X obj+54 %X", p + 1, (unsigned long long)s.frame, ps.attackHash,
                    ps.attackId, ps.lastMoveId, ps.stateFlags, (unsigned)ps.moveId);
            }
        }
        {   // the fighter's PlayerCollisionInfo hangs off FGCharacterObj::mCollisionInfo (item at +0x1BE0, object at +0x10)
            uintptr_t pci = mem::ReadOr<uintptr_t>(ps.obj + off::CO_CollisionInfo + 0x10, 0);
            uintptr_t vtPci = 0;
            for (auto& v : g_vts) if (v.kind == OK_PlayerCollisionInfo) vtPci = v.va;
            if (pci && vtPci && mem::ReadOr<uintptr_t>(pci, 0) == vtPci) ps.pci = pci;
        }
        for (auto& t : s.live) {
            if (!ps.pci && t.kind == OK_PlayerCollisionInfo && mem::ReadOr<uintptr_t>(t.ptr + off::PCI_Owner, 0) == ps.obj) ps.pci = t.ptr;
            if (t.kind == OK_Melee && mem::ReadOr<uintptr_t>(t.ptr + off::COL_Owner, 0) == ps.obj) {
                if (!ps.melee || t.createdTick >= 0) { ps.melee = t.ptr; ps.meleeState = mem::ReadOr<int>(t.ptr + off::COL_State, -1); }
            }
            if ((t.kind == OK_Melee || t.kind == OK_CollisionObj || t.kind == OK_Projectile) && mem::ReadOr<uintptr_t>(t.ptr + off::COL_Owner, 0) == ps.obj)
                ps.hitObjs.push_back({t.ptr, ProjState(t, mem::ReadOr<int>(t.ptr + off::COL_State, -1), s.tick, s.advanced)});
        }
        if (!ps.pci) ps.pci = FindPciByScan(ps.obj, ps.info);   // e.g. after a hot reload: its constructor ran before we loaded
        if (!ps.pci) continue;
        // hurt spheres (victim regions) used by the hit test: CollisionNode {FName, bone index, enabled, center, radius, prev}
        struct Raw { int nameIdx, nameNum, bone, enabled; float x, y, z, r; float px, py, pz, pr; };
        int n = mem::ReadOr<int>(ps.pci + off::PCI_NumVictim, 0);
        uintptr_t arr = mem::ReadOr<uintptr_t>(ps.pci + off::PCI_Victim, 0);
        if (n > 0 && n <= 64 && arr) {
            static Raw raw[64];
            if (mem::ReadRaw(arr, raw, sizeof(Raw) * n))
                for (int i = 0; i < n; ++i) {
                    if (!raw[i].enabled) continue;
                    ps.hurt.push_back({raw[i].nameIdx, raw[i].bone, i, OnFightLine(Vec3{raw[i].x, raw[i].y, raw[i].z}),
                                       HurtRadius(ps.obj, arr + (uintptr_t)i * sizeof(Raw), raw[i].r)});
                }
        }
        g_pciOf[p] = ps.pci;
        g_objOf[p] = ps.obj;
        ps.strikeList = mem::ReadOr<uintptr_t>(ps.pci + off::PCI_Attack, 0);
        {
            // Hitboxes tested during the last simulated frame. Kept while the game is frozen
            // or slowed (no new simulation frame), replaced/cleared when the next frame runs.
            static std::vector<HurtPoint> shown[2];
            std::lock_guard<std::mutex> l(g_capMtx);
            if (s.advanced) {
                if (g_cap[p].seq > g_capSeen[p]) { shown[p] = g_cap[p].pts; g_capSeen[p] = g_cap[p].seq; }
                else shown[p].clear();
            }
            ps.strike = shown[p];
            ps.striking = !shown[p].empty();
        }
    }

    // ---- non-melee attack objects (projectiles, lasso, thrown weapons): owner + lifecycle ----
    {
        static int logBudget = 4000;
        auto LOGOK = [&]() { return g_logVerbose && logBudget-- > 0; };
        struct ObjInfo { int owner = -1; int ownerOff = -1; int lastState = -99; uint64_t born = 0; int kind = 0; uintptr_t vt = 0; };
        static std::map<uintptr_t, ObjInfo> seen;
        static std::map<uintptr_t, int> ownerOffByVt;   // vtable -> offset of owner pointer (-2 = none found)
        std::map<uintptr_t, ObjInfo> now;
        for (auto& t : s.live) {
            if (t.kind != OK_Projectile && t.kind != OK_CollisionObj && t.kind != OK_Resolution) continue;
            ObjInfo oi;
            auto it = seen.find(t.ptr);
            if (it != seen.end() && it->second.vt == t.vtable) oi = it->second;
            else {
                oi.born = s.frame; oi.kind = t.kind; oi.vt = t.vtable;
                // find which player this object belongs to: scan for a pointer to a fighter object / info
                std::string offs;
                uint8_t buf[0x800];
                if (mem::ReadRaw(t.ptr, buf, sizeof(buf))) {
                    for (int o = 8; o < (int)sizeof(buf); o += 8) {
                        uintptr_t v; memcpy(&v, buf + o, 8);
                        for (int p = 0; p < 2; ++p) {
                            if (!v) continue;
                            bool isObj = v == s.p[p].obj, isInfo = v == s.p[p].info;
                            if (isObj || isInfo) {
                                char tmp[32]; snprintf(tmp, sizeof(tmp), " P%d%s@%X", p + 1, isObj ? "obj" : "info", o);
                                offs += tmp;
                                if (oi.owner < 0) { oi.owner = p; oi.ownerOff = o; }
                            }
                        }
                    }
                }
                if (LOGOK()) Log("obj+ %s %llX vt %llX f%llu P1flags %X state %d type %d r %.2f owners:%s", KindName(t.kind), (unsigned long long)t.ptr,
                    (unsigned long long)(t.vtable - g_base), (unsigned long long)s.frame, s.p[0].stateFlags,
                    mem::ReadOr<int>(t.ptr + off::COL_State, -1), mem::ReadOr<int>(t.ptr + off::COL_Type, 0), mem::ReadOr<float>(t.ptr + off::COL_Width, 0), offs.c_str());
            }
            if (t.kind == OK_Resolution) {
                // a collision resolution object appears on the frame an attack connects
                if (it == seen.end() || it->second.vt != t.vtable) {
                    // CollisionResolutionObj::mCollisionResultSpec (+0x90): attacker item at +0x28 (object at +0xC8)
                    uintptr_t atk = mem::ReadOr<uintptr_t>(t.ptr + 0xC8, 0);
                    // victim at +0xA8: a projectile / trap landing on the floor or a stage object resolves too,
                    // with no fighter as the victim - that is not contact with the opponent
                    uintptr_t vic = mem::ReadOr<uintptr_t>(t.ptr + 0xA8, 0);
                    bool known = false;
                    for (int p = 0; p < 2; ++p)
                        if (atk && atk == s.p[p].obj) { known = true; if (!vic || vic == s.p[1 - p].obj) s.p[p].contactNew = true; }
                    if (!known) for (int p = 0; p < 2; ++p) s.p[p].contactNew = true;
                    if (LOGOK()) Log("contact f%llu P1flags %X P2flags %X", (unsigned long long)s.frame, s.p[0].stateFlags, s.p[1].stateFlags);
                }
                now[t.ptr] = oi;
                continue;
            }
            int st = mem::ReadOr<int>(t.ptr + off::COL_State, -1);
            if (st != oi.lastState && it != seen.end() && LOGOK())
                if (g_logVerbose) Log("obj~ %llX f%llu state %d -> %d (age %llu)", (unsigned long long)t.ptr, (unsigned long long)s.frame, oi.lastState, st,
                    (unsigned long long)(s.frame - oi.born));
            oi.lastState = st;
            now[t.ptr] = oi;
            if (oi.owner >= 0 && oi.owner < 2) {
                PlayerSnap& ps = s.p[oi.owner];
                ps.projAlive = true;
                if (st == 1) ps.projActive = true;   // only while its hitbox is live (1); 4 = already connected
            }
        }
        for (auto& kv : seen)
            if (!now.count(kv.first) && LOGOK())
                if (g_logVerbose) Log("obj- %llX f%llu age %llu", (unsigned long long)kv.first, (unsigned long long)s.frame, (unsigned long long)(s.frame - kv.second.born));
        seen.swap(now);
        (void)ownerOffByVt;
    }
    SampleShapes(s);
    if (s.advanced) ibuf::OnTick(gf, s.p[0].valid, s.p[0].stateFlags, s.p[0].obj, false);

    if (g_recording) {
        RecordTick(s);
        if (--g_recLeft <= 0) FinishRecording();
    }
    if (s.advanced) {
        bot::OnSimFrame(s);   // scripted input runs on the game thread, exactly once per simulated frame
        std::lock_guard<std::mutex> l(g_snapMtx);
        g_frameQ.push_back(s);
        while (g_frameQ.size() > 60) g_frameQ.pop_front();
    }
    {
        std::lock_guard<std::mutex> l(g_snapMtx);
        g_snap = std::move(s);
    }
    // frame step: let exactly one tick through, then freeze again
    PollStep();
}

static void h_ProcDispatch() {
    o_ProcDispatch();
    OnTick();
}

static int g_dispatchSlot = 0xD0;
static void h_Dispatch(uintptr_t ptr, int a2) {
    // Ported from MKXHook's HookDispatch "Freeze World" (ermaccer)
    int v = g_frozen ? a2 : mem::ReadOr<int>(ptr + 0x18, a2);
    int cur = mem::ReadOr<int>(ptr + 0x14, v);
    if (cur == v) return;
    *(int*)(ptr + 0x14) = v;
    uintptr_t vt = *(uintptr_t*)ptr;
    ((DispatchFn)(*(uintptr_t*)(vt + g_dispatchSlot)))(ptr, v);
}

// This function is a generic "set location" that other actors call too (e.g. stage props).
// Only follow objects whose C++ class name contains "Camera".
static std::mutex g_camVtMtx;
static std::map<uintptr_t, bool> g_camVtOk;
static bool IsCameraClass(uintptr_t obj) {
    uintptr_t vt = mem::ReadOr<uintptr_t>(obj, 0);
    if (!vt) return false;
    std::lock_guard<std::mutex> l(g_camVtMtx);
    auto it = g_camVtOk.find(vt);
    if (it != g_camVtOk.end()) return it->second;
    char name[96] = {};
    uintptr_t col = mem::ReadOr<uintptr_t>(vt - 8, 0);
    if (col && mem::ReadOr<uint32_t>(col, 0) == 1) {
        uint32_t td = mem::ReadOr<uint32_t>(col + 12, 0);
        if (td && td < mem::ModuleSize()) mem::ReadRaw(g_base + td + 0x10, name, sizeof(name) - 1);
    }
    bool ok = strcmp(name, ".?AVAMKCamera@@") == 0;   // the fight camera (view in CameraCache.POV at +0x414..)
    g_camVtOk[vt] = ok;
    Log("SetPosition caller class '%s' -> %s", name, ok ? "camera" : "ignored");
    return ok;
}

static int h_CamSetPos(uintptr_t cam, Vec3* pos) {
    if (IsCameraClass(cam)) g_camera = cam;
    return o_CamSetPos(cam, pos);
}

static std::atomic<uint32_t> g_stepFrom{0};
static uint32_t SimFrame() {
    uintptr_t gi = GameInfo();
    return gi ? mem::ReadOr<uint32_t>(gi + off::GI_TickCtr, 0) : 0;
}
void SetFrozen(bool f) { g_stepPending = 0; g_frozen = f; }

// ---- practice mode controls (this build's PRACTICE_INFO, MK10.pdb; checked by prologue bytes before use)
static bool CodeMatches(uintptr_t rva, const uint8_t* b, size_t n) {
    uint8_t got[16] = {};
    return g_base && mem::ReadRaw(g_base + rva, got, n) && memcmp(got, b, n) == 0;
}
static uintptr_t PracticeInfo() {
    static const uint8_t pGet[] = {0x48, 0x8D, 0x05, 0xC9, 0x96, 0xD9, 0x02, 0xC3};
    if (!CodeMatches(0x582610, pGet, sizeof(pGet))) return 0;
    return ((uintptr_t(*)())(g_base + 0x582610))();
}
bool PracticeReset() {   // PRACTICE_INFO::ResetMatch(0): the pause menu's "Reset"
    static const uint8_t p[] = {0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF9};
    uintptr_t pi = PracticeInfo();
    if (!pi || !CodeMatches(0x58F0E0, p, sizeof(p))) { Log("practice: reset unavailable"); return false; }
    ((void(*)(uintptr_t, uint32_t))(g_base + 0x58F0E0))(pi, 0);
    return true;
}
bool PracticeSetGame(int setting, int value) {   // PRACTICE_INFO::ModifyGameplaySettings (1 = super meter: 2 infinite)
    static const uint8_t p[] = {0x83, 0xFA, 0x0D, 0x0F, 0x87, 0xC8, 0x01, 0x00, 0x00};
    uintptr_t pi = PracticeInfo();
    if (!pi || !CodeMatches(0x58B340, p, sizeof(p))) return false;
    ((void(*)(uintptr_t, int, int))(g_base + 0x58B340))(pi, setting, value);
    return true;
}
bool PracticeSetDrone(int player, int setting, int value) {   // PRACTICE_INFO::SetDroneSetting (3 = block mode)
    static const uint8_t p[] = {0x41, 0x83, 0xF8, 0x0D, 0x0F, 0x87, 0x53, 0x01, 0x00, 0x00};
    uintptr_t pi = PracticeInfo();
    if (!pi || !CodeMatches(0x592A00, p, sizeof(p))) return false;
    ((void(*)(uintptr_t, int, int, int))(g_base + 0x592A00))(pi, player, setting, value);
    return true;
}

// "Please reconnect the controller": the game keeps a list of pulled pads (FGSaveDataManager::PollControllerPulls)
// and clears it in RemovedPulledGamepadFromList when one comes back. Scripted input is a pad that never left,
// so the bot clears the list itself (game thread).
void RequestPadFix() { g_padFixReq = true; }
static void DoPadFix() {
    static const uint8_t p[] = {0x89, 0x4C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x50};
    if (!CodeMatches(0x13C950, p, sizeof(p))) { Log("padfix: unavailable"); return; }
    int n = mem::ReadOr<int>(g_base + 0x33034E8, 0);
    uintptr_t arr = mem::ReadOr<uintptr_t>(g_base + 0x33034E0, 0);
    for (int k = 0; k < 8 && n > 0 && arr; ++k) {
        int idx = mem::ReadOr<int>(arr, -1);
        Log("padfix: clearing pulled pad %d (%d in list)", idx, n);
        if (idx < 0 || idx > 7) break;
        ((void(*)(int))(g_base + 0x13C950))(idx);
        n = mem::ReadOr<int>(g_base + 0x33034E8, 0);
        arr = mem::ReadOr<uintptr_t>(g_base + 0x33034E0, 0);
    }
}
void PracticeDump() {
    {   // pads: GAMEPAD array (Input::GetGamepad) and the pause menu's owner
        uintptr_t gp = g_base + 0x33A3750;
        uintptr_t gi = mem::ReadOr<uintptr_t>(g_base + 0x30DBA00, 0);
        uintptr_t owner = gi ? mem::ReadOr<uintptr_t>(gi + 0xC0, 0) : 0;
        Log("pads: base %llX pause owner %llX (slot %d)", (unsigned long long)g_base, (unsigned long long)owner,
            owner >= gp && owner < gp + 12 * 0x2E8 ? (int)((owner - gp) / 0x2E8) : -1);
        for (int i = 0; i < 12; ++i) {
            uintptr_t g = gp + (uintptr_t)i * 0x2E8;
            Log("pad %2d: idx %d player %llX linked %llX flags %X bits %08X %08X", i, mem::ReadOr<int>(g, -9),
                (unsigned long long)mem::ReadOr<uintptr_t>(g + 8, 0), (unsigned long long)mem::ReadOr<uintptr_t>(g + 0x10, 0),
                mem::ReadOr<int>(g + 0x18, 0), mem::ReadOr<uint32_t>(g + 0x1C, 0), mem::ReadOr<uint32_t>(g + 0x20, 0));
        }
    }
    uintptr_t pi = PracticeInfo();
    if (!pi) { Log("practice: no PRACTICE_INFO"); return; }
    uint8_t b[0xA0] = {};
    mem::ReadRaw(pi, b, sizeof(b));
    std::string g = "practice: gameplay", d[2] = {"practice: drone P1", "practice: drone P2"};
    char t[8];
    for (int i = 0; i < 14; ++i) { snprintf(t, sizeof(t), " %d", (int8_t)b[0xE + i]); g += t; }
    for (int p = 0; p < 2; ++p) for (int i = 0; i < 0x40; ++i) { snprintf(t, sizeof(t), " %02X", b[0x20 + p * 0x40 + i]); d[p] += t; }
    Log("%s", g.c_str()); Log("%s", d[0].c_str()); Log("%s", d[1].c_str());
}

bool Frozen() { return g_frozen; }
// Unfreeze until exactly one new simulation frame has run, then freeze again.
void StepOnce() { g_stepFrom = SimFrame(); g_stepPending = 1; g_frozen = false; }
void PollStep() {
    if (g_stepPending.load() > 0 && SimFrame() != g_stepFrom.load()) { g_stepPending = 0; g_frozen = true; }
}

std::vector<std::string> RecentEvents() {
    std::lock_guard<std::mutex> l(g_trackMtx);
    return std::vector<std::string>(g_events.begin(), g_events.end());
}

bool Ready() { return g_ready; }
std::string Status() { return g_status; }

// ----------------------------------------------------------------------------- init
static bool VerifyVtable(VtDef& v) {
    uintptr_t va = g_base + v.rva;
    uintptr_t col = mem::ReadOr<uintptr_t>(va - 8, 0);
    if (!col) return false;
    uint32_t tdRva = mem::ReadOr<uint32_t>(col + 12, 0);
    char name[64] = {};
    if (!mem::ReadRaw(g_base + tdRva + 0x10, name, sizeof(name) - 1)) return false;
    if (strcmp(name, v.rtti) != 0) return false;
    v.va = va;
    return true;
}

static bool HookAt(uintptr_t target, void* detour, void** orig, const char* name) {
    if (!target) { Log("hook %s: target missing", name); return false; }
    MH_STATUS st = MH_CreateHook((void*)target, detour, orig);
    if (st != MH_OK) { Log("hook %s: MH_CreateHook failed %d", name, (int)st); return false; }
    st = MH_EnableHook((void*)target);
    if (st != MH_OK) { Log("hook %s: MH_EnableHook failed %d", name, (int)st); return false; }
    Log("hook %s at rva %llX", name, (unsigned long long)(target - g_base));
    return true;
}

bool Init() {
    g_base = mem::ModuleBase();
    char path[MAX_PATH];
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&Init, &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    g_dir = path;
    g_dir = g_dir.substr(0, g_dir.find_last_of("\\/") + 1);

    int found = 0;
    std::string missing;
    for (auto& s : g_sigs) {
        uintptr_t m = mem::FindPattern(s.pat);
        if (m && s.rip) {
            int32_t d = mem::ReadOr<int32_t>(m + s.off, 0);
            m = m + s.off + 4 + d;
        } else if (m) {
            m += s.off;
        }
        s.addr = m;
        if (m) found++; else missing += std::string(" ") + s.name;
        Log("sig %-36s %s rva %llX", s.name, m ? "OK " : "MISSING", (unsigned long long)(m ? m - g_base : 0));
    }
    g_pGameInfo = g_sigs[S_FindGameInfo].addr;
    g_pNames = g_sigs[S_GetNames].addr;
    if (g_sigs[S_Dispatch].addr) g_dispatchSlot = mem::ReadOr<int32_t>(g_sigs[S_Dispatch].addr + 17, 0xD0);

    int vtOk = 0;
    for (auto& v : g_vts) {
        bool ok = VerifyVtable(v);
        vtOk += ok;
        Log("vtable %-28s %s", v.rtti, ok ? "OK" : "MISMATCH");
    }

    InitExact();
    if (MH_Initialize() != MH_OK) { Log("MH_Initialize failed (already initialised?)"); }
    bool core = g_sigs[S_MKProcDispatch].addr && g_pGameInfo && g_sigs[S_GetObj].addr && g_sigs[S_GetPos].addr;
    if (core) {
        HookAt(g_sigs[S_MKProcDispatch].addr, (void*)h_ProcDispatch, (void**)&o_ProcDispatch, "MKProcDispatch");
        HookAt(g_sigs[S_Dispatch].addr, (void*)h_Dispatch, (void**)&o_Dispatch, "Dispatch");
        HookAt(g_sigs[S_CamSetPos].addr, (void*)h_CamSetPos, (void**)&o_CamSetPos, "MKCamera::SetPosition");
        HookAt(g_sigs[S_StrikeTest].addr, (void*)h_StrikeTest, (void**)&o_StrikeTest, "StrikeVsHurtTest");
        if (g_planeO && g_planeA) {
            HookAt(g_sigs[S_CheckColChar].addr, (void*)h_CheckChar, (void**)&o_CheckChar, "CollisionObj::CheckCollisionAgainstCharacter");
            HookAt(g_sigs[S_Sphere2D].addr, (void*)h_Sphere2D, (void**)&o_Sphere2D, "CollideSphereVsPlayer2D");
            HookAt(g_sigs[S_Sphere3D].addr, (void*)h_Sphere3D, (void**)&o_Sphere3D, "CollideSphereVsPlayer3D");
            HookAt(g_sigs[S_Cyl2D].addr, (void*)h_Cyl2D, (void**)&o_Cyl2D, "collide_cylinder_vs_character_2D");
            HookAt(g_sigs[S_Cyl3D].addr, (void*)h_Cyl3D, (void**)&o_Cyl3D, "collide_cylinder_vs_character_3D");
            HookAt(g_sigs[S_Box2D].addr, (void*)h_Box2D, (void**)&o_Box2D, "collide_box_vs_character_2D");
            HookAt(g_sigs[S_Box3D].addr, (void*)h_Box3D, (void**)&o_Box3D, "collide_box_vs_character_3D");
        }
        const int ctorSig[6] = {S_CollCtor0, S_CollCtor1, S_MeleeCtor, S_ProjCtor, S_PCICtor, S_ResCtor};
        if (vtOk == (int)(sizeof(g_vts) / sizeof(g_vts[0]))) {
            for (int i = 0; i < 6; ++i)
                HookAt(g_sigs[ctorSig[i]].addr, (void*)g_ctorHooks[i], (void**)&o_Ctor[i], g_sigs[ctorSig[i]].name);
        } else {
            Log("vtable verification failed - collision object tracking disabled");
        }
    }
    if (core) { ibuf::Init(); dib::Init(); }
    char st[256];
    snprintf(st, sizeof(st), "MKX: %d/%d signatures, %d/5 vtables%s%s", found, (int)S_COUNT, vtOk,
             missing.empty() ? "" : ", missing:", missing.c_str());
    g_status = st;
    g_ready = core;
    Log("%s", st);
    return core;
}

} // namespace mkx
