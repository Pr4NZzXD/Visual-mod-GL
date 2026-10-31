#include "menu.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "log.h"
#include "shared.h"

// The menu, ported from Ui.cs of the C# mod: a floating eye icon that opens a panel with a tab list on the left
// and the page of the chosen tab on the right. Everything is drawn by hand (rounded cards, switches, coloured
// sliders) on ImGui's foreground draw list; the finger is handled here too, with the hit areas the pages register
// while they are drawn (the next frame matches the finger against those).
// Runs on the render thread, right before the game presents a frame.

#ifdef HAVE_UI_FONT
extern const unsigned char kUiFont[];  // made by CMake from ImGui's misc/fonts
extern const unsigned int kUiFontSize;
#endif

namespace vm {
namespace {

using Col = ImVec4;
using Act = std::function<void()>;
using SetB = std::function<void(bool)>;
using SetF = std::function<void(float)>;
using Pick = std::function<void(int)>;

// ================================================================ state
bool gInit = false, gFailed = false;
UiSettings ui;
FxSettings fx;
MainStatus st;
std::chrono::steady_clock::time_point gLast;
float gTime = 0.f;       // seconds since the menu started
float gFps = 0.f;        // smoothed frames per second
bool gNeedSave = false;
float gSaveAt = -1.f;
int gPresetSel = -1;     // the preset that was picked last (cleared when a value is changed by hand)
float gScaleDraft = 1.f;
std::string gToast;
float gToastUntil = 0.f;
int gToastSeq = -1;
constexpr int kGraphN = 90;
float gFrameMs[kGraphN] = {};  // the last frame times, oldest first
int gFramePos = 0;

ImDrawList* D = nullptr;
ImFont* gFont = nullptr;

// layout, computed every frame from the screen size
float W = 0, H = 0;
float S = 1.f, ES = 44.f;
float fpsW, fpsH, fpsBoxW, fpsBoxH, graphW, graphH, pw, ph, hdrH, ftH, sbW;
float splitBoxW = 0.f, splitBoxH = 0.f, splitFont = 12.f, splitLine = 16.f;
SplitsView gSv;
float kx, ky, kw, kBot, cTop, cBot, cx, cw, cy;
float scroll = 0.f, contentH = 0.f;

const Col White(1.f, 1.f, 1.f, 1.f);
const Col CBlue(0.30f, 0.55f, 1.00f, 1.f), COrange(1.00f, 0.62f, 0.20f, 1.f), CGreen(0.30f, 0.90f, 0.40f, 1.f);
const Col CMagenta(0.90f, 0.30f, 0.80f, 1.f), CGrey(0.45f, 0.45f, 0.48f, 1.f), CRed(1.00f, 0.25f, 0.25f, 1.f);
const Col CDark(0.10f, 0.10f, 0.12f, 1.f);

using Palette = std::vector<std::vector<float>>;
const Palette kAccents = {{0.12f, 0.58f, 1.00f}, {0.25f, 0.80f, 0.85f}, {0.30f, 0.80f, 0.45f}, {0.65f, 0.85f, 0.30f},
                          {0.95f, 0.60f, 0.20f}, {0.90f, 0.32f, 0.32f}, {0.98f, 0.15f, 0.62f}, {0.65f, 0.45f, 0.95f}};
const Palette kBackgrounds = {{0.055f, 0.060f, 0.072f}, {0.06f, 0.09f, 0.17f}, {0.05f, 0.12f, 0.09f},
                              {0.15f, 0.06f, 0.08f},    {0.11f, 0.07f, 0.16f}, {0.15f, 0.15f, 0.16f}};
const Palette kFilterPalette = {{1.00f, 1.00f, 1.00f}, {1.00f, 0.35f, 0.35f}, {1.00f, 0.65f, 0.25f},
                                {1.00f, 0.95f, 0.45f}, {0.45f, 1.00f, 0.50f}, {0.40f, 0.90f, 1.00f},
                                {0.40f, 0.50f, 1.00f}, {0.75f, 0.45f, 1.00f}, {1.00f, 0.50f, 0.80f}};
const Palette kFogPalette = {{0.60f, 0.60f, 0.65f}, {1.00f, 1.00f, 1.00f}, {0.00f, 0.00f, 0.00f}, {0.80f, 0.10f, 0.10f},
                             {0.95f, 0.50f, 0.10f}, {0.10f, 0.60f, 0.20f}, {0.15f, 0.30f, 0.90f}, {0.50f, 0.15f, 0.70f}};

const std::vector<std::string> kTabNames = {"General", "Color", "Effects", "Advanced", "Performance", "Settings", "Cheat", "TAS"};
const char* kPageTitles[] = {"GENERAL", "COLOR GRADING", "VISUAL EFFECTS", "ADVANCED", "PERFORMANCE", "MENU SETTINGS", "CHEAT", "TAS TOOLS"};
const std::vector<std::string> kCheatSub = {"Player", "Enemies", "Items", "ESP"};
const std::vector<std::string> kTasSub = {"Time", "Splits", "Utility", "Crosshair"};
const std::vector<std::string> kSpeedNames1 = {"0.01x", "0.05x", "0.1x", "0.25x", "0.5x"};
const std::vector<std::string> kSpeedNames2 = {"0.75x", "1x", "1.5x", "2x", "3x", "5x"};
const float kSpeedVals1[5] = {0.01f, 0.05f, 0.1f, 0.25f, 0.5f};
const float kSpeedVals2[6] = {0.75f, 1.f, 1.5f, 2.f, 3.f, 5.f};
const std::vector<std::string> kCrossNames = {"Dot", "Plus", "Gap", "Circle", "Ring", "X"};
const std::vector<std::string> kColorSub = {"Basic", "Temperature", "Tint", "RGB"};
const std::vector<std::string> kPresetNames = {"Default", "Warm", "Cold", "Vivid", "Cinematic", "Noir",
                                               "Night", "VHS", "Dreamy", "Horror", "Golden", "Sepia"};
const std::vector<std::string> kAaNames = {"Game", "Off", "2x", "4x", "8x"};
const std::vector<std::string> kAnisoNames = {"Game", "Off", "On"};
const std::vector<std::string> kAoQuality = {"Low", "Medium", "High"};
const std::vector<std::string> kDofQuality = {"Small", "Medium", "Large"};
const std::vector<std::string> kFogModes = {"Game default", "No fog", "Custom"};
const std::vector<std::string> kLevelNames = {"Off", "Low", "Ultra low"};
const std::vector<std::string> kResNames = {"100%", "75%", "60%", "50%", "35%"};
const std::vector<std::string> kFpsNames = {"Game default", "30", "45", "60", "90"};
const std::vector<std::string> kTargetNames = {"30 FPS", "45 FPS", "60 FPS"};

// ================================================================ colours
float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
float lerpf(float a, float b, float t) { return a + (b - a) * t; }

Col shade(Col c, float d) { return Col(clampf(c.x + d, 0, 1), clampf(c.y + d, 0, 1), clampf(c.z + d, 0, 1), c.w); }
Col withA(Col c, float a) { return Col(c.x, c.y, c.z, a); }
Col fromP(const std::vector<float>& p) { return Col(p[0], p[1], p[2], 1.f); }
ImU32 U(const Col& c) { return ImGui::ColorConvertFloat4ToU32(c); }

Col hueRgb(float h) {
    float r = clampf(std::fabs(h * 6.f - 3.f) - 1.f, 0, 1);
    float g = clampf(2.f - std::fabs(h * 6.f - 2.f), 0, 1);
    float b = clampf(2.f - std::fabs(h * 6.f - 4.f), 0, 1);
    return Col(r, g, b, 1.f);
}

Col bgCol() { return Col(ui.bg[0], ui.bg[1], ui.bg[2], 1.f); }
Col accentNow() {
    if (!ui.rainbow) return Col(ui.accent[0], ui.accent[1], ui.accent[2], 1.f);
    float x = gTime * ui.rainbowSpeed * 0.25f;
    return hueRgb(x - std::floor(x));
}
// the cards follow the menu opacity too, so at 0 the menu is only its text and controls over the game
Col cardCol() { return withA(shade(bgCol(), 0.08f), ui.panelAlpha); }

int indexIn(const Palette& p, float r, float g, float b) {
    for (size_t i = 0; i < p.size(); i++)
        if (std::fabs(p[i][0] - r) < 0.006f && std::fabs(p[i][1] - g) < 0.006f && std::fabs(p[i][2] - b) < 0.006f) return (int)i;
    return -1;
}

// ================================================================ drawing primitives
void fill(float x, float y, float w, float h, Col c) {
    if (c.w <= 0.003f || w <= 0.f || h <= 0.f) return;
    D->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), U(c));
}

void rfill(float x, float y, float w, float h, float r, Col c) {
    if (w < 0.5f || h < 0.5f || c.w <= 0.003f) return;
    if (!ui.rounded || r < 0.8f) {
        fill(x, y, w, h, c);
        return;
    }
    r = std::min(r, std::min(w, h) * 0.5f);
    D->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), U(c), r);
}

// the ring is drawn inside the rectangle, like the C# version
void rring(float x, float y, float w, float h, float r, float t, Col c) {
    if (w < 0.5f || h < 0.5f || c.w <= 0.003f) return;
    if (!ui.rounded || r < 0.8f) {
        fill(x, y, w, t, c);
        fill(x, y + h - t, w, t, c);
        fill(x, y + t, t, h - 2 * t, c);
        fill(x + w - t, y + t, t, h - 2 * t, c);
        return;
    }
    r = std::min(r, std::min(w, h) * 0.5f);
    float hf = t * 0.5f;
    D->AddRect(ImVec2(x + hf, y + hf), ImVec2(x + w - hf, y + h - hf), U(c), std::max(0.f, r - hf), 0, t);
}

void disc(float x, float y, float rad, Col c) {
    if (c.w <= 0.003f) return;
    if (!ui.rounded && rad > 3.f * S) {  // without rounded corners a small square stands in, like the C# version
        fill(x - rad, y - rad, 2 * rad, 2 * rad, c);
        return;
    }
    D->AddCircleFilled(ImVec2(x, y), rad, U(c), 32);
}

void ring(float x, float y, float rad, float t, Col c) {
    if (c.w <= 0.003f) return;
    if (!ui.rounded) {
        rring(x - rad, y - rad, 2 * rad, 2 * rad, 0.f, t, c);
        return;
    }
    D->AddCircle(ImVec2(x, y), rad - t * 0.5f, U(c), 40, t);
}

void hgrad(float x, float y, float w, float h, Col a, Col b) {
    D->AddRectFilledMultiColor(ImVec2(x, y), ImVec2(x + w, y + h), U(a), U(b), U(b), U(a));
}

// ---------------------------------------------------------------- text
enum Al { Left, Center, Right, UpperLeft };

float fsz(float base, float minSize = 12.f) { return std::max(minSize, std::round(base * S)); }

