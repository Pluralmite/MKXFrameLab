#include "boxes.h"
#include "config.h"
#include "game.h"
#include "imgui.h"
#include "mkx.h"
#include <cmath>
#include <cstdio>

static const float kPi = 3.14159265358979f;

static ImU32 ToImCol(unsigned rrggbbaa, float alphaMul) {
    unsigned r = (rrggbbaa >> 24) & 0xFF, g = (rrggbbaa >> 16) & 0xFF, b = (rrggbbaa >> 8) & 0xFF, a = rrggbbaa & 0xFF;
    a = (unsigned)(a * alphaMul);
    return IM_COL32(r, g, b, a);
}

static unsigned KindColor(BoxKind k) {
    switch (k) {
    case BoxKind::Hurt: return g_cfg.colHurt;
    case BoxKind::Hit: return g_cfg.colHit;
    case BoxKind::Throw: return g_cfg.colThrow;
    case BoxKind::Push: return g_cfg.colPush;
    default: return g_cfg.colOther;
    }
}

static float ProjectRadius(const Camera& cam, const float c[3], float r, const float sc[2]);
static void DrawSphere(ImDrawList* dl, const Camera& cam, const float c[3], float r, ImU32 line, ImU32 fill, bool doFill);
static ImU32 ToImCol(unsigned rrggbbaa, float alphaMul);
static void DrawCapsule(ImDrawList* dl, const Camera& cam, const float a[3], const float b[3], float r, ImU32 line, ImU32 fill, bool doFill);
static void SetupUE3(Camera& cam, const int rot[3], float fov) {
    cam.fovDeg = fov;
    float pitch = (float)(rot[0] & 0xFFFF) * (2.f * kPi / 65536.f);
    float yaw = (float)(rot[1] & 0xFFFF) * (2.f * kPi / 65536.f);
    float roll = (float)(rot[2] & 0xFFFF) * (2.f * kPi / 65536.f);
    float sp = sinf(pitch), cp = cosf(pitch), sy = sinf(yaw), cy = cosf(yaw), sr = sinf(roll), cr = cosf(roll);
    cam.fwd[0] = cp * cy;  cam.fwd[1] = cp * sy;  cam.fwd[2] = sp;
    cam.right[0] = sr * sp * cy - cr * sy;
    cam.right[1] = sr * sp * sy + cr * cy;
    cam.right[2] = -sr * cp;
    cam.up[0] = -(cr * sp * cy + sr * sy);
    cam.up[1] = cy * sr - cr * sp * sy;
    cam.up[2] = cr * cp;
    cam.useMatrix = false;
    cam.valid = true;
}

bool CameraFromGame(Camera& cam, const mkx::Snapshot& s, float w, float h) {
    cam.valid = false;
    cam.screenW = w; cam.screenH = h;
    if (!s.camValid || !(s.camFov > 1.f && s.camFov < 179.f)) return false;
    cam.pos[0] = s.camPos.x; cam.pos[1] = s.camPos.y; cam.pos[2] = s.camPos.z;
    int rot[3] = {s.camRot.pitch, s.camRot.yaw, s.camRot.roll};
    SetupUE3(cam, rot, s.camFov);
    return true;
}

