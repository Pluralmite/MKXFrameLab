#include "config.h"
#include "log.h"
#include <cstdlib>
#include <sstream>
#include <algorithm>

Config g_cfg;

static std::string IniStr(const std::string& path, const char* sec, const char* key, const char* def = "") {
    char buf[2048];
    GetPrivateProfileStringA(sec, key, def, buf, sizeof(buf), path.c_str());
    std::string s = buf;
    // strip trailing ';' comments that are separated by whitespace
    size_t c = s.find(" ;");
    if (c != std::string::npos) s = s.substr(0, c);
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
    return s;
}

static long IniNum(const std::string& path, const char* sec, const char* key, long def) {
    std::string s = IniStr(path, sec, key);
    if (s.empty()) return def;
    return strtol(s.c_str(), nullptr, 0); // 0x.. hex or decimal
}

static float IniFloat(const std::string& path, const char* sec, const char* key, float def) {
    std::string s = IniStr(path, sec, key);
    if (s.empty()) return def;
    return (float)atof(s.c_str());
}

static bool IniBool(const std::string& path, const char* sec, const char* key, bool def) {
    std::string s = IniStr(path, sec, key);
    if (s.empty()) return def;
    return s == "1" || _stricmp(s.c_str(), "true") == 0 || _stricmp(s.c_str(), "yes") == 0;
}

// Field offsets inside box elements are plain hex numbers (no "0x" needed).
static int IniOff(const std::string& path, const char* sec, const char* key, int def) {
    std::string s = IniStr(path, sec, key);
    if (s.empty()) return def;
    if (s == "-1" || _stricmp(s.c_str(), "none") == 0) return -1;
    return (int)strtol(s.c_str(), nullptr, 16);
}

static unsigned IniColor(const std::string& path, const char* key, unsigned def) {
    std::string s = IniStr(path, "Colors", key);
    if (s.empty()) return def;
    return (unsigned)strtoul(s.c_str(), nullptr, 16);
}

static mem::ExprPtr IniExpr(Config& c, const char* sec, const char* key) {
    std::string s = IniStr(c.path, sec, key);
    if (s.empty()) return nullptr;
    std::string err;
    auto e = mem::Expr::Parse(s, &err);
    if (!e) {
        std::string msg = std::string("[") + sec + "] " + key + ": " + err + "  (\"" + s + "\")";
        c.errors.push_back(msg);
        Log("Config error: %s", msg.c_str());
    }
    return e;
}