void text(float x, float y, float w, float h, const std::string& t, float size, Col c, Al al, bool bold = false, bool wrap = false) {
    if (c.w <= 0.003f || t.empty()) return;
    ImVec2 ts = gFont->CalcTextSizeA(size, FLT_MAX, wrap ? w : 0.f, t.c_str());
    float px = x, py = y;
    if (al == Center) px = x + (w - ts.x) * 0.5f;
    else if (al == Right) px = x + w - ts.x;
    if (al == UpperLeft) py = y;
    else py = y + (h - ts.y) * 0.5f;
    bool clip = ts.x > w + 0.5f || ts.y > h + 0.5f;
    if (clip) D->PushClipRect(ImVec2(x, y), ImVec2(x + w, y + h), true);
    D->AddText(gFont, size, ImVec2(px, py), U(c), t.c_str(), nullptr, wrap ? w : 0.f);
    if (bold) D->AddText(gFont, size, ImVec2(px + size * 0.04f, py), U(c), t.c_str(), nullptr, wrap ? w : 0.f);
    if (clip) D->PopClipRect();
}

Col whiteA(float a) { return Col(1.f, 1.f, 1.f, a); }
void lbl(float x, float y, float w, float h, const std::string& t, Al al, float alpha = 1.f) {
    text(x, y, w, h, t, al == Right ? fsz(11.f) : fsz(11.5f), whiteA(alpha), al);
}
void small(float x, float y, float w, float h, const std::string& t, Al al, bool bold = false) {
    text(x, y, w, h, t, fsz(9.f, 10.5f), Col(0.55f, 0.60f, 0.68f, 1.f), al, bold);
}

std::string fmt(float v, const char* f) {
    int dec = std::max(0, (int)strlen(f) - 2);
    float eps = 0.5f * std::pow(10.f, -(float)dec);
    if (std::fabs(v) < eps) v = 0.f;  // no "-0"
    char b[32];
    snprintf(b, sizeof(b), "%.*f", dec, v);
    return b;
}

// ---------------------------------------------------------------- the eye
// outline (two arcs that meet at the corners), iris ring and pupil
void eyeGlyph(float cxp, float cyp, float size, Col c) {
    float half = size * 0.5f;
    const int N = 22;
    ImVec2 top[N], bot[N];
    for (int i = 0; i < N; i++) {
        float u = -1.f + 2.f * i / (N - 1);
        float hl = 0.55f * (1.f - u * u);
        top[i] = ImVec2(cxp + u * half, cyp - hl * half);
        bot[i] = ImVec2(cxp + u * half, cyp + hl * half);
    }
    float th = std::max(1.f, 0.11f * half);
    D->AddPolyline(top, N, U(c), 0, th);
    D->AddPolyline(bot, N, U(c), 0, th);
    D->AddCircle(ImVec2(cxp, cyp), 0.27f * half, U(c), 28, std::max(1.f, 0.14f * half));
    D->AddCircleFilled(ImVec2(cxp, cyp), 0.11f * half, U(c), 20);
}

// ================================================================ hit areas (filled while drawing)
struct Hit {
    float x = 0, y = 0, w = 0, h = 0;
    Act tap;
    SetB setB;
    bool val = false;
    Pick pick;
    int index = 0;
    bool slider = false;
    SetF set;
    SetB enable;
    Act release;
    float tx = 0, tw = 1, mn = 0, mx = 1, step = 0;
};
std::vector<Hit> hits;

Hit& newHit(float x, float y, float w, float h) {
    hits.emplace_back();
    Hit& t = hits.back();
    t.x = x;
    t.y = y;
    t.w = w;
    t.h = h;
    return t;
}

bool inside(float px, float py, float x, float y, float w, float h) { return px >= x && px <= x + w && py >= y && py <= y + h; }

// a change of a setting: the main thread gets it, and the file is written a moment later
bool gDirty = false;
void changed() { gDirty = true; }
void saveSoon() {
    gNeedSave = true;
    gSaveAt = gTime + 1.0f;
}

// ================================================================ layout
void layout() {
    S = clampf(H / 540.f, 0.8f, 3.f) * ui.scale;
    ES = clampf(44.f * S * ui.iconSize, 24.f, H * 0.3f);
    if (ui.eyeX < 0.f || ui.eyeY < 0.f) {
        ui.eyeX = W - ES - 10.f * S;
        ui.eyeY = H * 0.30f;
    }
    ui.eyeX = clampf(ui.eyeX, 0.f, std::max(0.f, W - ES));
    ui.eyeY = clampf(ui.eyeY, 0.f, std::max(0.f, H - ES));

    fpsW = 88.f * S;
    fpsH = 24.f * S;
    graphW = 150.f * S;
    graphH = 44.f * S;
    fpsBoxW = ui.showGraph ? std::max(fpsW, graphW) : fpsW;
    fpsBoxH = (ui.showFps ? fpsH : 0.f) + (ui.showGraph ? (ui.showFps ? 4.f * S : 0.f) + graphH : 0.f);
    if (fpsBoxH < fpsH) fpsBoxH = fpsH;
    if (ui.fpsX < 0.f || ui.fpsY < 0.f) {
        ui.fpsX = 8.f * S;
        ui.fpsY = 8.f * S;
    }
    ui.fpsX = clampf(ui.fpsX, 0.f, std::max(0.f, W - fpsBoxW));
    ui.fpsY = clampf(ui.fpsY, 0.f, std::max(0.f, H - fpsBoxH));

    // the split panel: its size follows the text
    {
        float sc = S * fx.tas.splitsScale;
        splitFont = std::max(10.f, std::round(11.f * sc));
        splitLine = splitFont * 1.4f;
        float nameW = 0.f;
        if (gFont)
            for (const SplitRow& r : gSv.rows) nameW = std::max(nameW, gFont->CalcTextSizeA(splitFont, FLT_MAX, 0, r.name.c_str()).x);
        splitBoxW = std::max(130.f * sc, nameW + 76.f * sc);
        splitBoxH = (float)(gSv.rows.size() + 1) * splitLine + 10.f * sc;
        if (ui.splitX < 0.f || ui.splitY < 0.f) {
            ui.splitX = 8.f * S;
            ui.splitY = 8.f * S + fpsBoxH + 8.f * S;
        }
        ui.splitX = clampf(ui.splitX, 0.f, std::max(0.f, W - splitBoxW));
        ui.splitY = clampf(ui.splitY, 0.f, std::max(0.f, H - splitBoxH));
    }

    pw = std::min(W - 12.f * S, 540.f * S);
    ph = std::min(H - 12.f * S, 340.f * S);
    if (ui.panX < 0.f || ui.panY < 0.f) {
        ui.panX = std::max(6.f * S, ui.eyeX - pw - 8.f * S);
        ui.panY = (H - ph) * 0.5f;
    }
    ui.panX = clampf(ui.panX, 0.f, std::max(0.f, W - pw));
    ui.panY = clampf(ui.panY, 0.f, std::max(0.f, H - ph));

    hdrH = 40.f * S;
    ftH = 22.f * S;
    sbW = std::min(108.f * S, pw * 0.27f);
    kx = ui.panX + sbW + 4.f * S;
    ky = ui.panY + hdrH;
    kw = pw - sbW - 10.f * S;
    kBot = ui.panY + ph - ftH - 6.f * S;
    cTop = ky + 6.f * S;
    cBot = kBot - 6.f * S;
    cx = kx + 8.f * S;
    cw = kw - 16.f * S - 6.f * S;
}

// ================================================================ widgets
bool vis(float h) { return cy >= cTop - 0.5f && cy + h <= cBot + 0.5f; }

void drawSwitch(float x, float y, float w, float h, bool on) {
    rfill(x, y, w, h, h * 0.5f, on ? accentNow() : shade(bgCol(), 0.22f));
    float kr = h * 0.5f - 2.f * S;
    disc(on ? x + w - h * 0.5f : x + h * 0.5f, y + h * 0.5f, kr, White);
}

void drawTrack(float tx, float cyy, float tw, float frac, bool on) {
    Col acc = on ? accentNow() : shade(bgCol(), 0.30f);
    frac = clampf(frac, 0.f, 1.f);
    rfill(tx, cyy - 2.f * S, tw, 4.f * S, 2.f * S, shade(bgCol(), 0.18f));
    rfill(tx, cyy - 2.f * S, tw * frac, 4.f * S, 2.f * S, acc);
    disc(tx + tw * frac, cyy, 7.f * S, on ? White : Col(0.7f, 0.72f, 0.76f, 1.f));
}

void toggle(const std::string& label, bool on, SetB set) {
    float h = 34.f * S;
    if (vis(h)) {
        float y = cy, rh = h - 3.f * S;
        rfill(cx, y, cw, rh, 7.f * S, cardCol());
        lbl(cx + 10.f * S, y, cw - 70.f * S, rh, label, Left);
        drawSwitch(cx + cw - 46.f * S, y + (rh - 20.f * S) * 0.5f, 36.f * S, 20.f * S, on);
        Hit& t = newHit(cx, y, cw, rh);
        t.setB = set;
        t.val = on;
    }
    cy += h;
}

// a name, a slider, the value and a switch on one row; moving the slider also switches the effect on
void fxRow(const std::string& label, bool on, SetB setOn, float value, float mn, float mx, float step, const char* f, SetF set) {
    float h = 36.f * S;
    if (vis(h)) {
        float y = cy, rh = h - 3.f * S;
        rfill(cx, y, cw, rh, 7.f * S, cardCol());
        float sw = 32.f * S, sh = 18.f * S;
        float sx = cx + cw - sw - 8.f * S;
        drawSwitch(sx, y + (rh - sh) * 0.5f, sw, sh, on);
        Hit& t = newHit(sx - 6.f * S, y, sw + 12.f * S, rh);
        t.setB = setOn;
        t.val = on;

        float lx = cx + 10.f * S, lw = 84.f * S;
        lbl(lx, y, lw, rh, label, Left, on ? 1.f : 0.6f);
        float vw = 42.f * S;
        float vx = sx - 6.f * S - vw;
        float tx = lx + lw + 4.f * S;
        float tw = vx - 6.f * S - tx;
        drawTrack(tx, y + rh * 0.5f, tw, (value - mn) / (mx - mn), on);
        lbl(vx, y, vw, rh, fmt(value, f), Right, on ? 1.f : 0.6f);

        Hit& sl = newHit(tx - 8.f * S, y, tw + 16.f * S, rh);
        sl.slider = true;
        sl.set = set;
        sl.enable = setOn;
        sl.tx = tx;
        sl.tw = tw;
        sl.mn = mn;
        sl.mx = mx;
        sl.step = step;
    }
    cy += h;
}