void DrawBones(ImDrawList* dl, const Camera& cam, const mkx::Snapshot& s, float radius, bool labels) {
    if (!cam.valid) return;
    for (int p = 0; p < 2; ++p) {
        const auto& ps = s.p[p];
        if (!ps.valid) continue;
        ImU32 col = p == 0 ? IM_COL32(255, 220, 60, 220) : IM_COL32(60, 220, 255, 220);
        for (const auto& b : ps.bones) {
            float w[3] = {b.pos.x, b.pos.y, b.pos.z}, sc[2];
            if (!WorldToScreen(cam, w, sc)) continue;
            if (radius > 0) {
                float pr = ProjectRadius(cam, w, radius, sc);
                dl->AddCircle(ImVec2(sc[0], sc[1]), pr < 2 ? 2 : pr, col, 12, 1.0f);
            } else {
                dl->AddCircleFilled(ImVec2(sc[0], sc[1]), 2.5f, col, 8);
            }
            if (labels) dl->AddText(ImVec2(sc[0] + 3, sc[1] - 6), col, mkx::BoneName(b.nameIndex).c_str());
        }
        float w[3] = {ps.pos.x, ps.pos.y, ps.pos.z}, sc[2];
        if (WorldToScreen(cam, w, sc)) {
            dl->AddLine(ImVec2(sc[0] - 14, sc[1]), ImVec2(sc[0] + 14, sc[1]), col, 2.f);
            dl->AddLine(ImVec2(sc[0], sc[1] - 14), ImVec2(sc[0], sc[1] + 14), col, 2.f);
            dl->AddText(ImVec2(sc[0] + 6, sc[1] + 4), col, p == 0 ? "P1" : "P2");
        }
    }
}

void DrawHurt(ImDrawList* dl, const Camera& cam, const mkx::Snapshot& s, float zeroRadius, bool fill, bool labels) {
    if (!cam.valid) return;
    unsigned col = g_cfg.colHurt;
    for (int p = 0; p < 2; ++p) {
        for (const auto& h : s.p[p].hurt) {
            float w[3] = {h.pos.x, h.pos.y, h.pos.z};
            float r = h.radius > 0.f ? h.radius : zeroRadius;
            ImU32 line = ToImCol(col | 0xFF, h.radius > 0.f ? 1.f : 0.6f);
            ImU32 fl = ToImCol(col, 0.30f);
            if (r > 0.f) DrawSphere(dl, cam, w, r, line, fl, fill && h.radius > 0.f);
            if (labels) {
                float sc[2];
                if (WorldToScreen(cam, w, sc)) {
                    char b[32]; snprintf(b, sizeof(b), "%d", h.part);
                    dl->AddText(ImVec2(sc[0] + 3, sc[1] - 7), IM_COL32(255, 255, 255, 230), b);
                }
            }
        }
    }
}

void DrawStrike(ImDrawList* dl, const Camera& cam, const mkx::Snapshot& s, int lingerFrames, bool fill) {
    if (!cam.valid) return;
    // keep the last tested hitbox on screen for a few frames so 1-2 frame actives are visible
    static std::vector<mkx::HurtPoint> last[2];
    static uint64_t lastTick[2] = {0, 0};
    for (int p = 0; p < 2; ++p) {
        const auto& ps = s.p[p];
        bool live = !ps.strike.empty();
        if (live) { last[p] = ps.strike; lastTick[p] = s.frame; }
        if (last[p].empty()) continue;
        uint64_t age = s.frame >= lastTick[p] ? s.frame - lastTick[p] : 0;
        if (!live && (int)age > lingerFrames) continue;
        float am = live ? 1.f : 0.35f * (1.f - (float)age / (float)(lingerFrames + 1));
        unsigned col = g_cfg.colHit;
        for (const auto& h : last[p]) {
            if (h.radius <= 0.f) continue;
            float w[3] = {h.pos.x, h.pos.y, h.pos.z};
            DrawSphere(dl, cam, w, h.radius, ToImCol(col | 0xFF, am), ToImCol(col, 0.5f * am), fill);
        }
    }
}

