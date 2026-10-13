// What the map screen, the minimap and the markers above the heads show, and the projections that
// place it on the screen. Built on the game thread each frame (plugin/map.cpp), drawn by the
// overlay (plugin/overlay.cpp); the debug channel reads the same scene back (exp_map).
#pragma once
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "kc/colors.h"
#include "kc/protocol.h"

namespace kcp {

struct SceneChar {
    uint32_t netId = 0;
    uint8_t owner = 0;
    std::string name;
    kc::Vec3 pos;
    bool avatar = false, down = false, dead = false;   // avatar: the player's main character
    // Its name above its head, in its player's colour (every character of a player, standing in this
    // machine's world, alive). The game's own name tag shows it when it can (gameName: recoloured by
    // plugin/map.cpp, the main character's framed by the overlay); otherwise the overlay draws the
    // name itself at the head. Never both.
    bool head = false;
    bool gameName = false;
    kc::Handle local;           // its handle in this machine's world (head only)
};
struct SceneThreat {
    kc::Vec3 pos;
    int count = 0;
    int kind = 1;               // kc::ThreatKind
    std::string label;
};
struct ScenePing {
    uint32_t id = 0;
    uint8_t owner = 0;
    std::string who;
    int kind = 0;               // kc::PingKind
    kc::Vec3 pos;
    float age = 0;              // seconds
};
struct SceneRect {
    float x = 0, y = 0, w = 0, h = 0;
    uint8_t owner = 0;
};

struct MapScene {
    bool live = false;          // a world is loaded and running: anything may be drawn
    bool covered = false;       // the management screen or the character editor is up: no 3D markers, no minimap
    uint8_t me = 0;             // the local player
    std::vector<std::pair<uint8_t, std::string>> players;   // legend: id, name
    // Coordinates read from the game's GUI (the map image, its visible part, the portraits) are in
    // MyGUI's view pixels, guiW x guiH. The overlay draws in its display pixels (the back buffer):
    // SceneToDisplay converts them, every frame, with the sizes of that frame. They differ while the
    // window is being resized, or when the game renders at another size than its GUI's view.
    float guiW = 0, guiH = 0;   // 0: unknown (taken as the display's size)
    bool inDisplay = false;     // the GUI coordinates below are already display pixels
    double builtAt = 0;         // NowSeconds() of the read
    std::string mapWhy;         // why the map screen's markers are not drawn (empty: the map shows)
    // the game's map screen, when it shows: the map image's rectangle (it pans and zooms with it),
    // the visible part of it, and the world rectangle the image covers
    bool mapOpen = false;
    float imgX = 0, imgY = 0, imgW = 0, imgH = 0;
    float clipX0 = 0, clipY0 = 0, clipX1 = 0, clipY1 = 0;
    bool boundsOk = false;
    float minX = 0, minZ = 0, sizeX = 0, sizeZ = 0;
    // the 3D camera: Ogre projection * view (row-major), and where it looks (x, z)
    bool camOk = false;
    float viewProj[16] = {};   // world -> clip: Ogre's projection * view, Kenshi's moving render origin folded in
    bool originOk = false;
    kc::Vec3 origin;           // that origin (SceneManager::getRelativeOrigin), for the tests
    float camFwdX = 0, camFwdZ = -1;
    // the minimap's centre: the selected character, else the player's own
    bool centreOk = false;
    kc::Vec3 centre;
    std::vector<SceneChar> chars;
    std::vector<SceneThreat> threats;
    std::vector<ScenePing> pings;
    std::vector<SceneRect> portraits;   // squad bar frames of the players' own characters (visible part, not covered)
    // what the GUI rectangles were read from, so the overlay can read them again in its own frame
    // (plugin/map.cpp, MapRefreshGui): squad bar cells with their owner, and the map's widgets
    std::vector<std::pair<void*, uint8_t>> portraitCells;
    // the frames around the main characters' names, where the game's name tag shows them (MyGUI view
    // pixels, the name's text with a margin; not under a game window), and the characters they are
    // read from (again in the overlay's frame, like the squad bar)
    std::vector<SceneRect> nameFrames;
    std::vector<std::pair<kc::Handle, uint8_t>> nameFrameChars;
    // settings
    bool showMap = true, showHeads = true, showPortraits = true, showMinimap = true, minimapRotate = false, showPings = true;
    int minimapCorner = 1;              // 0 top left, 1 top right, 2 bottom left, 3 bottom right
    float minimapX = -1, minimapY = -1; // free position: the circle's centre in fractions of the display (< 0: the corner)
    float minimapSize = 1.0f;           // radius factor (0.5 to 2.5)
    float minimapZoom = 1500.0f;        // world units from the centre to the rim
};

// What the overlay drew in its last frame (display pixels), for the tests (mapconv).
struct MapDrawn {
    double at = -1e9;           // NowSeconds()
    float w = 0, h = 0;         // display size of that frame
    bool refreshed = false;     // GUI rectangles read again in that frame (same thread as the game's GUI)
    int heads = 0;              // above the heads: frames around the game's names + names drawn by the overlay
    int nameFrames = 0;         // of which frames around the game's own name tags
    bool minimap = false;
    float mmX = 0, mmY = 0, mmR = 0;    // where the minimap was drawn: centre and radius (display pixels)
    bool mmFree = false;                // at a free position (else in its corner)
    bool mmDrag = false;                // being moved or resized with the mouse
    bool mapOpen = false;
    int mapMarkers = 0;         // characters, hostile squads and pings drawn on the map screen
    float imgX = 0, imgY = 0, imgW = 0, imgH = 0;
    std::vector<SceneRect> frames;
};

// ---- projections (pure: the overlay and the tests use the same ones)

// MyGUI view pixels -> display pixels (the back buffer ImGui draws into: io.DisplaySize).
// MyGUI renders its view stretched over the whole render target, so the factor is per axis.
// An unknown or absurd view size gives 1:1 (MyGUI's view is normally the back buffer).
struct GuiToDisplay {
    float fx = 1, fy = 1;
    bool known = false;         // both sizes were sane
};
inline GuiToDisplay MakeGuiToDisplay(float guiW, float guiH, float dispW, float dispH) {
    GuiToDisplay g;
    const auto sane = [](float v) { return std::isfinite(v) && v >= 16.0f && v <= 65536.0f; };
    if (sane(guiW) && sane(guiH) && sane(dispW) && sane(dispH)) {
        g.fx = dispW / guiW;
        g.fy = dispH / guiH;
        g.known = true;
    }
    return g;
}
inline SceneRect GuiRectToDisplay(const SceneRect& r, const GuiToDisplay& g) {
    SceneRect o = r;
    o.x = r.x * g.fx; o.y = r.y * g.fy;
    o.w = r.w * g.fx; o.h = r.h * g.fy;
    return o;
}
// Converts the scene's GUI rectangles to display pixels of this frame (once).
inline void SceneToDisplay(MapScene& s, float dispW, float dispH) {
    if (s.inDisplay) return;
    const GuiToDisplay g = MakeGuiToDisplay(s.guiW, s.guiH, dispW, dispH);
    s.imgX *= g.fx; s.imgW *= g.fx; s.imgY *= g.fy; s.imgH *= g.fy;
    s.clipX0 *= g.fx; s.clipX1 *= g.fx; s.clipY0 *= g.fy; s.clipY1 *= g.fy;
    for (auto& r : s.portraits) r = GuiRectToDisplay(r, g);
    for (auto& r : s.nameFrames) r = GuiRectToDisplay(r, g);
    s.guiW = dispW; s.guiH = dispH;
    s.inDisplay = true;
}

// ---- names above the heads (pure: plugin/map.cpp, the overlay and the tests)

// The colour of a player's character's name (RGBA, 0..1, what MyGUI's TextBox::setTextColour takes):
// the player's colour; a selected character's (the game shows it white instead of grey) is lifted
// towards white, so the selection still shows.
inline void NameTagColour(uint8_t owner, bool selected, float out[4]) {
    const kc::PlayerColour c = kc::PlayerColor(owner);
    const float lift = selected ? 0.35f : 0.0f;
    const uint8_t rgb[3] = {c.r, c.g, c.b};
    for (int i = 0; i < 3; ++i) {
        const float v = float(rgb[i]) / 255.0f;
        out[i] = v + (1.0f - v) * lift;
    }
    out[3] = 1.0f;
}
// Packed 0xRRGGBBAA of such a colour (to compare what was applied with what is wanted).
inline uint32_t PackColour(const float c[4]) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        const float f = c[i] < 0 ? 0.0f : (c[i] > 1 ? 1.0f : c[i]);
        v = v << 8 | uint32_t(f * 255.0f + 0.5f);
    }
    return v;
}
// The frame around a name: its text (textW x textH, centred in the name's box as the game's
// layout does: TextAlign Center) with a margin. A text size that cannot be read (zero, absurd)
// frames the whole box.
constexpr float kNameFramePadX = 5.0f, kNameFramePadY = 2.0f;
inline SceneRect NameFrameRect(float boxX, float boxY, float boxW, float boxH, float textW, float textH, uint8_t owner) {
    SceneRect r;
    r.owner = owner;
    const bool textOk = textW >= 1.0f && textH >= 1.0f && textW <= 4096.0f && textH <= 512.0f;
    const float w = textOk ? textW : boxW, h = textOk ? textH : boxH;
    const float cx = boxX + boxW * 0.5f, cy = boxY + boxH * 0.5f;
    r.x = cx - w * 0.5f - kNameFramePadX;
    r.y = cy - h * 0.5f - kNameFramePadY;
    r.w = w + 2 * kNameFramePadX;
    r.h = h + 2 * kNameFramePadY;
    return r;
}