// a name and a slider on one row (no switch)
void slider(const std::string& label, float value, float mn, float mx, float step, const char* f, SetF set, Act release = nullptr) {
    float h = 36.f * S;
    if (vis(h)) {
        float y = cy, rh = h - 3.f * S;
        rfill(cx, y, cw, rh, 7.f * S, cardCol());
        float lx = cx + 10.f * S, lw = 100.f * S;
        lbl(lx, y, lw, rh, label, Left);
        float vw = 44.f * S;
        float tx = lx + lw + 4.f * S;
        float tw = cx + cw - vw - 8.f * S - tx;
        drawTrack(tx, y + rh * 0.5f, tw, (value - mn) / (mx - mn), true);
        lbl(tx + tw + 8.f * S, y, vw, rh, fmt(value, f), Right);

        Hit& sl = newHit(tx - 8.f * S, y, tw + 16.f * S, rh);
        sl.slider = true;
        sl.set = set;
        sl.release = release;
        sl.tx = tx;
        sl.tw = tw;
        sl.mn = mn;
        sl.mx = mx;
        sl.step = step;
    }
    cy += h;
}

// style 0: plain accent bar; 1: rainbow; 2: a gradient from c0 to c1
void drawFxTrack(float tx, float cyy, float tw, float frac, int style, Col c0, Col c1, bool en) {
    float th = 6.f * S;
    float y = cyy - th * 0.5f;
    frac = clampf(frac, 0.f, 1.f);
    float dim = en ? 1.f : 0.45f;
    if (style == 1) {
        for (int i = 0; i < 6; i++) {
            Col a = hueRgb(i / 6.f), b = hueRgb((i + 1) / 6.f);
            hgrad(tx + tw * i / 6.f, y, tw / 6.f + 0.5f, th, withA(a, dim), withA(b, dim));
        }
    } else if (style == 2) {
        hgrad(tx, y, tw, th, withA(c0, dim), withA(c1, dim));
    } else {
        rfill(tx, y, tw, th, th * 0.5f, shade(bgCol(), 0.18f));
        rfill(tx, y, tw * frac, th, th * 0.5f, en ? accentNow() : shade(bgCol(), 0.30f));
    }
    float kxp = tx + tw * frac;
    disc(kxp, cyy, 8.f * S, withA(White, dim));
    disc(kxp, cyy, 3.5f * S, en ? accentNow() : shade(bgCol(), 0.30f));
}

// a tall row: name, value and (when setOn is given) a switch on top, a wide coloured slider below
void wideFx(const std::string& label, bool on, SetB setOn, float value, float mn, float mx, float step, const char* f, SetF set,
            int style, Col c0, Col c1) {
    float h = 50.f * S;
    if (vis(h)) {
        float y = cy, rh = h - 3.f * S, pad = 10.f * S;
        bool en = !setOn || on;
        rfill(cx, y, cw, rh, 7.f * S, cardCol());
        float right = cx + cw - pad;
        if (setOn) {
            float sw = 34.f * S, sh = 18.f * S, sx = right - sw;
            drawSwitch(sx, y + 6.f * S, sw, sh, on);
            Hit& t = newHit(sx - 8.f * S, y, sw + 16.f * S, 28.f * S);
            t.setB = setOn;
            t.val = on;
            right = sx - 8.f * S;
        }
        lbl(cx + pad, y + 2.f * S, cw * 0.5f, 26.f * S, label, Left, en ? 1.f : 0.6f);
        lbl(right - 60.f * S, y + 2.f * S, 60.f * S, 26.f * S, fmt(value, f), Right, en ? 1.f : 0.6f);
        float tx = cx + pad + 4.f * S, tw = cw - 2.f * pad - 8.f * S;
        float cyy = y + rh - 11.f * S;
        drawFxTrack(tx, cyy, tw, (value - mn) / (mx - mn), style, c0, c1, en);
        Hit& sl = newHit(tx - 10.f * S, y + 26.f * S, tw + 20.f * S, rh - 26.f * S);
        sl.slider = true;
        sl.set = set;
        sl.enable = setOn;
        sl.tx = tx;
        sl.tw = tw;
        sl.mn = mn;
        sl.mx = mx;
        sl.step = step;
    }
    cy += h;
}

// preset thumbnails: a small landscape in the colours of each look (sky top, sky bottom, hills)
const float kThumb[12][9] = {
    {0.45f, 0.62f, 0.78f, 0.78f, 0.84f, 0.82f, 0.14f, 0.24f, 0.17f},  // Default
    {0.95f, 0.62f, 0.30f, 1.00f, 0.85f, 0.55f, 0.26f, 0.14f, 0.08f},  // Warm
    {0.18f, 0.33f, 0.60f, 0.55f, 0.76f, 0.90f, 0.07f, 0.12f, 0.20f},  // Cold
    {0.55f, 0.20f, 0.90f, 1.00f, 0.50f, 0.70f, 0.12f, 0.06f, 0.25f},  // Vivid
    {0.10f, 0.30f, 0.40f, 0.90f, 0.60f, 0.35f, 0.05f, 0.08f, 0.10f},  // Cinematic
    {0.20f, 0.20f, 0.21f, 0.70f, 0.70f, 0.72f, 0.04f, 0.04f, 0.05f},  // Noir
    {0.02f, 0.12f, 0.04f, 0.10f, 0.55f, 0.20f, 0.02f, 0.18f, 0.06f},  // Night vision
    {0.35f, 0.25f, 0.55f, 0.85f, 0.55f, 0.60f, 0.20f, 0.15f, 0.30f},  // VHS
    {0.75f, 0.65f, 0.90f, 1.00f, 0.88f, 0.92f, 0.55f, 0.50f, 0.70f},  // Dreamy
    {0.05f, 0.10f, 0.08f, 0.25f, 0.32f, 0.26f, 0.02f, 0.04f, 0.03f},  // Horror
    {0.95f, 0.50f, 0.15f, 1.00f, 0.85f, 0.40f, 0.30f, 0.15f, 0.05f},  // Golden hour
    {0.45f, 0.34f, 0.22f, 0.80f, 0.66f, 0.45f, 0.20f, 0.14f, 0.08f},  // Sepia
};

void drawThumb(int i, float x, float y, float w, float h) {
    const float* c = kThumb[i];
    const int N = 24;
    D->PushClipRect(ImVec2(x, y), ImVec2(x + w, y + h), true);
    for (int k = 0; k < N; k++) {
        float u = (k + 0.5f) / N;
        float hill = 0.30f + 0.10f * std::sin(u * 7.f) + 0.07f * std::sin(u * 17.f + 1.f);
        float x0 = x + w * k / N, x1 = x + w * (k + 1) / N + 0.6f;
        float yh = y + h * (1.f - hill);  // the top of the hill (v grows upwards)
        // sky: the bottom colour at the hill line, the top colour at the top
        D->AddRectFilledMultiColor(ImVec2(x0, y), ImVec2(x1, yh), U(Col(c[0], c[1], c[2], 1.f)), U(Col(c[0], c[1], c[2], 1.f)),
                                   U(Col(c[3], c[4], c[5], 1.f)), U(Col(c[3], c[4], c[5], 1.f)));
        // ground: a little brighter towards the top
        float gt = 0.8f + 0.5f * hill, gb = 0.8f;
        D->AddRectFilledMultiColor(ImVec2(x0, yh), ImVec2(x1, y + h), U(Col(c[6] * gt, c[7] * gt, c[8] * gt, 1.f)),
                                   U(Col(c[6] * gt, c[7] * gt, c[8] * gt, 1.f)), U(Col(c[6] * gb, c[7] * gb, c[8] * gb, 1.f)),
                                   U(Col(c[6] * gb, c[7] * gb, c[8] * gb, 1.f)));
    }
    D->PopClipRect();
}

void presets(const std::vector<std::string>& names, int sel, Pick pick) {
    const int perRow = 6;
    int total = (int)names.size();
    int rows = (total + perRow - 1) / perRow;
    float rowH = 78.f * S;
    float h = rowH * rows;
    if (vis(h)) {
        float gap = 6.f * S;
        float w = (cw - 12.f * S - (perRow - 1) * gap) / perRow;
        float th = 46.f * S;
        for (int r = 0; r < rows; r++) {
            float y = cy + r * rowH;
            float x = cx + 6.f * S;
            for (int i = r * perRow; i < total && i < (r + 1) * perRow; i++) {
                drawThumb(i, x, y + 3.f * S, w, th);
                if (i == sel) rring(x - 2.f * S, y + 1.f * S, w + 4.f * S, th + 4.f * S, 5.f * S, 2.f * S, accentNow());
                lbl(x - 4.f * S, y + th + 5.f * S, w + 8.f * S, 18.f * S, names[i], Center, i == sel ? 1.f : 0.75f);
                Hit& t = newHit(x - 2.f * S, y, w + 4.f * S, th + 24.f * S);
                t.pick = pick;
                t.index = i;
                x += w + gap;
            }
        }
    }
    cy += h;
}

void section(const std::string& t) {
    float h = 24.f * S;
    if (vis(h)) small(cx + 4.f * S, cy + 4.f * S, cw - 8.f * S, h - 4.f * S, t, Left, true);
    cy += h;
}

void swatches(const Palette& pal, int sel, Pick pick) {
    float h = 34.f * S;
    if (vis(h)) {
        float y = cy;
        int n = (int)pal.size();
        float gap = 6.f * S;
        float size = std::min(26.f * S, (cw - 16.f * S - (n - 1) * gap) / n);
        float x = cx + 8.f * S;
        for (int i = 0; i < n; i++) {
            float sy = y + (h - 3.f * S - size) * 0.5f;
            rfill(x, sy, size, size, 6.f * S, fromP(pal[i]));
            if (i == sel) rring(x - 2.f * S, sy - 2.f * S, size + 4.f * S, size + 4.f * S, 7.f * S, 2.f * S, White);
            Hit& t = newHit(x - 2.f * S, y, size + 4.f * S, h - 3.f * S);
            t.pick = pick;
            t.index = i;
            x += size + gap;
        }
    }
    cy += h;
}

void segments(const std::vector<std::string>& names, int sel, Pick pick, float hmul) {
    float h = 36.f * S * hmul;
    if (vis(h)) {
        float y = cy;
        int n = (int)names.size();
        float gap = 6.f * S;
        float w = (cw - (n - 1) * gap) / n;
        float x = cx;
        for (int i = 0; i < n; i++) {
            rfill(x, y, w, h - 3.f * S, 7.f * S, i == sel ? accentNow() : cardCol());
            lbl(x, y, w, h - 3.f * S, names[i], Center);
            Hit& t = newHit(x, y, w, h - 3.f * S);
            t.pick = pick;
            t.index = i;
            x += w + gap;
        }
    }
    cy += h;
}

void button(const std::string& label, Act tap) {
    float h = 36.f * S;
    if (vis(h)) {
        float y = cy;
        rfill(cx, y, cw, h - 3.f * S, 7.f * S, cardCol());
        rring(cx, y, cw, h - 3.f * S, 7.f * S, 1.f * S, withA(accentNow(), 0.7f));
        lbl(cx, y, cw, h - 3.f * S, label, Center);
        Hit& t = newHit(cx, y, cw, h - 3.f * S);
        t.tap = tap;
    }
    cy += h;
}