static void DrawOBox(ImDrawList* dl, const Camera& cam, const float* m, float hx, float hy, float hz, ImU32 line, ImU32 fill, bool doFill) {
    float c[8][2];
    for (int i = 0; i < 8; ++i) {
        float sx = (i & 1) ? hx : -hx, sy = (i & 2) ? hy : -hy, sz = (i & 4) ? hz : -hz;
        float p[3];
        for (int k = 0; k < 3; ++k) p[k] = m[12 + k] + m[0 + k] * sx + m[4 + k] * sy + m[8 + k] * sz;
        if (!WorldToScreen(cam, p, c[i])) return;
    }
    static const int e[12][2] = {{0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7}};
    if (doFill) {
        static const int f[6][4] = {{0,1,3,2},{4,5,7,6},{0,1,5,4},{2,3,7,6},{0,2,6,4},{1,3,7,5}};
        for (auto& q : f)
            dl->AddQuadFilled(ImVec2(c[q[0]][0], c[q[0]][1]), ImVec2(c[q[1]][0], c[q[1]][1]), ImVec2(c[q[2]][0], c[q[2]][1]),
                              ImVec2(c[q[3]][0], c[q[3]][1]), fill);
    }
    for (auto& k : e) dl->AddLine(ImVec2(c[k[0]][0], c[k[0]][1]), ImVec2(c[k[1]][0], c[k[1]][1]), line, 1.5f);
}

void DrawShapes(ImDrawList* dl, const Camera& cam, const mkx::Snapshot& s, bool showInactive, bool fill) {
    if (!cam.valid) return;
    for (const auto& sh : s.shapes) {
        bool active = sh.state == 1;
        if (!active && !showInactive) continue;
        unsigned col = sh.kind == mkx::OK_Projectile ? 0xFF9020D0u : g_cfg.colHit;
        float am = active ? 1.f : 0.35f;
        ImU32 line = ToImCol(col | 0xFF, am), fl = ToImCol(col, 0.4f * am);
        float r = sh.radius > 0 ? sh.radius : 5.f;
        float p[3] = {sh.pos.x, sh.pos.y, sh.pos.z};
        if (!sh.poly.empty()) {
            // exact hit area from the game's own hit-test geometry
            ImVec2 pts[64];
            int n = 0;
            bool ok = true;
            for (auto& v : sh.poly) {
                float w[3] = {v.x, v.y, v.z}, sc[2];
                if (!WorldToScreen(cam, w, sc)) { ok = false; break; }
                pts[n++] = ImVec2(sc[0], sc[1]);
                if (n >= 64) break;
            }
            if (ok && n >= 3) {
                if (active) {
                    if (fill) dl->AddConvexPolyFilled(pts, n, fl);
                    dl->AddPolyline(pts, n, line, ImDrawFlags_Closed, 1.5f);
                } else {
                    // waiting to be tested: dashed outline only, so it never reads as a live hitbox
                    for (int i = 0; i < n; ++i) {
                        ImVec2 a = pts[i], b = pts[(i + 1) % n];
                        float dx = b.x - a.x, dy = b.y - a.y, len = sqrtf(dx * dx + dy * dy);
                        for (float t0 = 0; t0 < len; t0 += 12.f) {
                            float t1 = t0 + 6.f < len ? t0 + 6.f : len;
                            dl->AddLine(ImVec2(a.x + dx * t0 / len, a.y + dy * t0 / len), ImVec2(a.x + dx * t1 / len, a.y + dy * t1 / len), line, 1.2f);
                        }
                    }
                }
            }
            continue;
        }
        switch (sh.type) {
        case 1: DrawSphere(dl, cam, p, r, line, fl, fill); break;
        case 2: {
            // Measured against real hits (Enchantress stand A / back A): this shape is a flat rectangle in the
            // fight plane - 'length' along its X axis, the 0x3EC value is its full height along its Z axis,
            // centred on the object's position. (Drawing it as a capsule with radius 0x3EC was far too big.)
            const float* m = sh.mat;
            float hl = sh.length * 0.5f, hh = (sh.radius > 0 ? sh.radius : 5.f) * 0.5f;
            float c[3] = {m[12], m[13], m[14]};
            float q[4][3];
            const float sx[4] = {-1, 1, 1, -1}, sz[4] = {-1, -1, 1, 1};
            ImVec2 pts[4];
            bool ok = true;
            for (int i = 0; i < 4; ++i) {
                for (int k = 0; k < 3; ++k) q[i][k] = c[k] + m[k] * hl * sx[i] + m[8 + k] * hh * sz[i];
                float sc[2];
                if (!WorldToScreen(cam, q[i], sc)) { ok = false; break; }
                pts[i] = ImVec2(sc[0], sc[1]);
            }
            if (!ok) break;
            if (fill) dl->AddConvexPolyFilled(pts, 4, fl);
            dl->AddPolyline(pts, 4, line, ImDrawFlags_Closed, 1.5f);
            break;
        }
        case 3: DrawOBox(dl, cam, sh.mat, sh.length * 0.5f, sh.width * 0.5f, r, line, fl, fill); break;
        case 4:
            if (sh.hasMat) {
                float a[3] = {sh.mat[12], sh.mat[13], sh.mat[14]};
                DrawCapsule(dl, cam, a, p, r, line, fl, fill);
            } else {
                DrawSphere(dl, cam, p, r, line, fl, fill);
            }
            break;
        }
    }
}

