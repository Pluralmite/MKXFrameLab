// MKXFrameLab - configuration loaded from MKXFrameLab.ini (next to the .asi)
#pragma once
#include "memory.h"
#include <string>
#include <vector>

enum class BoxKind { Hurt, Hit, Throw, Push, Other };
enum class BoxShape { Sphere, Capsule, Box };
enum class CamMode { None, UE3, Matrix };

struct BoxSource {
    std::string name;
    bool enabled = true;
    int owner = 1;               // 1 or 2 -> "self" in expressions
    BoxKind kind = BoxKind::Hurt;
    BoxShape shape = BoxShape::Sphere;
    mem::ExprPtr array;          // address of first element (or of pointer array)
    mem::ExprPtr count;          // int32 count (optional)
    int countFixed = 1;
    int maxCount = 64;
    uint32_t stride = 0x40;
    bool indirect = false;       // array holds pointers to elements
    // element field offsets (-1 = not present)
    int offX = 0, offY = 4, offZ = 8;
    int offX2 = -1, offY2 = -1, offZ2 = -1;   // capsule second point
    int offRadius = -1;
    int offHalfX = -1, offHalfY = -1, offHalfZ = -1; // box half extents
    int offActive = -1;          // int32, nonzero = active
    bool activeOnly = false;     // skip elements whose active flag is 0
    float radiusFixed = 10.f;
    float scale = 1.f;
};

struct Config {
    std::string baseDir;   // folder of the mod files (set at start-up)
    std::string path;
    std::string dir;
    std::vector<std::string> errors;

    // General
    int menuKey = VK_F2;
    int boxesKey = VK_F3;
    int meterKey = VK_F4;
    int pauseKey = VK_F5;
    int stepKey = VK_F6;
    bool demo = false;
    bool startVisible = true;
    bool verboseLog = true;
    float menuScale = 1.0f;

    mem::ExprPtr frameCounter;   // int32 global gameplay frame counter (optional)
    mem::ExprPtr gameSpeed;      // float global game speed (optional, for pause/step)
    mem::ExprPtr modeCheck;      // int32 game-mode value (optional practice lock)
    int modeValue = 0;

    // Players
    mem::ExprPtr p1, p2;

    // Per-player fields (evaluated with self = player object)
    mem::ExprPtr posX, posY, posZ;   // float
    mem::ExprPtr state;              // int32 move / state id
    mem::ExprPtr moveFrame;          // int32 frame within current move
    mem::ExprPtr actionable;         // int32 nonzero = can act
    mem::ExprPtr attackActive;       // int32 nonzero = strike active
    mem::ExprPtr hitstun;            // int32 >0
    mem::ExprPtr blockstun;          // int32 >0
    std::vector<int> idleStates;     // state ids that count as actionable

    // Camera
    CamMode camMode = CamMode::None;
    mem::ExprPtr camPos;    // 3 floats
    mem::ExprPtr camRot;    // 3 int32 (UE3 rotator: pitch, yaw, roll; 65536 = 360 deg)
    mem::ExprPtr camFov;    // float degrees (horizontal)
    float fovFixed = 0.f;   // used when CamFov is empty
    mem::ExprPtr camMatrix; // 16 floats view-projection
    bool matrixRowMajor = true;

    std::vector<BoxSource> boxes;

    // Colors (ABGR as ImGui packs them, configured as RRGGBBAA hex)
    unsigned colHurt = 0x3C8CFFB4, colHit = 0xFF3030D0, colThrow = 0xFFD020C0,
             colPush = 0xB0B0B080, colOther = 0x30FF60B0;

    bool Load(const std::string& iniPath);
};

extern Config g_cfg;