void caption(const std::string& t, int lines = 1) {
    float h = (8.f + 12.f * lines) * S;
    if (vis(h)) text(cx + 4.f * S, cy, cw - 8.f * S, h, t, fsz(10.f), Col(0.62f, 0.66f, 0.72f, 1.f), UpperLeft, false, true);
    cy += h;
}

// ================================================================ the effect values and presets
void resetValues() {
    bool on = fx.on;
    FxSettings d;  // the defaults of every effect value
    d.on = on;
    // keep fog and performance: only the screen effects are reset
    d.fogMode = fx.fogMode; d.fogR = fx.fogR; d.fogG = fx.fogG; d.fogB = fx.fogB; d.fogThick = fx.fogThick;
    d.perfLevel = fx.perfLevel; d.noTextures = fx.noTextures; d.hideParticles = fx.hideParticles; d.noPostFx = fx.noPostFx;
    d.noShadow = fx.noShadow; d.drawDistOn = fx.drawDistOn; d.drawDist = fx.drawDist; d.resIndex = fx.resIndex;
    d.customRes = fx.customRes; d.customW = fx.customW; d.customH = fx.customH; d.squashOn = fx.squashOn;
    d.squash = fx.squash; d.fpsIndex = fx.fpsIndex; d.autoClean = fx.autoClean; d.cleanReq = fx.cleanReq;
    d.ambientOn = fx.ambientOn; d.ambient = fx.ambient; d.lightBoostOn = fx.lightBoostOn; d.lightBoost = fx.lightBoost;
    d.fovOn = fx.fovOn; d.fov = fx.fov; d.adaptive = fx.adaptive; d.adaptTarget = fx.adaptTarget;
    d.aaMode = fx.aaMode; d.anisoMode = fx.anisoMode; d.lodOn = fx.lodOn; d.lodBias = fx.lodBias;
    d.cheat = fx.cheat;  // the Cheat tab has nothing to do with the look: presets and "Reset" leave it alone
    d.espItems = fx.espItems; d.espEnemies = fx.espEnemies; d.espWalls = fx.espWalls;
    d.espItemColor = fx.espItemColor; d.espEnemyColor = fx.espEnemyColor;
    d.espItemWidth = fx.espItemWidth; d.espEnemyWidth = fx.espEnemyWidth;
    fx = d;
}

void applyPreset(int i) {
    resetValues();
    switch (i) {
        case 1:  // Warm
            fx.warmOn = true; fx.temperature = 35.f; fx.tint = 10.f;
            fx.satOn = true; fx.saturation = 10.f;
            fx.contrastOn = true; fx.contrast = 5.f;
            break;
        case 2:  // Cold
            fx.warmOn = true; fx.temperature = -35.f; fx.tint = -8.f;
            fx.satOn = true; fx.saturation = -5.f;
            fx.contrastOn = true; fx.contrast = 8.f;
            break;
        case 3:  // Vivid
            fx.satOn = true; fx.saturation = 45.f;
            fx.contrastOn = true; fx.contrast = 18.f;
            fx.bloomOn = true; fx.bloom = 0.8f; fx.bloomThreshold = 0.7f;
            break;
        case 4:  // Cinematic
            fx.contrastOn = true; fx.contrast = 22.f;
            fx.satOn = true; fx.saturation = -15.f;
            fx.warmOn = true; fx.temperature = 12.f; fx.tint = -6.f;
            fx.vignetteOn = true; fx.vignette = 0.4f;
            fx.grainOn = true; fx.grain = 0.2f;
            break;
        case 5:  // Noir
            fx.satOn = true; fx.saturation = -100.f;
            fx.contrastOn = true; fx.contrast = 28.f;
            fx.exposureOn = true; fx.exposure = -0.2f;
            fx.vignetteOn = true; fx.vignette = 0.5f;
            fx.grainOn = true; fx.grain = 0.3f;
            break;
        case 6:  // Night vision
            fx.filterOn = true; fx.fr = 0.40f; fx.fg = 1.00f; fx.fb = 0.45f; fx.filterStrength = 1.f;
            fx.satOn = true; fx.saturation = -60.f;
            fx.exposureOn = true; fx.exposure = 0.8f;
            fx.contrastOn = true; fx.contrast = 15.f;
            fx.bloomOn = true; fx.bloom = 1.2f; fx.bloomThreshold = 0.6f;
            fx.vignetteOn = true; fx.vignette = 0.55f; fx.vignetteSmooth = 0.5f;
            fx.grainOn = true; fx.grain = 0.45f; fx.grainSize = 1.2f;
            break;
        case 7:  // VHS
            fx.satOn = true; fx.saturation = 20.f;
            fx.contrastOn = true; fx.contrast = -10.f;
            fx.warmOn = true; fx.temperature = 8.f; fx.tint = 12.f;
            fx.chromaOn = true; fx.chroma = 0.5f;
            fx.lensOn = true; fx.lens = 10.f;
            fx.grainOn = true; fx.grain = 0.35f;
            fx.vignetteOn = true; fx.vignette = 0.3f;
            break;
        case 8:  // Dreamy
            fx.bloomOn = true; fx.bloom = 3.f; fx.bloomThreshold = 0.5f;
            fx.satOn = true; fx.saturation = 15.f;
            fx.exposureOn = true; fx.exposure = 0.3f;
            fx.contrastOn = true; fx.contrast = -12.f;
            fx.warmOn = true; fx.temperature = 10.f; fx.tint = 8.f;
            fx.dofOn = true; fx.dofFocus = 6.f; fx.dofAperture = 5.6f; fx.dofLength = 50.f; fx.dofQuality = 0;
            break;
        case 9:  // Horror
            fx.satOn = true; fx.saturation = -45.f;
            fx.contrastOn = true; fx.contrast = 25.f;
            fx.exposureOn = true; fx.exposure = -0.5f;
            fx.warmOn = true; fx.temperature = -12.f; fx.tint = -12.f;
            fx.vignetteOn = true; fx.vignette = 0.65f; fx.vignetteSmooth = 0.4f;
            fx.grainOn = true; fx.grain = 0.35f;
            fx.aoOn = true; fx.aoIntensity = 1.6f; fx.aoRadius = 0.6f; fx.aoQuality = 1;
            break;
        case 10:  // Golden hour
            fx.warmOn = true; fx.temperature = 45.f; fx.tint = 12.f;
            fx.exposureOn = true; fx.exposure = 0.2f;
            fx.satOn = true; fx.saturation = 20.f;
            fx.contrastOn = true; fx.contrast = 10.f;
            fx.bloomOn = true; fx.bloom = 1.2f; fx.bloomThreshold = 0.7f;
            fx.vignetteOn = true; fx.vignette = 0.25f;
            break;
        case 11:  // Sepia
            fx.filterOn = true; fx.fr = 1.00f; fx.fg = 0.82f; fx.fb = 0.55f; fx.filterStrength = 1.f;
            fx.satOn = true; fx.saturation = -70.f;
            fx.contrastOn = true; fx.contrast = 12.f;
            fx.vignetteOn = true; fx.vignette = 0.4f;
            fx.grainOn = true; fx.grain = 0.2f;
            break;
        default: break;
    }
    if (i > 0) fx.on = true;
}

// ================================================================ pages
void pageGeneral() {
    toggle("Effects on", fx.on, [](bool v) { fx.on = v; });
    caption(st.visual);
    section("PRESETS");
    presets(kPresetNames, gPresetSel, [](int i) { applyPreset(i); gPresetSel = i; });
    button("Reset to default", [] { resetValues(); gPresetSel = -1; });
    section("FOG");
    segments(kFogModes, fx.fogMode, [](int i) { fx.fogMode = i; }, 1.f);
    caption(fx.fogMode == 1 ? "Fog is off."
            : fx.fogMode == 2 ? "Custom fog: " + fmt(fx.fogThick, "0") + "% thick."
                              : "Fog is controlled by the game.");
    swatches(kFogPalette, indexIn(kFogPalette, fx.fogR, fx.fogG, fx.fogB), [](int i) {
        fx.fogR = kFogPalette[i][0]; fx.fogG = kFogPalette[i][1]; fx.fogB = kFogPalette[i][2];
        fx.fogMode = 2;  // picking a colour means "use my fog"
    });
    slider("Thickness", fx.fogThick, 0.f, 100.f, 1.f, "0", [](float v) { fx.fogThick = v; fx.fogMode = 2; });
    section("DISPLAY");
    toggle("Show FPS", ui.showFps, [](bool v) { ui.showFps = v; });
    toggle("Show frame graph", ui.showGraph, [](bool v) { ui.showGraph = v; });
}

void pageColor() {
    segments(kColorSub, ui.colorSub, [](int i) { ui.colorSub = i; scroll = 0.f; }, 0.85f);
    switch (ui.colorSub) {
        case 0:
            wideFx("Hue", fx.hueOn, [](bool v) { fx.hueOn = v; }, fx.hue, -180.f, 180.f, 5.f, "0", [](float v) { fx.hue = v; }, 1, CDark, CDark);
            wideFx("Saturation", fx.satOn, [](bool v) { fx.satOn = v; }, fx.saturation, -100.f, 100.f, 5.f, "0", [](float v) { fx.saturation = v; }, 2, CGrey, CMagenta);
            wideFx("Contrast", fx.contrastOn, [](bool v) { fx.contrastOn = v; }, fx.contrast, -100.f, 100.f, 5.f, "0", [](float v) { fx.contrast = v; }, 2, CDark, White);
            break;
        case 1:
            wideFx("Warmth", fx.warmOn, [](bool v) { fx.warmOn = v; }, fx.temperature, -100.f, 100.f, 5.f, "0", [](float v) { fx.temperature = v; }, 2, CBlue, COrange);
            caption("Cool (left) to warm (right).");
            break;
        case 2:
            wideFx("Tint", fx.warmOn, [](bool v) { fx.warmOn = v; }, fx.tint, -100.f, 100.f, 5.f, "0", [](float v) { fx.tint = v; }, 2, CGreen, CMagenta);
            caption("Green (left) to magenta (right).");
            break;
        default:
            wideFx("Colour filter", fx.filterOn, [](bool v) { fx.filterOn = v; }, fx.filterStrength, 0.f, 1.f, 0.05f, "0.00", [](float v) { fx.filterStrength = v; }, 0, CDark, CDark);
            wideFx("Red", true, nullptr, fx.fr, 0.f, 1.f, 0.01f, "0.00", [](float v) { fx.fr = v; fx.filterOn = true; }, 2, CDark, CRed);
            wideFx("Green", true, nullptr, fx.fg, 0.f, 1.f, 0.01f, "0.00", [](float v) { fx.fg = v; fx.filterOn = true; }, 2, CDark, CGreen);
            wideFx("Blue", true, nullptr, fx.fb, 0.f, 1.f, 0.01f, "0.00", [](float v) { fx.fb = v; fx.filterOn = true; }, 2, CDark, CBlue);
            swatches(kFilterPalette, indexIn(kFilterPalette, fx.fr, fx.fg, fx.fb), [](int i) {
                fx.fr = kFilterPalette[i][0]; fx.fg = kFilterPalette[i][1]; fx.fb = kFilterPalette[i][2];
                fx.filterOn = true;  // picking a colour means "use it"
            });
            break;
    }
}