bool CameraUpdate(Camera& cam, const mem::EvalCtx& ctx, float w, float h) {
    cam.valid = false;
    cam.screenW = w;
    cam.screenH = h;
    if (g_cfg.camMode == CamMode::Matrix && g_cfg.camMatrix) {
        uintptr_t a = g_cfg.camMatrix->Eval(ctx);
        if (!a || !mem::ReadRaw(a, cam.m, sizeof(cam.m))) return false;
        cam.useMatrix = true;
        cam.valid = true;
        return true;
    }
    if (g_cfg.camMode == CamMode::UE3 && g_cfg.camPos && g_cfg.camRot) {
        uintptr_t pa = g_cfg.camPos->Eval(ctx), ra = g_cfg.camRot->Eval(ctx);
        int rot[3];
        if (!pa || !ra || !mem::ReadRaw(pa, cam.pos, 12) || !mem::ReadRaw(ra, rot, 12)) return false;
        float fov = g_cfg.fovFixed > 0 ? g_cfg.fovFixed : 90.f;
        if (g_cfg.camFov) {
            uintptr_t fa = g_cfg.camFov->Eval(ctx);
            float f;
            if (fa && mem::Read(fa, f) && f > 1.f && f < 179.f) fov = f;
        }
        cam.fovDeg = fov;
        // UE3 rotator units: 65536 == 360 degrees. X forward, Y right, Z up (left-handed).
        float pitch = (float)(rot[0] & 0xFFFF) * (2.f * kPi / 65536.f);
        float yaw = (float)(rot[1] & 0xFFFF) * (2.f * kPi / 65536.f);
        float roll = (float)(rot[2] & 0xFFFF) * (2.f * kPi / 65536.f);
        float sp = sinf(pitch), cp = cosf(pitch), sy = sinf(yaw), cy = cosf(yaw), sr = sinf(roll), cr = cosf(roll);
        // Same as FRotationMatrix axes in UE3
        cam.fwd[0] = cp * cy;  cam.fwd[1] = cp * sy;  cam.fwd[2] = sp;
        cam.right[0] = sr * sp * cy - cr * sy;
        cam.right[1] = sr * sp * sy + cr * cy;
        cam.right[2] = -sr * cp;
        cam.up[0] = -(cr * sp * cy + sr * sy);
        cam.up[1] = cy * sr - cr * sp * sy;
        cam.up[2] = cr * cp;
        cam.useMatrix = false;
        cam.valid = true;
        return true;
    }
    return false;
}