bool Config::Load(const std::string& iniPath) {
    *this = Config();
    path = iniPath;
    size_t slash = path.find_last_of("\\/");
    dir = slash == std::string::npos ? "" : path.substr(0, slash + 1);

    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        errors.push_back("MKXFrameLab.ini not found next to the .asi - using defaults (demo only).");
        demo = true;
        return false;
    }

    menuKey = (int)IniNum(path, "General", "MenuKey", VK_F2);
    boxesKey = (int)IniNum(path, "General", "BoxesKey", VK_F3);
    meterKey = (int)IniNum(path, "General", "MeterKey", VK_F4);
    pauseKey = (int)IniNum(path, "General", "PauseKey", VK_F5);
    stepKey = (int)IniNum(path, "General", "StepKey", VK_F6);
    demo = IniBool(path, "General", "Demo", false);
    startVisible = IniBool(path, "General", "StartVisible", true);
    verboseLog = IniBool(path, "General", "VerboseLog", true);
    g_logVerbose = verboseLog;
    menuScale = IniFloat(path, "General", "MenuScale", 1.0f);
    frameCounter = IniExpr(*this, "General", "FrameCounter");
    gameSpeed = IniExpr(*this, "General", "GameSpeed");
    modeCheck = IniExpr(*this, "General", "ModeCheck");
    modeValue = (int)IniNum(path, "General", "ModeValue", 0);

    p1 = IniExpr(*this, "Players", "P1");
    p2 = IniExpr(*this, "Players", "P2");

    posX = IniExpr(*this, "Fields", "PosX");
    posY = IniExpr(*this, "Fields", "PosY");
    posZ = IniExpr(*this, "Fields", "PosZ");
    state = IniExpr(*this, "Fields", "State");
    moveFrame = IniExpr(*this, "Fields", "MoveFrame");
    actionable = IniExpr(*this, "Fields", "Actionable");
    attackActive = IniExpr(*this, "Fields", "AttackActive");
    hitstun = IniExpr(*this, "Fields", "Hitstun");
    blockstun = IniExpr(*this, "Fields", "Blockstun");
    {
        std::stringstream ss(IniStr(path, "Fields", "IdleStates"));
        std::string tok;
        while (std::getline(ss, tok, ','))
            if (!tok.empty()) idleStates.push_back((int)strtol(tok.c_str(), nullptr, 0));
    }

    std::string mode = IniStr(path, "Camera", "Mode", "none");
    if (_stricmp(mode.c_str(), "ue3") == 0) camMode = CamMode::UE3;
    else if (_stricmp(mode.c_str(), "matrix") == 0) camMode = CamMode::Matrix;
    camPos = IniExpr(*this, "Camera", "Pos");
    camRot = IniExpr(*this, "Camera", "Rot");
    camFov = IniExpr(*this, "Camera", "Fov");
    fovFixed = IniFloat(path, "Camera", "FovFixed", 0.f);
    camMatrix = IniExpr(*this, "Camera", "Matrix");
    matrixRowMajor = _stricmp(IniStr(path, "Camera", "MatrixLayout", "row").c_str(), "col") != 0;

    for (int i = 0; i < 32; ++i) {
        std::string sec = "Box" + std::to_string(i);
        const char* s = sec.c_str();
        if (IniStr(path, s, "Array").empty()) continue;
        BoxSource b;
        b.name = IniStr(path, s, "Name", s);
        b.enabled = IniBool(path, s, "Enabled", true);
        b.owner = (int)IniNum(path, s, "Owner", 1) == 2 ? 2 : 1;
        std::string kind = IniStr(path, s, "Kind", "hurt");
        if (_stricmp(kind.c_str(), "hit") == 0) b.kind = BoxKind::Hit;
        else if (_stricmp(kind.c_str(), "throw") == 0) b.kind = BoxKind::Throw;
        else if (_stricmp(kind.c_str(), "push") == 0) b.kind = BoxKind::Push;
        else if (_stricmp(kind.c_str(), "other") == 0) b.kind = BoxKind::Other;
        std::string shape = IniStr(path, s, "Shape", "sphere");
        if (_stricmp(shape.c_str(), "capsule") == 0) b.shape = BoxShape::Capsule;
        else if (_stricmp(shape.c_str(), "box") == 0) b.shape = BoxShape::Box;
        b.array = IniExpr(*this, s, "Array");
        b.count = IniExpr(*this, s, "Count");
        b.countFixed = (int)IniNum(path, s, "CountFixed", 1);
        b.maxCount = std::clamp((int)IniNum(path, s, "MaxCount", 64), 1, 512);
        b.stride = (uint32_t)IniOff(path, s, "Stride", 0x40);
        b.indirect = IniBool(path, s, "Indirect", false);
        b.offX = IniOff(path, s, "X", 0);
        b.offY = IniOff(path, s, "Y", 4);
        b.offZ = IniOff(path, s, "Z", 8);
        b.offX2 = IniOff(path, s, "X2", -1);
        b.offY2 = IniOff(path, s, "Y2", -1);
        b.offZ2 = IniOff(path, s, "Z2", -1);
        b.offRadius = IniOff(path, s, "Radius", -1);
        b.offHalfX = IniOff(path, s, "HalfX", -1);
        b.offHalfY = IniOff(path, s, "HalfY", -1);
        b.offHalfZ = IniOff(path, s, "HalfZ", -1);
        b.offActive = IniOff(path, s, "ActiveField", -1);
        b.activeOnly = IniBool(path, s, "ActiveOnly", false);
        b.radiusFixed = IniFloat(path, s, "RadiusFixed", 10.f);
        b.scale = IniFloat(path, s, "Scale", 1.f);
        if (b.array) boxes.push_back(b);
    }

    colHurt = IniColor(path, "Hurt", colHurt);
    colHit = IniColor(path, "Hit", colHit);
    colThrow = IniColor(path, "Throw", colThrow);
    colPush = IniColor(path, "Push", colPush);
    colOther = IniColor(path, "Other", colOther);

    Log("Config loaded: %s (%d box sources, %d errors)", path.c_str(), (int)boxes.size(), (int)errors.size());
    return true;
}