void pageEffects() {
    fxRow("Exposure", fx.exposureOn, [](bool v) { fx.exposureOn = v; }, fx.exposure, -5.f, 5.f, 0.1f, "0.0", [](float v) { fx.exposure = v; });
    fxRow("Gamma", fx.gammaOn, [](bool v) { fx.gammaOn = v; }, fx.gamma, -1.f, 1.f, 0.05f, "0.00", [](float v) { fx.gamma = v; });
    fxRow("Bloom", fx.bloomOn, [](bool v) { fx.bloomOn = v; }, fx.bloom, 0.f, 10.f, 0.1f, "0.0", [](float v) { fx.bloom = v; });
    fxRow("Vignette", fx.vignetteOn, [](bool v) { fx.vignetteOn = v; }, fx.vignette, 0.f, 1.f, 0.05f, "0.00", [](float v) { fx.vignette = v; });
    fxRow("Film grain", fx.grainOn, [](bool v) { fx.grainOn = v; }, fx.grain, 0.f, 1.f, 0.05f, "0.00", [](float v) { fx.grain = v; });
    button("Reset to default", [] { resetValues(); gPresetSel = -1; });
    section("DARK SCENES");
    fxRow("Brightness", fx.ambientOn, [](bool v) { fx.ambientOn = v; }, fx.ambient, 0.f, 100.f, 1.f, "0", [](float v) { fx.ambient = v; });
    fxRow("Light boost", fx.lightBoostOn, [](bool v) { fx.lightBoostOn = v; }, fx.lightBoost, 1.f, 3.f, 0.1f, "0.0", [](float v) { fx.lightBoost = v; });
    fxRow("Field of view", fx.fovOn, [](bool v) { fx.fovOn = v; }, fx.fov, 40.f, 110.f, 1.f, "0", [](float v) { fx.fov = v; });
    caption("Brightness: adds flat light, so shadowed corners are less black.\nLight boost: makes the game's real-time lights stronger (flashlight, lamps). Light that is baked into the map does not change; for that, use Exposure or Gamma above.\nThese stay when you pick a preset.", 6);
    if (!st.scene.empty()) caption(st.scene, 1 + (int)std::count(st.scene.begin(), st.scene.end(), '\n'));
}

void pageAdvanced() {
    section("MOTION");
    fxRow("Motion blur", fx.blurOn, [](bool v) { fx.blurOn = v; }, fx.blurStrength, 0.f, 360.f, 10.f, "0", [](float v) { fx.blurStrength = v; });
    slider("Blur quality", (float)fx.blurSamples, 4.f, 32.f, 4.f, "0", [](float v) { fx.blurSamples = (int)std::lround(v); });
    section("LENS");
    fxRow("Chromatic", fx.chromaOn, [](bool v) { fx.chromaOn = v; }, fx.chroma, 0.f, 1.f, 0.05f, "0.00", [](float v) { fx.chroma = v; });
    fxRow("Distortion", fx.lensOn, [](bool v) { fx.lensOn = v; }, fx.lens, -100.f, 100.f, 5.f, "0", [](float v) { fx.lens = v; });
    section("DEPTH");
    fxRow("Ambient occlusion", fx.aoOn, [](bool v) { fx.aoOn = v; }, fx.aoIntensity, 0.f, 4.f, 0.1f, "0.0", [](float v) { fx.aoIntensity = v; });
    slider("AO radius", fx.aoRadius, 0.05f, 3.f, 0.05f, "0.00", [](float v) { fx.aoRadius = v; });
    segments(kAoQuality, fx.aoQuality, [](int i) { fx.aoQuality = i; }, 1.f);
    caption("Ambient occlusion darkens corners and creases. Higher quality costs more FPS.", 2);
    fxRow("Depth of field", fx.dofOn, [](bool v) { fx.dofOn = v; }, fx.dofFocus, 0.5f, 50.f, 0.5f, "0.0", [](float v) { fx.dofFocus = v; });
    slider("Aperture", fx.dofAperture, 1.f, 32.f, 0.5f, "0.0", [](float v) { fx.dofAperture = v; });
    slider("Focal length", fx.dofLength, 10.f, 300.f, 5.f, "0", [](float v) { fx.dofLength = v; });
    segments(kDofQuality, fx.dofQuality, [](int i) { fx.dofQuality = i; }, 1.f);
    caption("Depth of field: the slider is the focus distance in metres. What is farther or closer gets blurry. A lower aperture number blurs more. This costs FPS. Both effects only work if the game has them: the line on the General page says so.", 5);
    section("TUNING");
    slider("Bloom threshold", fx.bloomThreshold, 0.f, 2.f, 0.05f, "0.00", [](float v) { fx.bloomThreshold = v; });
    slider("Vignette softness", fx.vignetteSmooth, 0.05f, 1.f, 0.05f, "0.00", [](float v) { fx.vignetteSmooth = v; });
    slider("Grain size", fx.grainSize, 0.3f, 3.f, 0.1f, "0.0", [](float v) { fx.grainSize = v; });
}

void pagePerf() {
    section("GRAPHICS");
    segments(kLevelNames, fx.perfLevel, [](int i) { fx.perfLevel = i; }, 1.f);
    toggle("No textures", fx.noTextures, [](bool v) { fx.noTextures = v; });
    toggle("Hide particles", fx.hideParticles, [](bool v) { fx.hideParticles = v; });
    toggle("No post-processing", fx.noPostFx, [](bool v) { fx.noPostFx = v; });
    if (fx.noPostFx) caption("This also switches off the visual effects of this mod.", 2);
    toggle("No shadow", fx.noShadow, [](bool v) { fx.noShadow = v; });

    section("QUALITY");
    caption("Anti-aliasing");
    segments(kAaNames, fx.aaMode, [](int i) { fx.aaMode = i; }, 1.f);
    caption("Anisotropic filtering");
    segments(kAnisoNames, fx.anisoMode, [](int i) { fx.anisoMode = i; }, 1.f);
    fxRow("LOD bias", fx.lodOn, [](bool v) { fx.lodOn = v; }, fx.lodBias, 0.3f, 2.f, 0.1f, "0.0", [](float v) { fx.lodBias = v; });
    caption("Off or a lower number saves GPU power. Without anti-aliasing the edges get jagged, without anisotropic filtering sloped floors get blurry. A LOD bias under 1 switches objects to simpler models sooner. \"Game\" leaves it as the game has it. These win over the graphics level above.", 6);

    section("DISTANCE AND RESOLUTION");
    fxRow("Draw distance", fx.drawDistOn, [](bool v) { fx.drawDistOn = v; }, fx.drawDist, 20.f, 300.f, 10.f, "0", [](float v) { fx.drawDist = v; });
    caption("Resolution");
    segments(kResNames, fx.resIndex, [](int i) { fx.resIndex = i; }, 1.f);
    toggle("Adaptive resolution", fx.adaptive, [](bool v) { fx.adaptive = v; });
    if (fx.adaptive) {
        caption("Lowers the resolution (down to 50%) when the FPS stays under your target, and raises it again when there is room. Pick the FPS the game really runs at.", 3);
        segments(kTargetNames, fx.adaptTarget, [](int i) { fx.adaptTarget = i; }, 1.f);
    }
    toggle("Custom screen size", fx.customRes, [](bool v) {
        fx.customRes = v;
        if (v) {  // start from the size the game renders at now
            fx.customW = st.screenW > 0 ? st.screenW : 1280;
            fx.customH = st.screenH > 0 ? st.screenH : 720;
        }
    });
    if (fx.customRes) {
        slider("Width", (float)fx.customW, 320.f, 4096.f, 4.f, "0", [](float v) { fx.customW = (int)std::lround(v); });
        slider("Height", (float)fx.customH, 180.f, 4096.f, 4.f, "0", [](float v) { fx.customH = (int)std::lround(v); });
    }
    button("Back to screen size", [] { fx.customRes = false; fx.resIndex = 0; });
    caption(st.resolution.empty() ? "Resolution: the screen's own size." : st.resolution, 3);

    section("SCREEN SHAPE AND FPS");
    fxRow("Squash view", fx.squashOn, [](bool v) { fx.squashOn = v; }, fx.squash, 0.4f, 2.5f, 0.05f, "0.00", [](float v) { fx.squash = v; });
    caption("FPS cap");
    segments(kFpsNames, fx.fpsIndex, [](int i) { fx.fpsIndex = i; }, 1.f);

    section("MEMORY");
    toggle("Auto clean", fx.autoClean, [](bool v) { fx.autoClean = v; });
    button("Clean memory now", [] { fx.cleanReq++; });
    caption(st.perf.empty() ? "Nothing changed yet." : st.perf, 4);
}

// ---- Cheat tab
void cheatPlayer() {
    CheatSettings& c = fx.cheat;
    section("SURVIVAL");
    toggle("God mode", c.godMode, [](bool v) { fx.cheat.godMode = v; });
    caption("Enemies cannot attack you.");
    toggle("No fall damage", c.noFallDamage, [](bool v) { fx.cheat.noFallDamage = v; });
    toggle("No trap", c.noTrap, [](bool v) { fx.cheat.noTrap = v; });
    toggle("No cracking floor", c.noCrackFloor, [](bool v) { fx.cheat.noCrackFloor = v; });
    section("MOVEMENT");
    fxRow("Speed", c.speedOn, [](bool v) { fx.cheat.speedOn = v; }, c.speed, 1.f, 6.f, 0.1f, "0.0",
          [](float v) { fx.cheat.speed = v; });
    caption("1.0 is the normal walking speed.");
    toggle("No clip", c.noclip, [](bool v) { fx.cheat.noclip = v; });
    caption("Walk through walls. Look up and walk forward to rise, look down to sink. Strafing keeps the height.", 2);
    section("WEAPONS");
    toggle("Infinite ammo", c.infAmmo, [](bool v) { fx.cheat.infAmmo = v; });
    toggle("Infinite spray", c.infSpray, [](bool v) { fx.cheat.infSpray = v; });
}