// The map screen: MapScreen::worldToMapCoords puts (x, z) at (x - minX) / sizeX * image width,
// (z - minZ) / sizeZ * image height inside the map image; the image sits at imgX, imgY.
inline bool WorldToMapScreen(const MapScene& s, const kc::Vec3& p, float& sx, float& sy) {
    if (!s.boundsOk || s.sizeX <= 0 || s.sizeZ <= 0) return false;
    sx = s.imgX + (p.x - s.minX) / s.sizeX * s.imgW;
    sy = s.imgY + (p.z - s.minZ) / s.sizeZ * s.imgH;
    return std::isfinite(sx) && std::isfinite(sy);
}
inline bool MapScreenToWorld(const MapScene& s, float sx, float sy, kc::Vec3& out) {
    if (!s.boundsOk || s.imgW <= 0 || s.imgH <= 0) return false;
    out.x = s.minX + (sx - s.imgX) / s.imgW * s.sizeX;
    out.z = s.minZ + (sy - s.imgY) / s.imgH * s.sizeZ;
    out.y = 0;
    return true;
}
inline bool InMapClip(const MapScene& s, float sx, float sy) {
    return sx >= s.clipX0 && sx <= s.clipX1 && sy >= s.clipY0 && sy <= s.clipY1;
}

// The 3D view: false when behind the camera.
inline bool WorldToScreen(const MapScene& s, const kc::Vec3& p, float w, float h, float& sx, float& sy) {
    if (!s.camOk) return false;
    const float* m = s.viewProj;
    const float cx = m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3];
    const float cy = m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7];
    const float cw = m[12] * p.x + m[13] * p.y + m[14] * p.z + m[15];
    if (!(cw > 1e-3f)) return false;
    sx = (cx / cw * 0.5f + 0.5f) * w;
    sy = (0.5f - cy / cw * 0.5f) * h;
    return std::isfinite(sx) && std::isfinite(sy);
}