bool WorldToScreen(const Camera& cam, const float p[3], float out[2]) {
    if (!cam.valid) return false;
    if (cam.useMatrix) {
        const float* m = cam.m;
        float x, y, w;
        if (g_cfg.matrixRowMajor) { // D3D style: v * M
            x = p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12];
            y = p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13];
            w = p[0] * m[3] + p[1] * m[7] + p[2] * m[11] + m[15];
        } else { // M * v
            x = p[0] * m[0] + p[1] * m[1] + p[2] * m[2] + m[3];
            y = p[0] * m[4] + p[1] * m[5] + p[2] * m[6] + m[7];
            w = p[0] * m[12] + p[1] * m[13] + p[2] * m[14] + m[15];
        }
        if (w < 0.001f) return false;
        out[0] = (x / w * 0.5f + 0.5f) * cam.screenW;
        out[1] = (1.f - (y / w * 0.5f + 0.5f)) * cam.screenH;
        return true;
    }
    float d[3] = {p[0] - cam.pos[0], p[1] - cam.pos[1], p[2] - cam.pos[2]};
    float z = d[0] * cam.fwd[0] + d[1] * cam.fwd[1] + d[2] * cam.fwd[2];
    if (z < 1.f) return false;
    float x = d[0] * cam.right[0] + d[1] * cam.right[1] + d[2] * cam.right[2];
    float y = d[0] * cam.up[0] + d[1] * cam.up[1] + d[2] * cam.up[2];
    float cx = cam.screenW * 0.5f, cy = cam.screenH * 0.5f;
    float focal = cx / tanf(cam.fovDeg * kPi / 360.f); // horizontal FOV, as UE3 uses
    out[0] = cx + x / z * focal;
    out[1] = cy - y / z * focal;
    return true;
}

// Projected size of a world-space radius at a point (approximation via camera right/up axis).
static float ProjectRadius(const Camera& cam, const float c[3], float r, const float sc[2]) {
    float q[3];
    if (cam.useMatrix) {
        // try each world axis and take the largest projected distance
        float best = 0;
        for (int ax = 0; ax < 3; ++ax) {
            q[0] = c[0]; q[1] = c[1]; q[2] = c[2];
            q[ax] += r;
            float s[2];
            if (WorldToScreen(cam, q, s)) {
                float dx = s[0] - sc[0], dy = s[1] - sc[1];
                best = fmaxf(best, sqrtf(dx * dx + dy * dy));
            }
        }
        return best;
    }
    for (int i = 0; i < 3; ++i) q[i] = c[i] + cam.up[i] * r;
    float s[2];
    if (!WorldToScreen(cam, q, s)) return 0;
    return fabsf(s[1] - sc[1]);
}

static bool ReadElemF(uintptr_t elem, int off, float& out) { return off >= 0 && mem::Read(elem + off, out); }

struct ShapeData {
    float a[3], b[3];
    float radius;
    float half[3];
    bool active;
};

static int ResolveCount(const BoxSource& b, const mem::EvalCtx& c) {
    int n = b.countFixed;
    if (b.count) {
        uintptr_t a = b.count->Eval(c);
        int v;
        if (a && mem::Read(a, v)) n = v;
        else return 0;
    }
    if (n < 0) n = 0;
    if (n > b.maxCount) n = b.maxCount;
    return n;
}