void cheatEnemies() {
    CheatSettings& c = fx.cheat;
    int e = clampi(ui.cheatEnemy, 0, kEnemyCount - 1);
    const std::vector<std::string> row1(kEnemyNames, kEnemyNames + 4), row2(kEnemyNames + 4, kEnemyNames + kEnemyCount);
    section("ENEMY");
    segments(row1, e < 4 ? e : -1, [](int i) { ui.cheatEnemy = i; }, 0.9f);
    segments(row2, e >= 4 ? e - 4 : -1, [](int i) { ui.cheatEnemy = i + 4; }, 0.9f);
    std::string name = kEnemyNames[e];
    std::string up = name;
    for (char& ch : up) ch = (char)toupper((unsigned char)ch);
    section(up);
    // every enemy has its own set: only what the game really has for it is shown
    toggle("Freeze", c.enemy[e].freeze, [e](bool v) { fx.cheat.enemy[e].freeze = v; });
    caption(name + " cannot move.");
    if (kEnemyCan[e][1]) {
        toggle("Blind", c.enemy[e].blind, [e](bool v) { fx.cheat.enemy[e].blind = v; });
        caption(name + " cannot see you.");
    }
    if (kEnemyCan[e][2]) {
        toggle("Deaf", c.enemy[e].deaf, [e](bool v) { fx.cheat.enemy[e].deaf = v; });
        caption(name + " hears nothing: no cracking floor, no dropped items.", 2);
    }
    if (!kEnemyCan[e][1] || !kEnemyCan[e][2]) {
        std::string miss = !kEnemyCan[e][1] && !kEnemyCan[e][2] ? "Blind and Deaf are" : (!kEnemyCan[e][1] ? "Blind is" : "Deaf is");
        caption(miss + " not available for " + name + ": the game has no such behaviour for it.", 2);
    }
    button(std::string(kEnemyKill[e]) + " " + name, [e] { fx.cheat.killReq[e]++; });
    if (e == 2) caption("Mom Spider cannot die: this stuns her, like the game's own weapons.", 2);
    if (e == 6) caption("The Little One cannot die: this knocks it out, like the game's own weapons.", 2);
}

void cheatItems() {
    CheatSettings& c = fx.cheat;
    int sel = clampi(c.spawnItem, 0, kItemCount - 1);
    section("SPAWN ITEMS");
    slider("Item", (float)(sel + 1), 1.f, (float)kItemCount, 1.f, "0", [](float v) { fx.cheat.spawnItem = (int)std::lround(v) - 1; });
    caption(std::string("Selected: ") + kItemNames[sel]);
    button("Spawn item", [] { fx.cheat.spawnReq++; });
    section("PICK UP");
    toggle("Auto grab items", c.autoGrab, [](bool v) { fx.cheat.autoGrab = v; });
    caption("Picks up the items around you by itself.");
    fxRow("Long reach", c.longReachOn, [](bool v) { fx.cheat.longReachOn = v; }, c.reach, 2.f, 40.f, 1.f, "0",
          [](float v) { fx.cheat.reach = v; });
    caption("Items, doors and everything you can interact with work from far away. The slider is the distance in metres.", 2);
}

void espColorRows(int sel, const std::function<void(int)>& pick) {
    const std::vector<std::string> row1(kEspColorNames, kEspColorNames + 4), row2(kEspColorNames + 4, kEspColorNames + 8);
    segments(row1, sel < 4 ? sel : -1, [pick](int i) { pick(i); }, 0.9f);
    segments(row2, sel >= 4 ? sel - 4 : -1, [pick](int i) { pick(i + 4); }, 0.9f);
}

void cheatEsp() {
    section("ITEMS");
    toggle("Item outline", fx.espItems, [](bool v) { fx.espItems = v; });
    espColorRows(fx.espItemColor, [](int i) { fx.espItemColor = i; });
    slider("Thickness", fx.espItemWidth, 1.f, 8.f, 0.5f, "0.0", [](float v) { fx.espItemWidth = v; });
    section("ENTITIES");
    toggle("Entity outline", fx.espEnemies, [](bool v) { fx.espEnemies = v; });
    espColorRows(fx.espEnemyColor, [](int i) { fx.espEnemyColor = i; });
    slider("Thickness", fx.espEnemyWidth, 1.f, 8.f, 0.5f, "0.0", [](float v) { fx.espEnemyWidth = v; });
    section("VIEW");
    toggle("Through walls", fx.espWalls, [](bool v) { fx.espWalls = v; });
    caption("Off: the outline shows only where you can see the object.");
    caption("The outline follows the real shape. The game's own outline component draws it.", 2);
    if (!st.esp.empty()) caption(st.esp, 2);
}

void pageCheat() {
    segments(kCheatSub, ui.cheatSub, [](int i) { ui.cheatSub = i; scroll = 0.f; }, 0.85f);
    if (!st.cheat.empty()) caption(st.cheat, 3);
    switch (ui.cheatSub) {
        case 0: cheatPlayer(); break;
        case 1: cheatEnemies(); break;
        case 2: cheatItems(); break;
        default: cheatEsp(); break;
    }
}

void pageTas() {
    segments(kTasSub, ui.tasSub, [](int i) { ui.tasSub = i; scroll = 0.f; }, 0.85f);
    if (!st.tas.empty()) caption(st.tas, 3);
    TasSettings& t = fx.tas;
    switch (ui.tasSub) {
        case 0: {  // ---- time
            section("SPEEDHACK");
            fxRow("Speedhack", t.speedOn, [](bool v) { fx.tas.speedOn = v; }, t.speed, 0.01f, 5.f, 0.01f, "0.00", [](float v) { fx.tas.speed = v; });
            caption("The whole game runs at this speed, from 0.01x (almost stopped) to 5x. Your own pause menu is not touched.", 2);
            int sel1 = -1, sel2 = -1;
            for (int i = 0; i < 5; i++) if (std::fabs(t.speed - kSpeedVals1[i]) < 0.001f) sel1 = i;
            for (int i = 0; i < 6; i++) if (std::fabs(t.speed - kSpeedVals2[i]) < 0.001f) sel2 = i;
            caption("Presets");
            segments(kSpeedNames1, sel1, [](int i) { fx.tas.speed = kSpeedVals1[i]; fx.tas.speedOn = true; }, 0.9f);
            segments(kSpeedNames2, sel2, [](int i) { fx.tas.speed = kSpeedVals2[i]; fx.tas.speedOn = true; }, 0.9f);
            section("AUDIO");
            toggle("Audio sync", t.audioSync, [](bool v) { fx.tas.audioSync = v; });
            caption("Sounds get higher or lower with the speed, so audio and picture stay together. The pitch stops at 0.05x and 3x.", 2);
            break;
        }
        case 1: {  // ---- item splits
            section("ITEM SPLITS");
            toggle("Show the splits panel", t.splitsShow, [](bool v) { fx.tas.splitsShow = v; });
            toggle("Auto list items", t.autoList, [](bool v) { fx.tas.autoList = v; });
            toggle("Escape split", t.escapeSplit, [](bool v) { fx.tas.escapeSplit = v; });
            caption("The panel lists every item with the time you picked it up. Auto list adds a new item to the list the first time you pick it up. With it off, only the items already in the list get a time. Escape is always the last row.", 4);
            button("Reset splits and start the timer", [] { fx.tas.resetSplitsReq++; });
            button("Clear the list", [] { fx.tas.clearListReq++; });
            section("PANEL");
            slider("Panel scale", t.splitsScale, 0.6f, 2.f, 0.05f, "0.00", [](float v) { fx.tas.splitsScale = v; });
            slider("Panel opacity", t.splitsAlpha, 0.f, 1.f, 0.05f, "0.00", [](float v) { fx.tas.splitsAlpha = v; });
            caption("Drag the panel on the screen to move it. The timer starts again at every scene load.", 2);
            break;
        }
        case 2: {  // ---- utility
            section("RESTART");
            button("Restart", [] { fx.tas.restartReq++; });
            caption("Loads the scene you are in again.", 1);
            section("CUTSCENES");
            toggle("Skip the bed cutscene", t.skipBed, [](bool v) { fx.tas.skipBed = v; });
            toggle("Skip the ending cutscene", t.skipEnding, [](bool v) { fx.tas.skipEnding = v; });
            caption("The cutscene is not cut out: it plays at 8x (bed) and 10x (ending), then the game goes on as usual.", 2);
            break;
        }
        default: {  // ---- crosshair
            section("CROSSHAIR");
            toggle("Crosshair", t.crosshairOn, [](bool v) { fx.tas.crosshairOn = v; });
            caption("Preset");
            segments(kCrossNames, t.crosshairPreset, [](int i) { fx.tas.crosshairPreset = i; fx.tas.crosshairOn = true; }, 0.9f);
            slider("Scale", t.crosshairScale, 0.5f, 3.f, 0.05f, "0.00", [](float v) { fx.tas.crosshairScale = v; });
            slider("Opacity", t.crosshairAlpha, 0.f, 1.f, 0.05f, "0.00", [](float v) { fx.tas.crosshairAlpha = v; });
            break;
        }
    }
}

void resetLook() {
    ui.scale = 1.f;
    gScaleDraft = 1.f;
    ui.panelAlpha = 0.94f;
    ui.iconSize = 1.f;
    ui.iconAlpha = 1.f;
    ui.rainbow = false;
    ui.rainbowSpeed = 0.5f;
    ui.rounded = true;
    ui.lock = false;
    for (int i = 0; i < 3; i++) {
        ui.accent[i] = kAccents[0][i];
        ui.bg[i] = kBackgrounds[0][i];
    }
    ui.eyeX = ui.eyeY = ui.panX = ui.panY = ui.fpsX = ui.fpsY = ui.splitX = ui.splitY = -1.f;
}

std::string gLogText;
bool gLogShown = false;

void pageUi() {
    slider("Menu size", gScaleDraft, 0.6f, 1.8f, 0.05f, "0.00", [](float v) { gScaleDraft = v; }, [] { ui.scale = gScaleDraft; });
    slider("Menu opacity", ui.panelAlpha, 0.f, 1.f, 0.02f, "0.00", [](float v) { ui.panelAlpha = v; });
    slider("Icon size", ui.iconSize, 0.6f, 2.f, 0.05f, "0.00", [](float v) { ui.iconSize = v; });
    slider("Icon opacity", ui.iconAlpha, 0.f, 1.f, 0.05f, "0.00", [](float v) { ui.iconAlpha = v; });
    if (ui.iconAlpha < 0.05f) caption("The icon is invisible now, but it still opens the menu where it was. To get it back, delete visualmobile.cfg.", 3);
    section("COLOURS");
    swatches(kAccents, ui.rainbow ? -1 : indexIn(kAccents, ui.accent[0], ui.accent[1], ui.accent[2]), [](int i) {
        for (int k = 0; k < 3; k++) ui.accent[k] = kAccents[i][k];
        ui.rainbow = false;
    });
    swatches(kBackgrounds, indexIn(kBackgrounds, ui.bg[0], ui.bg[1], ui.bg[2]), [](int i) {
        for (int k = 0; k < 3; k++) ui.bg[k] = kBackgrounds[i][k];
    });
    toggle("Rainbow accent", ui.rainbow, [](bool v) { ui.rainbow = v; });
    slider("Rainbow speed", ui.rainbowSpeed, 0.1f, 3.f, 0.1f, "0.0", [](float v) { ui.rainbowSpeed = v; });
    section("BEHAVIOUR");
    toggle("Rounded corners", ui.rounded, [](bool v) { ui.rounded = v; });
    toggle("Lock icon and menu position", ui.lock, [](bool v) { ui.lock = v; });
    button("Reset menu look", [] { resetLook(); });
    section("LOG");
    button("Show last log lines", [] { gLogText = logTail(22); gLogShown = true; });
    if (gLogShown) caption(gLogText.empty() ? "(the log is empty)" : gLogText, 22);
}

