#include "scene.h"

#include <chrono>
#include <cmath>
#include <unordered_map>

#include "il2cpp.h"
#include "log.h"

// Dark scenes, three independent options. Each one remembers what the game had and puts it back when it is
// switched off. The game strips most of the managed setters, so the engine functions behind them are used where
// no wrapper is left (il2::icall); the getters that still exist are called the normal way.
//   Brightness   - the ambient light is made flat grey, so shadowed corners get some light
//   Light boost  - every light's intensity (and a bit of its range) is multiplied
//   Field of view- the camera's FOV

namespace vm {
namespace scene {
namespace {

using namespace il2;

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline void keepAlive(void* p) { asm volatile("" : : "r"(p) : "memory"); }

struct Held {
    Il2CppObject* o = nullptr;
    uint32_t h = 0;
};
Held hold(Il2CppObject* o) {
    Held x;
    x.o = o;
    x.h = retain(o);
    return x;
}
void drop(Held& x) {
    release(x.h);
    x = Held();
}

struct Fns {
    bool tried = false;
    Il2CppClass *cLight = nullptr, *cCamera = nullptr;
    const MethodInfo *mLightGetI = nullptr, *mLightGetR = nullptr, *mCamGetFov = nullptr, *mCamSetFov = nullptr,
                     *mCamGetOrtho = nullptr, *mCamGetTarget = nullptr;
    int (*getAmbMode)() = nullptr;
    void (*setAmbMode)(int) = nullptr;
    void (*getAmbColor)(float*) = nullptr;
    void (*setAmbColor)(const float*) = nullptr;
    void (*lightSetI)(void*, float) = nullptr;
    void (*lightSetR)(void*, float) = nullptr;
    bool ambOk = false, lightOk = false, fovOk = false;
} F;

void resolve() {
    if (F.tried) return;
    F.tried = true;
    F.cLight = findClass("UnityEngine", "Light");
    F.cCamera = findClass("UnityEngine", "Camera");
    F.mLightGetI = findMethod(F.cLight, "get_intensity", 0);
    F.mLightGetR = findMethod(F.cLight, "get_range", 0);
    F.mCamGetFov = findMethod(F.cCamera, "get_fieldOfView", 0);
    F.mCamSetFov = findMethod(F.cCamera, "set_fieldOfView", 1);
    F.mCamGetOrtho = findMethod(F.cCamera, "get_orthographic", 0);
    F.mCamGetTarget = findMethod(F.cCamera, "get_targetTexture", 0);
    F.getAmbMode = reinterpret_cast<int (*)()>(icall("UnityEngine.RenderSettings::get_ambientMode"));
    F.setAmbMode = reinterpret_cast<void (*)(int)>(icall("UnityEngine.RenderSettings::set_ambientMode"));
    F.getAmbColor = reinterpret_cast<void (*)(float*)>(icall("UnityEngine.RenderSettings::get_ambientLight_Injected"));
    F.setAmbColor = reinterpret_cast<void (*)(const float*)>(icall("UnityEngine.RenderSettings::set_ambientLight_Injected"));
    F.lightSetI = reinterpret_cast<void (*)(void*, float)>(icall("UnityEngine.Light::set_intensity"));
    F.lightSetR = reinterpret_cast<void (*)(void*, float)>(icall("UnityEngine.Light::set_range"));
    F.ambOk = F.getAmbMode && F.setAmbMode && F.getAmbColor && F.setAmbColor;
    F.lightOk = F.cLight && F.mLightGetI && F.mLightGetR && F.lightSetI && F.lightSetR;
    F.fovOk = F.cCamera && F.mCamGetFov && F.mCamSetFov;
    logf("scene: brightness %s, light boost %s, field of view %s", F.ambOk ? "ready" : "not available",
         F.lightOk ? "ready" : "not available", F.fovOk ? "ready" : "not available");
}

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---------------------------------------------------------------- brightness (ambient light)
struct Amb {
    bool saved = false;
    int mode = 0;
    float color[4] = {0, 0, 0, 1};
    int wroteMode = -1;
    float wroteColor[4] = {0, 0, 0, 1};
    int64_t next = 0;
} Ab;

bool nearC(const float* a, const float* b) {
    return std::fabs(a[0] - b[0]) < 0.003f && std::fabs(a[1] - b[1]) < 0.003f && std::fabs(a[2] - b[2]) < 0.003f;
}

void ambientTick(const FxSettings& s, int64_t now) {
    if (!F.ambOk) return;
    if (!s.ambientOn) {
        if (Ab.saved) {
            F.setAmbMode(Ab.mode);
            F.setAmbColor(Ab.color);
            Ab.saved = false;
            logf("scene: ambient light restored");
        }
        return;
    }
    if (now < Ab.next) return;
    Ab.next = now + 50;  // written again often: a scene load resets it

    int mode = F.getAmbMode();
    float color[4];
    F.getAmbColor(color);
    bool changedByGame = mode != Ab.wroteMode || !nearC(color, Ab.wroteColor);
    if (!Ab.saved || changedByGame) {
        Ab.mode = mode;
        for (int i = 0; i < 4; i++) Ab.color[i] = color[i];
        Ab.saved = true;
    }
    // 0..100 % -> a flat grey of 0..0.8 (1.0 would wash the whole scene out)
    float g = clampf(s.ambient, 0.f, 100.f) * 0.01f * 0.8f;
    float want[4] = {g, g, g, 1.f};
    F.setAmbMode(3);  // Flat
    F.setAmbColor(want);
    Ab.wroteMode = F.getAmbMode();
    F.getAmbColor(Ab.wroteColor);
}

// ---------------------------------------------------------------- light boost
struct LightEntry {
    Held h;
    float origI = 0, origR = 0, lastI = 0, lastR = 0;
};
std::unordered_map<Il2CppObject*, LightEntry> gLights;
int64_t gLightScan = 0, gLightNext = 0;
float gLastMul = 0;

float readI(Il2CppObject* l) { return callFloat(F.mLightGetI, l, nullptr, 0.f); }
float readR(Il2CppObject* l) { return callFloat(F.mLightGetR, l, nullptr, 0.f); }

void lightTick(const FxSettings& s, int64_t now) {
    if (!F.lightOk) return;
    if (!s.lightBoostOn) {
        for (auto& kv : gLights) {
            if (alive(kv.first)) {
                F.lightSetI(kv.first, kv.second.origI);
                F.lightSetR(kv.first, kv.second.origR);
            }
            drop(kv.second.h);
        }
        gLights.clear();
        gLastMul = 0;
        return;
    }
    if (now >= gLightScan) {  // new lights are looked for twice a second
        gLightScan = now + 500;
        Il2CppObject* arr = findObjectsOfType(F.cLight);
        if (arr) {
            size_t n = A.array_length(arr);
            Il2CppObject** el = arrayData(arr);
            for (size_t i = 0; i < n && i < 256; i++) {
                Il2CppObject* l = el[i];
                if (!l || !alive(l) || gLights.count(l)) continue;
                LightEntry e;
                e.h = hold(l);
                e.origI = e.lastI = readI(l);
                e.origR = e.lastR = readR(l);
                gLights[l] = e;
            }
            keepAlive(arr);
        }
    }
    if (now < gLightNext) return;
    gLightNext = now + 33;

    float m = clampf(s.lightBoost, 1.f, 3.f);
    float rm = 1.f + (m - 1.f) * 0.5f;  // the reach grows less than the brightness
    for (auto it = gLights.begin(); it != gLights.end();) {
        Il2CppObject* l = it->first;
        LightEntry& e = it->second;
        if (!alive(l)) {
            drop(e.h);
            it = gLights.erase(it);
            continue;
        }
        float ci = readI(l), cr = readR(l);
        // the game changed it (a flickering flashlight, a scene effect): that is the new original
        if (std::fabs(ci - e.lastI) > 0.0005f) e.origI = ci;
        if (std::fabs(cr - e.lastR) > 0.0005f) e.origR = cr;
        float ni = e.origI * m, nr = e.origR * rm;
        if (std::fabs(ci - ni) > 0.0005f) {
            F.lightSetI(l, ni);
            e.lastI = readI(l);
        } else {
            e.lastI = ci;
        }
        if (std::fabs(cr - nr) > 0.0005f) {
            F.lightSetR(l, nr);
            e.lastR = readR(l);
        } else {
            e.lastR = cr;
        }
        ++it;
    }
    if (gLastMul != m) {
        gLastMul = m;
        logf("scene: %d lights x%.2f", (int)gLights.size(), m);
    }
}

// ---------------------------------------------------------------- field of view
struct CamEntry {
    Held h;
    float orig = 0;
};
std::unordered_map<Il2CppObject*, CamEntry> gCams;
int64_t gCamScan = 0, gCamNext = 0;

float readFov(Il2CppObject* c) { return callFloat(F.mCamGetFov, c, nullptr, 0.f); }
void writeFov(Il2CppObject* c, float v) {
    void* a[1] = {&v};
    invoke(F.mCamSetFov, c, a);
}

void fovTick(const FxSettings& s, int64_t now) {
    if (!F.fovOk) return;
    if (!s.fovOn) {
        for (auto& kv : gCams) {
            if (alive(kv.first)) writeFov(kv.first, kv.second.orig);
            drop(kv.second.h);
        }
        gCams.clear();
        return;
    }
    if (now >= gCamScan) {
        gCamScan = now + 1000;
        Il2CppObject* arr = findObjectsOfType(F.cCamera);
        if (arr) {
            size_t n = A.array_length(arr);
            Il2CppObject** el = arrayData(arr);
            for (size_t i = 0; i < n && i < 64; i++) {
                Il2CppObject* c = el[i];
                if (!c || !alive(c) || gCams.count(c)) continue;
                if (callBool(F.mCamGetOrtho, c, nullptr, false)) continue;
                if (F.mCamGetTarget && invoke(F.mCamGetTarget, c, nullptr)) continue;  // only what is drawn on the screen
                CamEntry e;
                e.h = hold(c);
                e.orig = readFov(c);
                gCams[c] = e;
            }
            keepAlive(arr);
        }
    }
    if (now < gCamNext) return;
    gCamNext = now + 16;
    float want = clampf(s.fov, 40.f, 110.f);
    for (auto it = gCams.begin(); it != gCams.end();) {
        if (!alive(it->first)) {
            drop(it->second.h);
            it = gCams.erase(it);
            continue;
        }
        if (std::fabs(readFov(it->first) - want) > 0.05f) writeFov(it->first, want);
        ++it;
    }
}

}  // namespace

void tick(const FxSettings& s) {
    if (!s.ambientOn && !s.lightBoostOn && !s.fovOn && !Ab.saved && gLights.empty() && gCams.empty()) return;
    resolve();
    int64_t now = nowMs();
    ambientTick(s, now);
    lightTick(s, now);
    fovTick(s, now);
}

void onSceneLoaded() {
    // the lights and cameras of the old scene are gone; the new ones are found within a second
    for (auto it = gLights.begin(); it != gLights.end();) {
        if (!alive(it->first)) {
            drop(it->second.h);
            it = gLights.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = gCams.begin(); it != gCams.end();) {
        if (!alive(it->first)) {
            drop(it->second.h);
            it = gCams.erase(it);
        } else {
            ++it;
        }
    }
    gLightScan = 0;
    gCamScan = 0;
}

}  // namespace scene
}  // namespace vm