template <class F>
static void ForEachElement(const BoxSource& b, const mem::EvalCtx& base, F&& fn) {
    mem::EvalCtx c = base;
    c.self = b.owner == 2 ? base.p2 : base.p1;
    if (!c.self && (b.array->Text().find("self") != std::string::npos)) return;
    uintptr_t arr = b.array->Eval(c);
    if (!arr) return;
    int n = ResolveCount(b, c);
    for (int i = 0; i < n; ++i) {
        uintptr_t elem;
        if (b.indirect) {
            uint64_t p;
            if (!mem::Read(arr + (uintptr_t)i * 8, p) || !p) continue;
            elem = (uintptr_t)p;
        } else {
            elem = arr + (uintptr_t)i * b.stride;
        }
        ShapeData s{};
        if (!ReadElemF(elem, b.offX, s.a[0]) || !ReadElemF(elem, b.offY, s.a[1]) || !ReadElemF(elem, b.offZ, s.a[2])) continue;
        if (!std::isfinite(s.a[0]) || !std::isfinite(s.a[1]) || !std::isfinite(s.a[2])) continue;
        s.radius = b.radiusFixed;
        ReadElemF(elem, b.offRadius, s.radius);
        s.radius *= b.scale;
        s.active = true;
        if (b.offActive >= 0) {
            int v = 0;
            if (mem::Read(elem + b.offActive, v)) s.active = v != 0;
        }
        if (b.activeOnly && !s.active) continue;
        if (b.shape == BoxShape::Capsule) {
            if (!ReadElemF(elem, b.offX2, s.b[0]) || !ReadElemF(elem, b.offY2, s.b[1]) || !ReadElemF(elem, b.offZ2, s.b[2])) {
                s.b[0] = s.a[0]; s.b[1] = s.a[1]; s.b[2] = s.a[2];
            }
        }
        if (b.shape == BoxShape::Box) {
            for (int k = 0; k < 3; ++k) s.half[k] = s.radius;
            ReadElemF(elem, b.offHalfX, s.half[0]);
            ReadElemF(elem, b.offHalfY, s.half[1]);
            ReadElemF(elem, b.offHalfZ, s.half[2]);
            for (int k = 0; k < 3; ++k) s.half[k] *= b.scale;
        }
        fn(s);
    }
}

bool BoxesPlayerHasActiveHit(int owner, const mem::EvalCtx& ctx) {
    bool any = false;
    for (const auto& b : g_cfg.boxes) {
        if (!b.enabled || b.kind != BoxKind::Hit || b.owner != owner) continue;
        ForEachElement(b, ctx, [&](const ShapeData& s) { if (s.active) any = true; });
        if (any) return true;
    }
    return false;
}

static void DrawSphere(ImDrawList* dl, const Camera& cam, const float c[3], float r, ImU32 line, ImU32 fill, bool doFill) {
    float sc[2];
    if (!WorldToScreen(cam, c, sc)) return;
    float pr = ProjectRadius(cam, c, r, sc);
    if (pr < 1.f) pr = 1.f;
    if (doFill) dl->AddCircleFilled(ImVec2(sc[0], sc[1]), pr, fill, 24);
    dl->AddCircle(ImVec2(sc[0], sc[1]), pr, line, 24, 1.5f);
}

static void DrawCapsule(ImDrawList* dl, const Camera& cam, const float a[3], const float b[3], float r,
                        ImU32 line, ImU32 fill, bool doFill) {
    float sa[2], sb[2];
    if (!WorldToScreen(cam, a, sa) || !WorldToScreen(cam, b, sb)) return;
    float ra = fmaxf(1.f, ProjectRadius(cam, a, r, sa));
    float rb = fmaxf(1.f, ProjectRadius(cam, b, r, sb));
    float dx = sb[0] - sa[0], dy = sb[1] - sa[1];
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.5f) { DrawSphere(dl, cam, a, r, line, fill, doFill); return; }
    // One outline around the whole capsule (the real hit area): the hull of the two end circles.
    float ux = dx / len, uy = dy / len;
    float base = atan2f(uy, ux);
    const int seg = 16;
    ImVec2 pts[2 * (seg + 1)];
    int n = 0;
    for (int i = 0; i <= seg; ++i) {        // cap around b, facing away from a
        float t = base - 1.5707963f + 3.1415927f * i / seg;
        pts[n++] = ImVec2(sb[0] + cosf(t) * rb, sb[1] + sinf(t) * rb);
    }
    for (int i = 0; i <= seg; ++i) {        // cap around a, facing away from b
        float t = base + 1.5707963f + 3.1415927f * i / seg;
        pts[n++] = ImVec2(sa[0] + cosf(t) * ra, sa[1] + sinf(t) * ra);
    }
    if (doFill) dl->AddConvexPolyFilled(pts, n, fill);
    dl->AddPolyline(pts, n, line, ImDrawFlags_Closed, 1.5f);
}