// ================================================================ the panel, the eye, the FPS counter
void drawTab(int i, float x, float y, float w, float h, Col acc) {
    bool sel = i == ui.tab;
    if (sel) rfill(x, y, w, h, 7.f * S, withA(acc, 0.92f));
    lbl(x + 12.f * S, y, w - 14.f * S, h, kTabNames[i], Left, sel ? 1.f : 0.72f);
    Hit& t = newHit(x, y, w, h);
    t.pick = [](int n) { ui.tab = n; scroll = 0.f; };
    t.index = i;
}

void drawPanel() {
    Col acc = accentNow();
    float s = S, x = ui.panX, y = ui.panY;
    Col card = withA(shade(bgCol(), 0.035f), ui.panelAlpha);

    rfill(x, y, pw, ph, 12.f * s, withA(bgCol(), ui.panelAlpha));
    rring(x, y, pw, ph, 12.f * s, 1.5f * s, withA(acc, 0.75f));

    // ---- left: logo and tabs
    rfill(x + 6.f * s, y + 6.f * s, sbW - 4.f * s, ph - 12.f * s, 9.f * s, card);
    eyeGlyph(x + 26.f * s, y + 22.f * s, 24.f * s, acc);
    text(x + 42.f * s, y + 6.f * s, sbW - 46.f * s, 32.f * s, "VISUAL\nMOBILE", fsz(9.5f, 10.5f), White, Left, true);

    float tx = x + 10.f * s, tw = sbW - 12.f * s, th = 32.f * s;
    float ty = y + hdrH + 4.f * s;
    // the tab list: General .. Performance, then Cheat (tab 6). Settings (tab 5) sits at the bottom.
    const int kTopTabs[7] = {0, 1, 2, 3, 4, 6, 7};
    th = 28.f * s;
    for (int i = 0; i < 7; i++) drawTab(kTopTabs[i], tx, ty + i * (th + 3.f * s), tw, th, acc);
    th = 32.f * s;
    float sy = y + ph - 12.f * s - th;
    fill(tx + 4.f * s, sy - 8.f * s, tw - 8.f * s, 1.f * s, Col(1.f, 1.f, 1.f, 0.10f));
    drawTab(5, tx, sy, tw, th, acc);

    // ---- right: header, page, footer
    rfill(kx, ky, kw, kBot - ky, 9.f * s, card);
    text(kx + 4.f * s, y, kw - 60.f * s, hdrH, kPageTitles[ui.tab], fsz(10.5f), Col(0.82f, 0.86f, 0.95f, 1.f), Left, true);
    float cb = 26.f * s;
    float cbx = x + pw - cb - 10.f * s;
    rfill(cbx, y + (hdrH - cb) * 0.5f, cb, cb, 7.f * s, cardCol());
    lbl(cbx, y + (hdrH - cb) * 0.5f, cb, cb, "x", Center);
    Hit& close = newHit(cbx - 6.f * s, y, cb + 16.f * s, hdrH);
    close.tap = [] { ui.open = false; };

    float fy = kBot + 2.f * s;
    if (gTime < gToastUntil) small(kx + 8.f * s, fy, kw - 60.f * s, ftH, gToast, Left);
    else small(kx + 8.f * s, fy, kw - 60.f * s, ftH, "Adjust your game's visuals to your liking.", Left);
    small(kx + kw - 60.f * s, fy, 52.f * s, ftH, std::string("v") + kVersion, Right);

    // ---- rows
    D->PushClipRect(ImVec2(kx, cTop - 1.f), ImVec2(kx + kw, cBot + 1.f), true);
    cy = cTop - scroll;
    switch (ui.tab) {
        case 0: pageGeneral(); break;
        case 1: pageColor(); break;
        case 2: pageEffects(); break;
        case 3: pageAdvanced(); break;
        case 4: pagePerf(); break;
        case 6: pageCheat(); break;
        case 7: pageTas(); break;
        default: pageUi(); break;
    }
    contentH = (cy + scroll) - cTop;
    D->PopClipRect();

    // scroll indicator
    float viewH = cBot - cTop;
    if (contentH > viewH + 1.f) {
        float barH = std::max(18.f * s, viewH * viewH / contentH);
        float barY = cTop + (viewH - barH) * (scroll / std::max(1.f, contentH - viewH));
        rfill(kx + kw - 7.f * s, barY, 3.f * s, barH, 1.5f * s, withA(acc, 0.7f));
    }
}

void drawEye() {
    float a = ui.iconAlpha;
    Col acc = accentNow();
    float r = ES * 0.5f;
    float cxp = ui.eyeX + r, cyp = ui.eyeY + r;
    disc(cxp, cyp, r, withA(bgCol(), 0.88f * a));
    ring(cxp, cyp, r - S, 2.f * S, withA(acc, a));
    eyeGlyph(cxp, cyp, ES * 0.74f, withA(acc, a));
}

// the last frame times as thin bars: green up to 18 ms, yellow up to 34 ms, red above
void drawGraph(float x, float y, float w, float h) {
    float s = S;
    rfill(x, y, w, h, 8.f * s, withA(bgCol(), 0.82f));
    rring(x, y, w, h, 8.f * s, 1.f * s, withA(accentNow(), 0.7f));
    float px = x + 6.f * s, pw2 = w - 12.f * s, top = y + 14.f * s, ph2 = h - 14.f * s - 4.f * s;
    const float maxMs = 50.f;
    // guide lines at 16.7 ms (60 FPS) and 33.3 ms (30 FPS)
    fill(px, top + ph2 * (1.f - 16.7f / maxMs), pw2, 1.f, Col(1.f, 1.f, 1.f, 0.12f));
    fill(px, top + ph2 * (1.f - 33.3f / maxMs), pw2, 1.f, Col(1.f, 1.f, 1.f, 0.12f));
    float bw = pw2 / kGraphN;
    float sum = 0.f, worst = 0.f;
    for (int i = 0; i < kGraphN; i++) {
        float ms = gFrameMs[(gFramePos + i) % kGraphN];
        sum += ms;
        worst = std::max(worst, ms);
        if (ms <= 0.f) continue;
        float bh = ph2 * clampf(ms / maxMs, 0.02f, 1.f);
        Col c = ms <= 18.f ? Col(0.35f, 0.84f, 0.48f, 1.f) : (ms <= 34.f ? Col(0.95f, 0.75f, 0.20f, 1.f) : Col(1.f, 0.36f, 0.36f, 1.f));
        fill(px + i * bw, top + ph2 - bh, std::max(1.f, bw - 0.5f), bh, c);
    }
    char t[48];
    snprintf(t, sizeof(t), "%.1f ms   worst %.0f", sum / kGraphN, worst);
    text(x + 6.f * s, y + 1.f * s, w - 12.f * s, 14.f * s, t, fsz(8.5f, 10.5f), Col(0.70f, 0.74f, 0.82f, 1.f), Left);
}

void drawFps() {
    float s = S;
    float y = ui.fpsY;
    if (ui.showFps) {
        int n = (int)std::lround(gFps);
        Col col = n >= 50 ? Col(0.35f, 0.84f, 0.48f, 1.f) : (n >= 30 ? Col(0.95f, 0.75f, 0.20f, 1.f) : Col(1.f, 0.36f, 0.36f, 1.f));
        rfill(ui.fpsX, y, fpsW, fpsH, fpsH * 0.5f, withA(bgCol(), 0.82f));
        rring(ui.fpsX, y, fpsW, fpsH, fpsH * 0.5f, 1.f * s, withA(accentNow(), 0.7f));
        float size = fsz(11.5f);
        std::string num = std::to_string(n), rest = " FPS";
        float w1 = gFont->CalcTextSizeA(size, FLT_MAX, 0, num.c_str()).x, w2 = gFont->CalcTextSizeA(size, FLT_MAX, 0, rest.c_str()).x;
        float x0 = ui.fpsX + (fpsW - (w1 + w2)) * 0.5f;
        float th = gFont->CalcTextSizeA(size, FLT_MAX, 0, num.c_str()).y;
        float y0 = y + (fpsH - th) * 0.5f;
        D->AddText(gFont, size, ImVec2(x0, y0), U(col), num.c_str());
        D->AddText(gFont, size, ImVec2(x0 + size * 0.04f, y0), U(col), num.c_str());
        D->AddText(gFont, size, ImVec2(x0 + w1, y0), U(White), rest.c_str());
        y += fpsH + 4.f * s;
    }
    if (ui.showGraph) drawGraph(ui.fpsX, y, graphW, graphH);
}

// ================================================================ the split panel and the crosshair (always on screen)
std::string fmtTime(double t) {
    if (t < 0.0) return "--:--.--";
    int m = (int)(t / 60.0);
    double sec = t - m * 60.0;
    char b[32];
    snprintf(b, sizeof(b), "%02d:%05.2f", m, sec);
    return b;
}

// a plain panel: dark box, one row per item, the time on the right
void drawSplits() {
    if (!fx.tas.splitsShow) return;
    float sc = S * fx.tas.splitsScale;
    float a = clampf(fx.tas.splitsAlpha, 0.f, 1.f);
    float x = ui.splitX, y = ui.splitY;
    rfill(x, y, splitBoxW, splitBoxH, 4.f * sc, Col(0.f, 0.f, 0.f, 0.72f * a));
    float tx = x + 6.f * sc, rx = x + splitBoxW - 6.f * sc;
    float ty = y + 5.f * sc;
    auto row = [&](const std::string& name, const std::string& time, Col nc, Col tc) {
        D->AddText(gFont, splitFont, ImVec2(tx, ty), U(withA(nc, a)), name.c_str());
        float w = gFont->CalcTextSizeA(splitFont, FLT_MAX, 0, time.c_str()).x;
        D->AddText(gFont, splitFont, ImVec2(rx - w, ty), U(withA(tc, a)), time.c_str());
        ty += splitLine;
    };
    row("Time", fmtTime(gSv.timer), Col(0.75f, 0.78f, 0.85f, 1.f), gSv.running ? White : Col(0.35f, 0.84f, 0.48f, 1.f));
    for (const SplitRow& r : gSv.rows) {
        bool done = r.seconds >= 0.0;
        row(r.name, fmtTime(r.seconds), done ? White : Col(0.55f, 0.58f, 0.65f, 1.f), done ? Col(0.35f, 0.84f, 0.48f, 1.f) : Col(0.55f, 0.58f, 0.65f, 1.f));
    }
}