// The ground under a screen point, taken as the horizontal plane at height `groundY` (Kenshi's
// ground is not flat: close enough for a ping near the characters).
inline bool ScreenToGround(const MapScene& s, float sx, float sy, float w, float h, float groundY, kc::Vec3& out);

// The minimap: world offset from the centre -> screen offset from the circle's centre (pixels),
// north up (screen up = -z, as on the map screen) or turned so the camera looks up.
struct MinimapFrame {
    float cx = 0, cy = 0, radius = 0;   // screen
    float scale = 0;                    // pixels per world unit
    float cosA = 1, sinA = 0;           // rotation applied to north-up offsets
};
inline MinimapFrame MakeMinimapFrame(const MapScene& s, float cx, float cy, float radius) {
    MinimapFrame f;
    f.cx = cx; f.cy = cy; f.radius = radius;
    f.scale = radius / (s.minimapZoom > 1 ? s.minimapZoom : 1);
    if (s.minimapRotate) {
        const float len = std::sqrt(s.camFwdX * s.camFwdX + s.camFwdZ * s.camFwdZ);
        if (len > 1e-4f) {
            // the camera's facing, (fx, fz) in north-up screen axes, must point straight up
            const float a = std::atan2(s.camFwdZ, s.camFwdX);
            const float t = -1.5707963f - a;
            f.cosA = std::cos(t); f.sinA = std::sin(t);
        }
    }
    return f;
}
inline void MinimapProject(const MinimapFrame& f, const kc::Vec3& centre, const kc::Vec3& p, float& sx, float& sy) {
    const float dx = (p.x - centre.x) * f.scale, dy = (p.z - centre.z) * f.scale;
    sx = f.cx + dx * f.cosA - dy * f.sinA;
    sy = f.cy + dx * f.sinA + dy * f.cosA;
}
inline kc::Vec3 MinimapUnproject(const MinimapFrame& f, const kc::Vec3& centre, float sx, float sy) {
    const float rx = sx - f.cx, ry = sy - f.cy;
    const float dx = rx * f.cosA + ry * f.sinA, dy = -rx * f.sinA + ry * f.cosA;
    return {centre.x + dx / f.scale, centre.y, centre.z + dy / f.scale};
}