static void DrawBox(ImDrawList* dl, const Camera& cam, const float c[3], const float h[3], ImU32 line, ImU32 fill, bool doFill) {
    float corners[8][2];
    for (int i = 0; i < 8; ++i) {
        float p[3] = {c[0] + ((i & 1) ? h[0] : -h[0]), c[1] + ((i & 2) ? h[1] : -h[1]), c[2] + ((i & 4) ? h[2] : -h[2])};
        if (!WorldToScreen(cam, p, corners[i])) return;
    }
    static const int edges[12][2] = {{0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7}};
    if (doFill) {
        // fill the screen-space bounding rect of the projection lightly
        float mnx = 1e9f, mny = 1e9f, mxx = -1e9f, mxy = -1e9f;
        for (auto& p : corners) { mnx = fminf(mnx, p[0]); mny = fminf(mny, p[1]); mxx = fmaxf(mxx, p[0]); mxy = fmaxf(mxy, p[1]); }
        dl->AddRectFilled(ImVec2(mnx, mny), ImVec2(mxx, mxy), fill);
    }
    for (auto& e : edges)
        dl->AddLine(ImVec2(corners[e[0]][0], corners[e[0]][1]), ImVec2(corners[e[1]][0], corners[e[1]][1]), line, 1.5f);
}

BoxDrawStats BoxesDraw(ImDrawList* dl, const Camera& cam, const mem::EvalCtx& ctx,
                       bool showHurt, bool showHit, bool showThrow, bool showPush, bool showOther, bool fill) {
    BoxDrawStats st;
    if (!cam.valid) return st;
    for (const auto& b : g_cfg.boxes) {
        if (!b.enabled) continue;
        bool show = (b.kind == BoxKind::Hurt && showHurt) || (b.kind == BoxKind::Hit && showHit) ||
                    (b.kind == BoxKind::Throw && showThrow) || (b.kind == BoxKind::Push && showPush) ||
                    (b.kind == BoxKind::Other && showOther);
        if (!show) continue;
        st.sources++;
        unsigned col = KindColor(b.kind);
        int before = st.drawn;
        ForEachElement(b, ctx, [&](const ShapeData& s) {
            float am = s.active ? 1.f : 0.25f; // inactive attack shapes drawn faint
            ImU32 line = ToImCol(col | 0xFF, am);
            ImU32 fl = ToImCol(col, 0.35f * am);
            switch (b.shape) {
            case BoxShape::Sphere: DrawSphere(dl, cam, s.a, s.radius, line, fl, fill); break;
            case BoxShape::Capsule: DrawCapsule(dl, cam, s.a, s.b, s.radius, line, fl, fill); break;
            case BoxShape::Box: DrawBox(dl, cam, s.a, s.half, line, fl, fill); break;
            }
            st.drawn++;
        });
        if (st.drawn == before) st.failed++;
    }
    return st;
}

void DrawPlayerMarkers(ImDrawList* dl, const Camera& cam) {
    for (int i = 0; i < 2; ++i) {
        const PlayerSample& p = g_game.p[i];
        if (!p.valid) continue;
        float s[2];
        if (!WorldToScreen(cam, p.pos, s)) continue;
        ImU32 c = i == 0 ? IM_COL32(255, 220, 60, 255) : IM_COL32(60, 220, 255, 255);
        dl->AddLine(ImVec2(s[0] - 12, s[1]), ImVec2(s[0] + 12, s[1]), c, 2.f);
        dl->AddLine(ImVec2(s[0], s[1] - 12), ImVec2(s[0], s[1] + 12), c, 2.f);
        dl->AddText(ImVec2(s[0] + 6, s[1] + 4), c, i == 0 ? "P1" : "P2");
    }
}