// six crosshairs, drawn with a dark edge so they show on any background
void drawCrosshair() {
    if (!fx.tas.crosshairOn) return;
    float a = clampf(fx.tas.crosshairAlpha, 0.f, 1.f);
    if (a <= 0.f) return;
    float sc = S * fx.tas.crosshairScale;
    float cx = std::round(W * 0.5f), cy2 = std::round(H * 0.5f);
    ImU32 light = U(Col(1.f, 1.f, 1.f, a)), dark = U(Col(0.f, 0.f, 0.f, 0.75f * a));
    float t = std::max(1.5f, 1.6f * sc), edge = std::max(1.f, 1.2f * sc);
    auto line = [&](float x1, float y1, float x2, float y2) {
        D->AddLine(ImVec2(cx + x1, cy2 + y1), ImVec2(cx + x2, cy2 + y2), dark, t + 2.f * edge);
        D->AddLine(ImVec2(cx + x1, cy2 + y1), ImVec2(cx + x2, cy2 + y2), light, t);
    };
    auto dot = [&](float r) {
        D->AddCircleFilled(ImVec2(cx, cy2), r + edge, dark, 16);
        D->AddCircleFilled(ImVec2(cx, cy2), r, light, 16);
    };
    auto ring = [&](float r) {
        D->AddCircle(ImVec2(cx, cy2), r, dark, 32, t + 2.f * edge);
        D->AddCircle(ImVec2(cx, cy2), r, light, 32, t);
    };
    float L = 9.f * sc, g = 4.f * sc;
    switch (clampi(fx.tas.crosshairPreset, 0, 5)) {
        case 0: dot(2.2f * sc); break;                                   // dot
        case 1: line(-L, 0, L, 0); line(0, -L, 0, L); break;             // plus
        case 2:                                                          // plus with a gap
            line(-L - g, 0, -g, 0); line(g, 0, L + g, 0);
            line(0, -L - g, 0, -g); line(0, g, 0, L + g);
            break;
        case 3: ring(8.f * sc); break;                                   // circle
        case 4: ring(9.f * sc); dot(1.8f * sc); break;                   // circle with a dot
        default: line(-L, -L, L, L); line(-L, L, L, -L); break;          // X
    }
}

// ================================================================ the finger
enum Mode { ModeNone, ModeTap, ModeSlider, ModeEye, ModePanel, ModeScroll, ModeFps, ModeSplits };
Mode gMode = ModeNone;
bool gPtrDown = false, gMoved = false;
float gDownX = 0, gDownY = 0, gLastX = 0, gLastY = 0;
Hit gTapHit, gActiveHit;
bool gHasTap = false, gHasActive = false;

const Hit* findHit(float x, float y) {
    for (int i = (int)hits.size() - 1; i >= 0; i--)
        if (inside(x, y, hits[i].x, hits[i].y, hits[i].w, hits[i].h)) return &hits[i];
    return nullptr;
}

void slideTo(const Hit& h, float x) {
    float f = clampf((x - h.tx) / std::max(1.f, h.tw), 0.f, 1.f);
    float v = h.mn + f * (h.mx - h.mn);
    if (h.step > 0.f) v = std::round(v / h.step) * h.step;
    v = clampf(v, h.mn, h.mx);
    gPresetSel = -1;
    if (h.set) h.set(v);
    if (h.enable) h.enable(true);
    changed();
    saveSoon();
}

void runTap(const Hit& t) {
    if (t.tap) t.tap();
    else if (t.setB) {
        t.setB(!t.val);
        gPresetSel = -1;
    } else if (t.pick) t.pick(t.index);
    changed();
    saveSoon();
}

void pointerDown(float x, float y) {
    gDownX = gLastX = x;
    gDownY = gLastY = y;
    gMoved = false;
    gMode = ModeNone;
    gHasTap = gHasActive = false;

    if (ui.open && inside(x, y, ui.panX, ui.panY, pw, ph)) {
        const Hit* h = findHit(x, y);
        if (y < ui.panY + hdrH) {
            if (h) {
                gMode = ModeTap;
                gTapHit = *h;
                gHasTap = true;
            } else {
                gMode = ModePanel;
            }
            return;
        }
        if (h && h->slider) {
            gActiveHit = *h;
            gHasActive = true;
            gMode = ModeSlider;
            slideTo(gActiveHit, x);
            return;
        }
        if (h) {
            gTapHit = *h;
            gHasTap = true;
        }
        gMode = ModeTap;  // becomes a scroll if the finger moves
        return;
    }
    if (inside(x, y, ui.eyeX, ui.eyeY, ES, ES)) gMode = ModeEye;
    else if ((ui.showFps || ui.showGraph) && inside(x, y, ui.fpsX, ui.fpsY, fpsBoxW, fpsBoxH)) gMode = ModeFps;
    else if (fx.tas.splitsShow && inside(x, y, ui.splitX, ui.splitY, splitBoxW, splitBoxH)) gMode = ModeSplits;
}

void pointerMove(float x, float y) {
    float dx = x - gLastX, dy = y - gLastY;
    gLastX = x;
    gLastY = y;
    if (!gMoved && (std::fabs(x - gDownX) > 10.f * S || std::fabs(y - gDownY) > 10.f * S)) gMoved = true;
    switch (gMode) {
        case ModeSlider:
            if (gHasActive) slideTo(gActiveHit, x);
            break;
        case ModeEye:
            if (gMoved && !ui.lock) {
                ui.eyeX += dx;
                ui.eyeY += dy;
            }
            break;
        case ModePanel:
            if (!ui.lock) {
                ui.panX += dx;
                ui.panY += dy;
            }
            break;
        case ModeFps:
            if (gMoved && !ui.lock) {
                ui.fpsX += dx;
                ui.fpsY += dy;
            }
            break;
        case ModeSplits:
            if (gMoved && !ui.lock) {
                ui.splitX += dx;
                ui.splitY += dy;
            }
            break;
        case ModeTap:
            if (gMoved && gDownY >= cTop && gDownX >= kx) gMode = ModeScroll;
            break;
        case ModeScroll:
            scroll -= dy;
            break;
        default: break;
    }
}

void pointerUp(float x, float y) {
    switch (gMode) {
        case ModeEye:
            if (!gMoved) {
                ui.open = !ui.open;
                scroll = 0.f;
            }
            saveSoon();
            break;
        case ModeTap:
            if (gHasTap && inside(x, y, gTapHit.x, gTapHit.y, gTapHit.w, gTapHit.h)) runTap(gTapHit);
            break;
        case ModeSlider:
            if (gHasActive && gActiveHit.release) gActiveHit.release();
            saveSoon();
            break;
        case ModePanel:
        case ModeFps:
        case ModeSplits:
            saveSoon();
            break;
        default: break;
    }
    gMode = ModeNone;
    gHasTap = gHasActive = false;
}

void handleFinger(int h) {
    Touch t = getTouch();
    bool down = t.valid && t.down;
    float x = t.x, y = (float)h - t.y;  // the menu counts from the top
    if (down && !gPtrDown) {
        gPtrDown = true;
        pointerDown(x, y);
    } else if (down && gPtrDown) {
        pointerMove(x, y);
    } else if (!down && gPtrDown) {
        gPtrDown = false;
        if (t.valid) {
            gLastX = x;
            gLastY = y;
        }
        pointerUp(gLastX, gLastY);
    }
    float viewH = cBot - cTop;
    scroll = clampf(scroll, 0.f, std::max(0.f, contentH - viewH));
    gBlockTouch.store(gPtrDown && gMode != ModeNone);
}

// ================================================================ setup
void initImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    // The menu font: the Roboto the build compiled in (CMake) -> a clean system font -> ImGui's built-in pixel font.
    {
        ImFontConfig fc;
        fc.OversampleH = 2;
        fc.OversampleV = 2;
#ifdef HAVE_UI_FONT
        fc.FontDataOwnedByAtlas = false;  // it lives in the library, ImGui must not free it
        gFont = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kUiFont), (int)kUiFontSize, 40.f, &fc);
        if (gFont) logf("menu: font Roboto (built in)");
#endif
        static const char* const kFonts[] = {
            "/system/fonts/Roboto-Regular.ttf", "/system/fonts/RobotoStatic-Regular.ttf", "/system/fonts/NotoSans-Regular.ttf",
            "/system/fonts/DroidSans.ttf",
        };
        for (size_t i = 0; !gFont && i < sizeof(kFonts) / sizeof(kFonts[0]); i++) {
            FILE* f = fopen(kFonts[i], "rb");
            if (!f) continue;
            fclose(f);
            gFont = io.Fonts->AddFontFromFileTTF(kFonts[i], 40.f, &fc);
            if (gFont) logf("menu: font %s", kFonts[i]);
        }
    }
    if (!gFont) {
        logf("menu: no usable font, using the built-in one");
        ImFontConfig fc;
        fc.SizePixels = 32.f;
        gFont = io.Fonts->AddFontDefault(&fc);
    }

    if (!ImGui_ImplOpenGL3_Init("#version 300 es")) {
        logf("menu: ImGui OpenGL backend failed to start");
        gFailed = true;
        return;
    }
    settingsLoad(ui, fx);
    gScaleDraft = ui.scale;
    hits.reserve(160);
    gLast = std::chrono::steady_clock::now();
    gInit = true;
    logf("menu: ImGui ready");
}

}  // namespace

void menuFrame(int w, int h) {
    if (gFailed || w <= 0 || h <= 0) return;
    if (!gInit) {
        initImGui();
        if (!gInit) return;
    }

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - gLast).count();
    gLast = now;
    if (dt <= 0.f || dt > 0.5f) dt = 1.f / 60.f;
    io.DeltaTime = dt;
    gTime += dt;
    gFps = gFps <= 0.f ? 1.f / dt : lerpf(gFps, 1.f / dt, 0.1f);
    setMeasuredFps(gFps);
    gFrameMs[gFramePos] = dt * 1000.f;
    gFramePos = (gFramePos + 1) % kGraphN;

    W = (float)w;
    H = (float)h;
    st = currentStatus();
    gSv = getSplitsView();
    if (gToastSeq < 0) gToastSeq = st.toastSeq;
    if (st.toastSeq != gToastSeq) {
        gToastSeq = st.toastSeq;
        gToast = st.toast;
        gToastUntil = gTime + 3.f;
    }
    if (gMode != ModeSlider) gScaleDraft = ui.scale;

    layout();
    handleFinger(h);
    layout();  // the finger may have moved things

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    D = ImGui::GetForegroundDrawList();
    hits.clear();
    drawCrosshair();
    drawFps();
    drawSplits();
    drawEye();
    if (ui.open) drawPanel();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    if (gDirty) {
        gDirty = false;
        publishFx(fx);
    }
    if (gNeedSave && gTime >= gSaveAt) {
        gNeedSave = false;
        settingsSave(ui, fx);
    }
}

}  // namespace vm