// Where the minimap goes on a w x h display: in its corner (fx or fy < 0), else its centre at
// (fx * w, fy * h), always kept on screen with its north marker, scale and +/- buttons. `size`
// scales the default radius (11 % of the height, 70 to 160 px). Fractions survive resizes.
struct MinimapPlace {
    float cx = 0, cy = 0, r = 0;
    bool free = false;
};
constexpr float kMinimapMinSize = 0.5f, kMinimapMaxSize = 2.5f;
constexpr float kMinimapPadSide = 10.0f, kMinimapPadBottom = 26.0f;   // north marker, scale and buttons
inline float MinimapBaseRadius(float h) {   // size 1
    const float base = h * 0.11f;
    return base < 70.0f ? 70.0f : (base > 160.0f ? 160.0f : base);
}
inline float MinimapRadius(float size, float w, float h) {
    const float base = MinimapBaseRadius(h);
    size = size < kMinimapMinSize ? kMinimapMinSize : (size > kMinimapMaxSize ? kMinimapMaxSize : size);
    float r = base * size;
    // never more than fits: the circle and its pads inside the display
    const float maxR = std::fmin((w - 2 * kMinimapPadSide) * 0.5f, (h - kMinimapPadSide - kMinimapPadBottom) * 0.5f);
    if (r > maxR) r = maxR;
    return r < 20.0f ? 20.0f : r;
}
inline MinimapPlace PlaceMinimap(int corner, float fx, float fy, float size, float w, float h) {
    MinimapPlace p;
    p.r = MinimapRadius(size, w, h);
    const float R = p.r, margin = 18.0f;
    if (fx < 0 || fy < 0) {
        corner = corner < 0 ? 0 : (corner > 3 ? 3 : corner);
        p.cx = (corner % 2 == 0) ? margin + R : w - margin - R;
        // top right: below KenshiCoop's status panel
        p.cy = corner >= 2 ? h - margin - R - 30.0f : margin + R + (corner == 1 ? 150.0f : 0.0f);
    } else {
        p.free = true;
        p.cx = fx * w;
        p.cy = fy * h;
    }
    // on screen, whatever the display size
    const float x0 = R + kMinimapPadSide, x1 = w - R - kMinimapPadSide, y0 = R + kMinimapPadSide, y1 = h - R - kMinimapPadBottom;
    p.cx = x1 < x0 ? w * 0.5f : (p.cx < x0 ? x0 : (p.cx > x1 ? x1 : p.cx));
    p.cy = y1 < y0 ? h * 0.5f : (p.cy < y0 ? y0 : (p.cy > y1 ? y1 : p.cy));
    return p;
}

inline bool ScreenToGround(const MapScene& s, float sx, float sy, float w, float h, float groundY, kc::Vec3& out) {
    if (!s.camOk || w <= 0 || h <= 0) return false;
    // Solve viewProj * (x, groundY, z, 1) ~ (ndcX * cw, ndcY * cw, ., cw) for x, z: two linear equations.
    const float nx = sx / w * 2.0f - 1.0f, ny = 1.0f - sy / h * 2.0f;
    const float* m = s.viewProj;
    // row_i . p - nd_i * row_3 . p = 0  ->  a_i x + b_i z = c_i
    const float a0 = m[0] - nx * m[12], b0 = m[2] - nx * m[14], c0 = -((m[1] - nx * m[13]) * groundY + (m[3] - nx * m[15]));
    const float a1 = m[4] - ny * m[12], b1 = m[6] - ny * m[14], c1 = -((m[5] - ny * m[13]) * groundY + (m[7] - ny * m[15]));
    const float det = a0 * b1 - a1 * b0;
    if (std::fabs(det) < 1e-9f) return false;
    out.x = (c0 * b1 - c1 * b0) / det;
    out.z = (a0 * c1 - a1 * c0) / det;
    out.y = groundY;
    // it must be in front of the camera
    const float cw = m[12] * out.x + m[13] * out.y + m[14] * out.z + m[15];
    return cw > 1e-3f && std::isfinite(out.x) && std::isfinite(out.z);
}

const char* PingKindName(int kind);     // French, for the players
const char* ThreatKindName(int kind);   // French

} // namespace kcp
