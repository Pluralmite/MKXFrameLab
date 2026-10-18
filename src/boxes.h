// MKXFrameLab - camera projection and hit/hurt shape drawing
#pragma once
#include "memory.h"

struct ImDrawList;

struct Camera {
    bool valid = false;
    float pos[3] = {0, 0, 0};
    float fwd[3] = {1, 0, 0}, right[3] = {0, 1, 0}, up[3] = {0, 0, 1};
    float fovDeg = 90.f;
    float m[16] = {0};
    bool useMatrix = false;
    float screenW = 1920, screenH = 1080;
};

bool CameraUpdate(Camera& cam, const mem::EvalCtx& ctx, float screenW, float screenH);
namespace mkx { struct Snapshot; }
bool CameraFromGame(Camera& cam, const mkx::Snapshot& s, float screenW, float screenH);
void DrawHurt(ImDrawList* dl, const Camera& cam, const mkx::Snapshot& s, float zeroRadius, bool fill, bool labels);
void DrawStrike(ImDrawList* dl, const Camera& cam, const mkx::Snapshot& s, int lingerFrames, bool fill);
void DrawShapes(ImDrawList* dl, const Camera& cam, const mkx::Snapshot& s, bool showInactive, bool fill);
void DrawBones(ImDrawList* dl, const Camera& cam, const mkx::Snapshot& s, float radius, bool labels);
bool WorldToScreen(const Camera& cam, const float world[3], float out[2]);

struct BoxDrawStats { int drawn = 0; int sources = 0; int failed = 0; };

BoxDrawStats BoxesDraw(ImDrawList* dl, const Camera& cam, const mem::EvalCtx& ctx,
                       bool showHurt, bool showHit, bool showThrow, bool showPush, bool showOther, bool fill);
bool BoxesPlayerHasActiveHit(int owner, const mem::EvalCtx& ctx);

// Debug: draw a marker at each player's configured position.
void DrawPlayerMarkers(ImDrawList* dl, const Camera& cam);
