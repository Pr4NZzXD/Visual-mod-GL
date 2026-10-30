#include "fog.h"

#include <chrono>
#include <cmath>

#include "il2cpp.h"
#include "log.h"

// Fog control, the same three modes as the C# mod:
//   0 Game default - the mod does not touch the fog
//   1 No fog       - RenderSettings.fog is switched off
//   2 Custom       - fog on, with your own colour and thickness (exponential-squared)
// The game strips most of the managed RenderSettings properties, so the engine functions behind them are called
// directly (il2::icall). While a mode is active the values are written again every frame, because a scene load may
// reset them; the original values are saved first and put back when the mode returns to "Game default".

namespace vm {
namespace fog {
namespace {

struct Fns {
    bool tried = false, ok = false;
    bool (*getFog)() = nullptr;
    void (*setFog)(bool) = nullptr;
    int (*getMode)() = nullptr;
    void (*setMode)(int) = nullptr;
    float (*getDens)() = nullptr;
    void (*setDens)(float) = nullptr;
    float (*getStart)() = nullptr;
    void (*setStart)(float) = nullptr;
    float (*getEnd)() = nullptr;
    void (*setEnd)(float) = nullptr;
    void (*getColor)(float*) = nullptr;     // get_fogColor_Injected(out Color)
    void (*setColor)(const float*) = nullptr;  // set_fogColor_Injected(ref Color)
} F;

struct State {
    bool fog = false;
    int mode = 0;
    float color[4] = {0, 0, 0, 1};
    float density = 0.f, start = 0.f, end = 0.f;
};

bool gSaved = false;
State gOrig, gWrote;
int64_t gLastMs = 0;

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool resolve() {
    if (F.tried) return F.ok;
    F.tried = true;
    auto get = [](const char* n) { return il2::icall((std::string("UnityEngine.RenderSettings::") + n).c_str()); };
    F.getFog = reinterpret_cast<bool (*)()>(get("get_fog"));
    F.setFog = reinterpret_cast<void (*)(bool)>(get("set_fog"));
    F.getMode = reinterpret_cast<int (*)()>(get("get_fogMode"));
    F.setMode = reinterpret_cast<void (*)(int)>(get("set_fogMode"));
    F.getDens = reinterpret_cast<float (*)()>(get("get_fogDensity"));
    F.setDens = reinterpret_cast<void (*)(float)>(get("set_fogDensity"));
    F.getStart = reinterpret_cast<float (*)()>(get("get_fogStartDistance"));
    F.setStart = reinterpret_cast<void (*)(float)>(get("set_fogStartDistance"));
    F.getEnd = reinterpret_cast<float (*)()>(get("get_fogEndDistance"));
    F.setEnd = reinterpret_cast<void (*)(float)>(get("set_fogEndDistance"));
    F.getColor = reinterpret_cast<void (*)(float*)>(get("get_fogColor_Injected"));
    F.setColor = reinterpret_cast<void (*)(const float*)>(get("set_fogColor_Injected"));
    F.ok = F.getFog && F.setFog && F.getMode && F.setMode && F.getDens && F.setDens && F.getStart && F.setStart && F.getEnd &&
           F.setEnd && F.getColor && F.setColor;
    logf(F.ok ? "fog: ready" : "fog: some engine functions are missing, fog control is not available");
    return F.ok;
}

State read() {
    State s;
    s.fog = F.getFog();
    s.mode = F.getMode();
    F.getColor(s.color);
    s.density = F.getDens();
    s.start = F.getStart();
    s.end = F.getEnd();
    return s;
}

void write(const State& s) {
    F.setFog(s.fog);
    F.setMode(s.mode);
    F.setColor(s.color);
    F.setDens(s.density);
    F.setStart(s.start);
    F.setEnd(s.end);
}

bool near(const float* a, const float* b) {
    return std::fabs(a[0] - b[0]) < 0.003f && std::fabs(a[1] - b[1]) < 0.003f && std::fabs(a[2] - b[2]) < 0.003f;
}

}  // namespace

void tick(const FxSettings& s) {
    if (s.fogMode == 0 && !gSaved) return;
    int64_t now = nowMs();
    if (now - gLastMs < 16) return;
    gLastMs = now;
    if (!resolve()) return;

    if (s.fogMode == 0) {  // back to the game's own fog
        write(gOrig);
        gSaved = false;
        return;
    }

    State cur = read();
    // a scene load reset the fog: what is there now are the new originals
    bool changedByGame = cur.fog != gWrote.fog || cur.mode != gWrote.mode || !near(cur.color, gWrote.color) ||
                         std::fabs(cur.density - gWrote.density) > 0.0005f;
    if (!gSaved || changedByGame) {
        gOrig = cur;
        gSaved = true;
    }

    State w = cur;
    if (s.fogMode == 2) {
        w.fog = true;
        w.mode = 3;  // ExponentialSquared
        w.color[0] = s.fogR;
        w.color[1] = s.fogG;
        w.color[2] = s.fogB;
        w.color[3] = 1.f;
        // 0..100 -> 0..0.2 (exponential-squared density): 30 % is a clear haze, 100 % is a few metres of view
        w.density = std::fmin(100.f, std::fmax(0.f, s.fogThick)) * 0.002f;
    } else {
        w.fog = false;
    }
    write(w);
    gWrote = read();  // what the engine really stored, so rounding never looks like "the game changed it"
}

// nothing to do: a scene that resets the fog is noticed in tick() (the values differ from what was written there)
void onSceneLoaded() {}

}  // namespace fog
}  // namespace vm
