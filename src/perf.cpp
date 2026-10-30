#include "perf.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "fog.h"
#include "il2cpp.h"
#include "log.h"
#include "scene.h"

// The performance options, ported from the C# mod. Everything is reversible: the original value of every setting,
// material and component that is changed is remembered and put back when the option is switched off.
// The game strips many managed Unity methods, so those settings are written through the engine functions behind
// them (il2::icall). Scans (FindObjectsOfType) run on slow timers and the renderer work is spread over many frames.

namespace vm {
namespace perf {
namespace {

using namespace il2;

const float kResSteps[] = {1.f, 0.75f, 0.6f, 0.5f, 0.35f};
const int kFpsSteps[] = {0, 30, 45, 60, 90};
constexpr int kMaxPerFrame = 400;
constexpr double kBudgetMs = 5.0;

double nowS() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

inline void keepAlive(void* p) { asm volatile("" : : "r"(p) : "memory"); }

bool containsI(const std::string& hay, const char* needle) {
    size_t n = strlen(needle);
    if (hay.size() < n) return false;
    for (size_t i = 0; i + n <= hay.size(); i++) {
        size_t k = 0;
        while (k < n && std::tolower((unsigned char)hay[i + k]) == std::tolower((unsigned char)needle[k])) k++;
        if (k == n) return true;
    }
    return false;
}

// an object that is kept alive by a GC handle while we remember it
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

// ---------------------------------------------------------------- what is needed from Unity
template <class T>
struct IProp {
    T (*get)() = nullptr;
    void (*set)(T) = nullptr;
    bool ok() const { return get && set; }
};

struct U {
    bool tried = false, ok = false;
    Il2CppClass *cCamera = nullptr, *cRenderer = nullptr, *cLight = nullptr, *cBehaviour = nullptr;
    const MethodInfo *mRenGetEnabled = nullptr, *mRenSetEnabled = nullptr, *mGetSharedMats = nullptr, *mGetLm = nullptr,
                     *mSetLm = nullptr, *mGetGo = nullptr, *mGetName = nullptr, *mGetHide = nullptr, *mSetHide = nullptr,
                     *mHasProp = nullptr, *mGetTex = nullptr, *mSetTex = nullptr, *mCamGetAspect = nullptr,
                     *mCamSetAspect = nullptr, *mCamGetOrtho = nullptr, *mCamGetTarget = nullptr, *mCamGetFar = nullptr,
                     *mCamSetFar = nullptr, *mBehGetEnabled = nullptr, *mBehSetEnabled = nullptr,
                     *mGetComponents = nullptr, *mLightGetShadows = nullptr, *mSetTargetFps = nullptr,
                     *mGetVsync = nullptr, *mSetVsync = nullptr, *mScreenW = nullptr, *mScreenH = nullptr,
                     *mScreenFull = nullptr, *mActiveScene = nullptr, *mSceneHandle = nullptr;
    // engine functions behind stripped wrappers
    IProp<int> qShadows, qAA, qAniso, qSkin, qRaycast, qPixel, qMip;
    IProp<float> qShadowDist, qLod;
    IProp<bool> qSoft, qProbes;
    int (*appGetFps)() = nullptr;
    void (*lightSetShadows)(void*, int) = nullptr;
    void (*camResetAspect)(void*) = nullptr;
    void (*screenSetRes)(int, int, int, void*) = nullptr;
    void* (*unloadUnused)() = nullptr;
} G;

template <class T>
void resolveProp(IProp<T>& p, const char* cls, const char* name) {
    std::string base = std::string("UnityEngine.") + cls + "::";
    p.get = reinterpret_cast<T (*)()>(icall((base + "get_" + name).c_str()));
    p.set = reinterpret_cast<void (*)(T)>(icall((base + "set_" + name).c_str()));
}

bool resolve() {
    if (G.tried) return G.ok;
    G.tried = true;
    Il2CppClass* cObject = findClass("UnityEngine", "Object");
    Il2CppClass* cComponent = findClass("UnityEngine", "Component");
    Il2CppClass* cMaterial = findClass("UnityEngine", "Material");
    Il2CppClass* cApp = findClass("UnityEngine", "Application");
    Il2CppClass* cQuality = findClass("UnityEngine", "QualitySettings");
    Il2CppClass* cScreen = findClass("UnityEngine", "Screen");
    Il2CppClass* cScene = findClass("UnityEngine.SceneManagement", "SceneManager");
    Il2CppClass* cSceneStruct = findClass("UnityEngine.SceneManagement", "Scene");
    G.cCamera = findClass("UnityEngine", "Camera");
    G.cRenderer = findClass("UnityEngine", "Renderer");
    G.cLight = findClass("UnityEngine", "Light");
    G.cBehaviour = findClass("UnityEngine", "Behaviour");
    if (!cObject || !cComponent || !cMaterial || !G.cCamera || !G.cRenderer || !G.cLight || !G.cBehaviour) {
        logf("perf: some Unity classes are missing, the performance options are not available");
        return false;
    }
    G.mRenGetEnabled = findMethod(G.cRenderer, "get_enabled", 0);
    G.mRenSetEnabled = findMethod(G.cRenderer, "set_enabled", 1);
    G.mGetSharedMats = findMethod(G.cRenderer, "get_sharedMaterials", 0);
    G.mGetLm = findMethod(G.cRenderer, "get_lightmapIndex", 0);
    G.mSetLm = findMethod(G.cRenderer, "set_lightmapIndex", 1);
    G.mGetGo = findMethod(cComponent, "get_gameObject", 0);
    G.mGetComponents = findMethod(cComponent, "GetComponents", 1, "System.Type");
    G.mGetName = findMethod(cObject, "get_name", 0);
    G.mGetHide = findMethod(cObject, "get_hideFlags", 0);
    G.mSetHide = findMethod(cObject, "set_hideFlags", 1);
    G.mHasProp = findMethod(cMaterial, "HasProperty", 1, "System.String");
    G.mGetTex = findMethod(cMaterial, "GetTexture", 1, "System.String");
    G.mSetTex = findMethod(cMaterial, "SetTexture", 2, "System.String");
    G.mCamGetAspect = findMethod(G.cCamera, "get_aspect", 0);
    G.mCamSetAspect = findMethod(G.cCamera, "set_aspect", 1);
    G.mCamGetOrtho = findMethod(G.cCamera, "get_orthographic", 0);
    G.mCamGetTarget = findMethod(G.cCamera, "get_targetTexture", 0);
    G.mCamGetFar = findMethod(G.cCamera, "get_farClipPlane", 0);
    G.mCamSetFar = findMethod(G.cCamera, "set_farClipPlane", 1);
    G.mBehGetEnabled = findMethod(G.cBehaviour, "get_enabled", 0);
    G.mBehSetEnabled = findMethod(G.cBehaviour, "set_enabled", 1);
    G.mLightGetShadows = findMethod(G.cLight, "get_shadows", 0);
    G.mSetTargetFps = findMethod(cApp, "set_targetFrameRate", 1);
    G.mGetVsync = findMethod(cQuality, "get_vSyncCount", 0);
    G.mSetVsync = findMethod(cQuality, "set_vSyncCount", 1);
    G.mScreenW = findMethod(cScreen, "get_width", 0);
    G.mScreenH = findMethod(cScreen, "get_height", 0);
    G.mScreenFull = findMethod(cScreen, "get_fullScreen", 0);
    G.mActiveScene = findMethod(cScene, "GetActiveScene", 0);
    G.mSceneHandle = findMethod(cSceneStruct, "get_handle", 0);

    resolveProp(G.qShadows, "QualitySettings", "shadows");
    resolveProp(G.qShadowDist, "QualitySettings", "shadowDistance");
    resolveProp(G.qPixel, "QualitySettings", "pixelLightCount");
    resolveProp(G.qAA, "QualitySettings", "antiAliasing");
    resolveProp(G.qAniso, "QualitySettings", "anisotropicFiltering");
    resolveProp(G.qSoft, "QualitySettings", "softParticles");
    resolveProp(G.qProbes, "QualitySettings", "realtimeReflectionProbes");
    resolveProp(G.qLod, "QualitySettings", "lodBias");
    resolveProp(G.qMip, "QualitySettings", "globalTextureMipmapLimit");
    resolveProp(G.qSkin, "QualitySettings", "skinWeights");
    resolveProp(G.qRaycast, "QualitySettings", "particleRaycastBudget");
    G.appGetFps = reinterpret_cast<int (*)()>(icall("UnityEngine.Application::get_targetFrameRate"));
    G.lightSetShadows = reinterpret_cast<void (*)(void*, int)>(icall("UnityEngine.Light::set_shadows"));
    G.camResetAspect = reinterpret_cast<void (*)(void*)>(icall("UnityEngine.Camera::ResetAspect"));
    G.screenSetRes = reinterpret_cast<void (*)(int, int, int, void*)>(icall("UnityEngine.Screen::SetResolution_Injected"));
    G.unloadUnused = reinterpret_cast<void* (*)()>(icall("UnityEngine.Resources::UnloadUnusedAssets"));
    // say what is missing, so a log tells which option cannot work in this game build
    std::string missing;
#define NEEDM(x) if (!G.x) missing += " " #x
    NEEDM(mRenGetEnabled); NEEDM(mRenSetEnabled); NEEDM(mGetSharedMats); NEEDM(mGetLm); NEEDM(mSetLm); NEEDM(mGetGo);
    NEEDM(mGetComponents); NEEDM(mGetName); NEEDM(mGetHide); NEEDM(mSetHide); NEEDM(mHasProp); NEEDM(mGetTex); NEEDM(mSetTex);
    NEEDM(mCamGetAspect); NEEDM(mCamSetAspect); NEEDM(mCamGetOrtho); NEEDM(mCamGetTarget); NEEDM(mCamGetFar); NEEDM(mCamSetFar);
    NEEDM(mBehGetEnabled); NEEDM(mBehSetEnabled); NEEDM(mLightGetShadows); NEEDM(mSetTargetFps); NEEDM(mGetVsync);
    NEEDM(mSetVsync); NEEDM(mScreenW); NEEDM(mScreenH); NEEDM(mScreenFull); NEEDM(mActiveScene); NEEDM(mSceneHandle);
    NEEDM(appGetFps); NEEDM(lightSetShadows); NEEDM(camResetAspect); NEEDM(screenSetRes); NEEDM(unloadUnused);
#undef NEEDM
    G.ok = true;
    logf("perf: ready%s%s", missing.empty() ? "" : ", not found:", missing.c_str());
    return true;
}

// ---------------------------------------------------------------- small wrappers
bool rGetEnabled(Il2CppObject* r) { return callBool(G.mRenGetEnabled, r, nullptr, false); }
void rSetEnabled(Il2CppObject* r, bool v) {
    void* a[1] = {&v};
    invoke(G.mRenSetEnabled, r, a);
}
void bSetEnabled(Il2CppObject* b, bool v) {
    void* a[1] = {&v};
    invoke(G.mBehSetEnabled, b, a);
}
float camFar(Il2CppObject* c) { return callFloat(G.mCamGetFar, c, nullptr, 0.f); }
void camSetFar(Il2CppObject* c, float v) {
    void* a[1] = {&v};
    invoke(G.mCamSetFar, c, a);
}
float camAspect(Il2CppObject* c) { return callFloat(G.mCamGetAspect, c, nullptr, 0.f); }
void camSetAspect(Il2CppObject* c, float v) {
    void* a[1] = {&v};
    invoke(G.mCamSetAspect, c, a);
}
int hideFlags(Il2CppObject* o) { return callInt(G.mGetHide, o, nullptr, 0); }
void setHideFlags(Il2CppObject* o, int v) {
    void* a[1] = {&v};
    invoke(G.mSetHide, o, a);
}
std::string nameOf(Il2CppObject* o) {
    if (!o || !G.mGetName) return "";
    return str(reinterpret_cast<Il2CppString*>(invoke(G.mGetName, o, nullptr)));
}

// managed strings for material property names, made once and kept
Il2CppString* propStr(const std::string& s) {
    static std::unordered_map<std::string, Il2CppString*> cache;
    auto it = cache.find(s);
    if (it != cache.end()) return it->second;
    Il2CppString* m = A.string_new(s.c_str());
    retain(reinterpret_cast<Il2CppObject*>(m));
    cache[s] = m;
    return m;
}

// ---------------------------------------------------------------- quality settings
struct Quality {
    bool saved = false;
    int level = -1;
    double next = 0;
    int shadows = 0, pixel = 0, aa = 0, aniso = 0, mip = 0, skin = 0, raycast = 0;
    float shadowDist = 0, lod = 0;
    bool soft = false, probes = false;
} Q;

template <class T>
void setIf(IProp<T>& p, T v) {
    if (p.ok() && p.get() != v) p.set(v);
}
template <class T>
void saveP(IProp<T>& p, T& out) {
    if (p.get) out = p.get();
}
template <class T>
void restoreP(IProp<T>& p, T v) {
    if (p.ok()) p.set(v);
}

void qualityTick(const FxSettings& s, double now) {
    if (s.perfLevel <= 0) {
        if (Q.saved) {
            restoreP(G.qShadows, Q.shadows);
            restoreP(G.qShadowDist, Q.shadowDist);
            restoreP(G.qPixel, Q.pixel);
            restoreP(G.qAA, Q.aa);
            restoreP(G.qAniso, Q.aniso);
            restoreP(G.qSoft, Q.soft);
            restoreP(G.qProbes, Q.probes);
            restoreP(G.qLod, Q.lod);
            restoreP(G.qMip, Q.mip);
            restoreP(G.qSkin, Q.skin);
            restoreP(G.qRaycast, Q.raycast);
            Q.saved = false;
            logf("perf: quality settings restored");
        }
        Q.level = -1;
        return;
    }
    if (!Q.saved) {
        saveP(G.qShadows, Q.shadows);
        saveP(G.qShadowDist, Q.shadowDist);
        saveP(G.qPixel, Q.pixel);
        saveP(G.qAA, Q.aa);
        saveP(G.qAniso, Q.aniso);
        saveP(G.qSoft, Q.soft);
        saveP(G.qProbes, Q.probes);
        saveP(G.qLod, Q.lod);
        saveP(G.qMip, Q.mip);
        saveP(G.qSkin, Q.skin);
        saveP(G.qRaycast, Q.raycast);
        Q.saved = true;
        logf("perf: quality settings saved");
    }
    // written again about every 1.5 s: the game may reset them (scene change, its own graphics menu)
    if (Q.level == s.perfLevel && now < Q.next) return;
    Q.level = s.perfLevel;
    Q.next = now + 1.5;

    bool ultra = s.perfLevel >= 2;
    setIf(G.qShadows, 0);  // Disable
    if (G.qShadowDist.ok() && G.qShadowDist.get() > 0.01f) G.qShadowDist.set(0.f);
    setIf(G.qPixel, ultra ? 0 : 1);
    setIf(G.qAA, 0);
    setIf(G.qAniso, 0);  // Disable
    setIf(G.qSoft, false);
    setIf(G.qProbes, false);
    float lod = ultra ? 0.2f : 0.5f;
    if (G.qLod.ok() && std::fabs(G.qLod.get() - lod) > 0.001f) G.qLod.set(lod);
    setIf(G.qMip, ultra ? 3 : 1);
    setIf(G.qSkin, ultra ? 1 : 2);  // OneBone / TwoBones
    setIf(G.qRaycast, ultra ? 4 : 16);
}

// ---------------------------------------------------------------- your own anti-aliasing, anisotropic and LOD bias
// These win over the graphics level above. A value that is switched off goes back to what the game (or the level) had.
struct Gfx {
    bool saved = false, aaOn = false, anOn = false, lodOn = false;
    int aa = 0, aniso = 0;
    float lod = 1.f;
    int lastAa = -1, lastAn = -1;
    bool lastLodOn = false;
    float lastLod = -1.f;
    double next = 0;
} Gx;

void gfxTick(const FxSettings& s, double now) {
    bool wantAa = s.aaMode > 0, wantAn = s.anisoMode > 0, wantLod = s.lodOn;
    if (!Gx.saved && (wantAa || wantAn || wantLod)) {
        // the game's own values (the ones the graphics level saved, if that is on)
        Gx.aa = Q.saved ? Q.aa : (G.qAA.get ? G.qAA.get() : 0);
        Gx.aniso = Q.saved ? Q.aniso : (G.qAniso.get ? G.qAniso.get() : 0);
        Gx.lod = Q.saved ? Q.lod : (G.qLod.get ? G.qLod.get() : 1.f);
        Gx.saved = true;
        Gx.next = 0;
        logf("perf: graphics options saved (aa %d, aniso %d, lod %.2f)", Gx.aa, Gx.aniso, Gx.lod);
    }
    // a value that was switched off: give it back (the graphics level writes its own again when it is on)
    if (Gx.aaOn && !wantAa) {
        if (s.perfLevel <= 0) restoreP(G.qAA, Gx.aa); else Q.level = -1;
        Gx.aaOn = false;
    }
    if (Gx.anOn && !wantAn) {
        if (s.perfLevel <= 0) restoreP(G.qAniso, Gx.aniso); else Q.level = -1;
        Gx.anOn = false;
    }
    if (Gx.lodOn && !wantLod) {
        if (s.perfLevel <= 0) restoreP(G.qLod, Gx.lod); else Q.level = -1;
        Gx.lodOn = false;
    }
    if (!wantAa && !wantAn && !wantLod) {
        if (Gx.saved) {
            Gx.saved = false;
            logf("perf: graphics options restored");
        }
        Gx.lastAa = Gx.lastAn = -1;
        Gx.lastLodOn = false;
        Gx.lastLod = -1.f;
        return;
    }

    float lod = std::min(2.f, std::max(0.3f, s.lodBias));
    bool changed = s.aaMode != Gx.lastAa || s.anisoMode != Gx.lastAn || s.lodOn != Gx.lastLodOn || std::fabs(lod - Gx.lastLod) > 0.001f;
    if (!changed && now < Gx.next) return;  // written again about every 1.5 s: the game may reset them
    Gx.lastAa = s.aaMode;
    Gx.lastAn = s.anisoMode;
    Gx.lastLodOn = s.lodOn;
    Gx.lastLod = lod;
    Gx.next = now + 1.5;

    if (wantAa) {
        static const int samples[5] = {0, 0, 2, 4, 8};
        setIf(G.qAA, samples[std::min(4, s.aaMode)]);
        Gx.aaOn = true;
    }
    if (wantAn) {
        setIf(G.qAniso, s.anisoMode == 1 ? 0 : 1);  // Disable / Enable
        Gx.anOn = true;
    }
    if (wantLod) {
        if (G.qLod.ok() && std::fabs(G.qLod.get() - lod) > 0.001f) G.qLod.set(lod);
        Gx.lodOn = true;
    }
}

// ---------------------------------------------------------------- render resolution
struct Res {
    bool active = false;  // a size other than the screen's own is applied
    int origW = 0, origH = 0, appW = 0, appH = 0;
    bool origFull = true;
    int wantW = 0, wantH = 0;
    double wantSince = 0;
} R;

int screenW() { return callInt(G.mScreenW, nullptr, nullptr, 0); }
int screenH() { return callInt(G.mScreenH, nullptr, nullptr, 0); }

// ---------------------------------------------------------------- adaptive resolution
// The render size follows the FPS: down one step when the frame rate stays under the target, back up (slowly,
// and slower each time it did not hold) when there is room. Never below 50 %.
const int kAdTargets[] = {30, 45, 60};
struct Adaptive {
    int idx = 0;
    int low = 0, high = 0;
    double next = 0, skipUntil = 0, lastUp = -1000, holdUp = 12;
    bool wasOn = false;
} Ad;
bool gAdaptiveOn = false;

void adaptiveTick(const FxSettings& s, double now) {
    gAdaptiveOn = s.adaptive && !s.customRes;
    if (!gAdaptiveOn) {
        if (Ad.wasOn) {
            Ad = Adaptive();
            Ad.wasOn = false;
        }
        return;
    }
    Ad.wasOn = true;
    if (now < 8.0 || now < Ad.skipUntil || now < Ad.next) return;
    Ad.next = now + 1.0;
    float fps = measuredFps();
    if (fps <= 1.f) return;
    float target = (float)kAdTargets[std::min(2, std::max(0, s.adaptTarget))];
    if (fps < target * 0.88f) {
        Ad.high = 0;
        if (++Ad.low >= 3) {
            Ad.low = 0;
            if (Ad.idx < 3) {
                Ad.idx++;
                if (now - Ad.lastUp < 20.0) Ad.holdUp = std::min(96.0, Ad.holdUp * 2.0);  // it did not hold: wait longer next time
                Ad.skipUntil = now + 3.0;
                logf("perf: adaptive resolution down to %d%% (%.0f FPS, wanted %.0f)", (int)std::lround(kResSteps[Ad.idx] * 100), fps, target);
            }
        }
    } else if (fps >= target * 0.97f && Ad.idx > 0) {
        Ad.low = 0;
        if (++Ad.high >= (int)Ad.holdUp) {
            Ad.high = 0;
            Ad.idx--;
            Ad.lastUp = now;
            Ad.skipUntil = now + 3.0;
            logf("perf: adaptive resolution up to %d%% (%.0f FPS)", (int)std::lround(kResSteps[Ad.idx] * 100), fps);
        }
    } else {
        Ad.low = 0;
    }
}

void resolutionTick(const FxSettings& s, double now) {
    if (now < 8.0) return;  // not while the game is still starting
    int want = gAdaptiveOn ? Ad.idx : std::min(4, std::max(0, s.resIndex));
    if (!R.active && !s.customRes && want == 0) return;
    if (!G.screenSetRes) return;

    if (!R.active) {  // the first change: remember the screen's own size
        R.origW = screenW();
        R.origH = screenH();
        R.origFull = callBool(G.mScreenFull, nullptr, nullptr, true);
    }
    if (R.origW < 100 || R.origH < 100) return;

    int w, h;
    if (s.customRes) {
        int cw = s.customW > 0 ? s.customW : (R.active ? R.appW : R.origW);
        int ch = s.customH > 0 ? s.customH : (R.active ? R.appH : R.origH);
        w = std::min(4096, std::max(320, cw));
        h = std::min(4096, std::max(180, ch));
    } else if (want > 0) {
        w = std::max(320, (int)std::lround(R.origW * kResSteps[want]));
        h = std::max(180, (int)std::lround(R.origH * kResSteps[want]));
    } else {
        w = R.origW;
        h = R.origH;
    }
    if (R.active ? (w == R.appW && h == R.appH) : (w == R.origW && h == R.origH)) return;

    // applied only once it has stayed the same for a moment: every change rebuilds the screen surface
    if (w != R.wantW || h != R.wantH) {
        R.wantW = w;
        R.wantH = h;
        R.wantSince = now;
        return;
    }
    if (now - R.wantSince < 0.6) return;

    uint32_t refreshRate[2] = {0, 0};  // RefreshRate {0, 0}: the device's own
    G.screenSetRes(w, h, R.origFull ? 1 : 3, refreshRate);  // FullScreenWindow / Windowed
    R.appW = w;
    R.appH = h;
    R.active = !(w == R.origW && h == R.origH);
    logf("perf: resolution %dx%d%s", w, h, R.active ? "" : " (the screen's own size)");
}

// ---------------------------------------------------------------- squash (aspect ratio of the 3D view)
std::unordered_map<Il2CppObject*, Held> gSqCams;
double gSqNext = 0;

void squashTick(const FxSettings& s, double now) {
    if (!s.squashOn) {
        for (auto& kv : gSqCams) {
            if (alive(kv.first) && G.camResetAspect) G.camResetAspect(kv.first);
            drop(kv.second);
        }
        gSqCams.clear();
        return;
    }
    if (now >= gSqNext) {  // new cameras are looked for twice a second
        gSqNext = now + 0.5;
        for (auto it = gSqCams.begin(); it != gSqCams.end();) {
            if (!alive(it->first)) {
                drop(it->second);
                it = gSqCams.erase(it);
            } else {
                ++it;
            }
        }
        Il2CppObject* arr = findObjectsOfType(G.cCamera);
        if (arr) {
            size_t n = A.array_length(arr);
            Il2CppObject** el = arrayData(arr);
            for (size_t i = 0; i < n && i < 64; i++) {
                Il2CppObject* c = el[i];
                if (!c || !alive(c) || gSqCams.count(c)) continue;
                if (callBool(G.mCamGetOrtho, c, nullptr, false)) continue;
                if (invoke(G.mCamGetTarget, c, nullptr)) continue;  // only what is drawn on the screen
                gSqCams[c] = hold(c);
            }
            keepAlive(arr);
        }
    }
    int sw = screenW(), sh = screenH();
    float screen = sh > 0 ? (float)sw / sh : 16.f / 9.f;
    float aspect = screen * std::min(3.f, std::max(0.3f, s.squash));
    for (auto& kv : gSqCams)
        if (alive(kv.first) && std::fabs(camAspect(kv.first) - aspect) > 0.001f) camSetAspect(kv.first, aspect);
}

// ---------------------------------------------------------------- FPS cap
struct Fps {
    bool saved = false;
    int origFps = -1, origVsync = 1;
    double next = 0;
} Fp;

void fpsTick(const FxSettings& s, double now) {
    int cap = (s.fpsIndex > 0 && s.fpsIndex < 5) ? kFpsSteps[s.fpsIndex] : 0;
    if (cap > 0) {
        if (!Fp.saved) {
            Fp.origFps = G.appGetFps ? G.appGetFps() : -1;
            Fp.origVsync = callInt(G.mGetVsync, nullptr, nullptr, 1);
            Fp.saved = true;
        }
        if (now < Fp.next) return;
        Fp.next = now + 2.0;
        int zero = 0, c = cap;
        if (callInt(G.mGetVsync, nullptr, nullptr, 0) != 0) {
            void* a[1] = {&zero};
            invoke(G.mSetVsync, nullptr, a);
        }
        void* b[1] = {&c};
        invoke(G.mSetTargetFps, nullptr, b);
    } else if (Fp.saved) {
        int v = Fp.origVsync, f = Fp.origFps;
        void* a[1] = {&v};
        invoke(G.mSetVsync, nullptr, a);
        void* b[1] = {&f};
        invoke(G.mSetTargetFps, nullptr, b);
        Fp.saved = false;
    }
}

// ---------------------------------------------------------------- renderers: textures, particles, shadows
struct TexRef {
    std::string prop;
    Held tex;
    int flags = 0;
};
struct MatEntry {
    Held mat;
    std::vector<std::string> props;  // the texture slots this material has
    std::vector<TexRef> refs;
    int stamp = 0;
};

const char* kTexProps[] = {"_MainTex", "_BaseMap", "_BaseColorMap", "_BumpMap", "_NormalMap", "_EmissionMap",
                           "_DetailAlbedoMap", "_DetailNormalMap", "_OcclusionMap", "_MetallicGlossMap", "_SpecGlossMap",
                           "_ParallaxMap", "_DetailMask", "_Tex", "_Texture", "_DiffuseMap", "_Diffuse"};
constexpr int kDontUnload = 32;  // HideFlags.DontUnloadUnusedAsset

std::unordered_map<Il2CppObject*, MatEntry> gMats;
std::unordered_map<Il2CppObject*, Held> gParts, gShadowParts;
struct LmEntry {
    Held r;
    int idx = 0;
};
std::unordered_map<Il2CppObject*, LmEntry> gLm;

Held gQueueArr;
size_t gQueueN = 0, gQueuePos = 0;
int gScanId = 0;
double gNextScan = 0, gBurstUntil = 0;
int gNsNames = 0;
bool gNsLogged = false;

void stripMaterial(Il2CppObject* m) {
    if (!alive(m)) return;
    auto it = gMats.find(m);
    if (it == gMats.end()) {
        MatEntry e;
        e.mat = hold(m);
        for (const char* p : kTexProps) {
            void* a[1] = {propStr(p)};
            if (callBool(G.mHasProp, m, a, false)) e.props.push_back(p);
        }
        it = gMats.emplace(m, std::move(e)).first;
    }
    MatEntry& e = it->second;
    if (e.stamp == gScanId) return;  // shared by many renderers: once per scan is enough
    e.stamp = gScanId;

    for (const std::string& prop : e.props) {
        void* ga[1] = {propStr(prop)};
        Il2CppObject* t = invoke(G.mGetTex, m, ga);
        if (!t) continue;
        TexRef* r = nullptr;
        for (TexRef& x : e.refs)
            if (x.prop == prop) {
                r = &x;
                break;
            }
        if (!r) {
            e.refs.emplace_back();
            r = &e.refs.back();
            r->prop = prop;
        }
        if (r->tex.o != t) {
            // the game put a different texture there: the old one is no longer ours to hold on to
            if (r->tex.o && alive(r->tex.o)) setHideFlags(r->tex.o, r->flags);
            drop(r->tex);
            r->tex = hold(t);
            r->flags = hideFlags(t);
            // keep the texture alive: "unload unused assets" must not destroy what we may have to put back
            setHideFlags(t, r->flags | kDontUnload);
        }
        void* sa[2] = {propStr(prop), nullptr};
        invoke(G.mSetTex, m, sa);
    }
}

void stripTextures(Il2CppObject* r) {
    Il2CppObject* arr = invoke(G.mGetSharedMats, r, nullptr);
    if (!arr) return;
    size_t n = A.array_length(arr);
    Il2CppObject** el = arrayData(arr);
    for (size_t i = 0; i < n && i < 32; i++)
        if (el[i]) stripMaterial(el[i]);
    keepAlive(arr);
}

bool hasShadowWord(const std::string& s) { return containsI(s, "shadow") || containsI(s, "blob"); }

bool looksLikeShadow(Il2CppObject* r, std::string& why) {
    Il2CppObject* go = invoke(G.mGetGo, r, nullptr);
    std::string goName = go ? nameOf(go) : "";
    if (hasShadowWord(goName)) {
        why = "object " + goName;
        return true;
    }
    Il2CppObject* arr = invoke(G.mGetSharedMats, r, nullptr);
    if (arr) {
        size_t n = A.array_length(arr);
        Il2CppObject** el = arrayData(arr);
        for (size_t i = 0; i < n && i < 32; i++) {
            if (!el[i]) continue;
            std::string mn = nameOf(el[i]);
            if (hasShadowWord(mn)) {
                why = "material " + mn + " on " + goName;
                keepAlive(arr);
                return true;
            }
        }
        keepAlive(arr);
    }
    return false;
}

// Baked shadows: this game paints its shadows into lightmaps. Every renderer is told it has no lightmap
// (lightmapIndex -1) and its own index is put back when the switch is turned off.
void dropLightmap(Il2CppObject* r) {
    int li = callInt(G.mGetLm, r, nullptr, -1);
    if (li < 0 || li >= 65534) return;  // none already
    if (!gLm.count(r)) {
        LmEntry e;
        e.r = hold(r);
        e.idx = li;
        gLm[r] = e;
    }
    int none = -1;
    void* a[1] = {&none};
    invoke(G.mSetLm, r, a);
}

void handleRenderer(const FxSettings& s, Il2CppObject* r) {
    if (!r || !alive(r)) return;
    if (s.noShadow) {
        if (rGetEnabled(r)) {
            std::string why;
            if (looksLikeShadow(r, why)) {
                gShadowParts[r] = hold(r);
                rSetEnabled(r, false);
                if (gNsNames < 25) {
                    gNsNames++;
                    logf("no shadow: hid %s", why.c_str());
                }
                return;
            }
        }
        dropLightmap(r);
    }
    if (s.hideParticles && rGetEnabled(r) && isA(r, "ParticleSystemRenderer")) {
        gParts[r] = hold(r);
        rSetEnabled(r, false);
        return;
    }
    if (s.noTextures) stripTextures(r);
}

void restoreTextures() {
    for (auto& kv : gMats) {
        MatEntry& e = kv.second;
        bool live = alive(e.mat.o);
        for (TexRef& r : e.refs) {
            if (r.tex.o && alive(r.tex.o)) {
                if (live) {
                    void* a[2] = {propStr(r.prop), r.tex.o};
                    invoke(G.mSetTex, e.mat.o, a);
                }
                setHideFlags(r.tex.o, r.flags);
            }
            drop(r.tex);
        }
        drop(e.mat);
    }
    logf("perf: textures restored (%d materials)", (int)gMats.size());
    gMats.clear();
}

void restoreHeld(std::unordered_map<Il2CppObject*, Held>& m) {
    for (auto& kv : m) {
        if (alive(kv.first)) rSetEnabled(kv.first, true);
        drop(kv.second);
    }
    m.clear();
}

void restoreLightmaps() {
    for (auto& kv : gLm) {
        if (alive(kv.first)) {
            int idx = kv.second.idx;
            void* a[1] = {&idx};
            invoke(G.mSetLm, kv.first, a);
        }
        drop(kv.second.r);
    }
    gLm.clear();
}

void rendererTick(const FxSettings& s, double now) {
    if (!s.noTextures && !gMats.empty()) restoreTextures();
    if (!s.hideParticles && !gParts.empty()) restoreHeld(gParts);
    if (!s.noTextures && !s.hideParticles && !s.noShadow) {
        drop(gQueueArr);
        gQueueN = gQueuePos = 0;
        return;
    }

    // a new scan when the previous one is finished and the timer ran out; right after a scene load they come quickly
    if (gQueuePos >= gQueueN && now >= gNextScan) {
        drop(gQueueArr);
        gQueueN = gQueuePos = 0;
        gScanId++;
        gNextScan = now + (now < gBurstUntil ? 1.0 : 3.0);
        Il2CppObject* arr = findObjectsOfType(G.cRenderer);
        if (arr) {
            gQueueArr = hold(arr);
            gQueueN = A.array_length(arr);
        }
    }
    if (!gQueueArr.o) return;

    // spread over frames by time, not by count, so a big scene is finished quickly without a stutter
    auto t0 = std::chrono::steady_clock::now();
    int n = 0;
    Il2CppObject** el = arrayData(gQueueArr.o);
    while (gQueuePos < gQueueN && n < kMaxPerFrame) {
        Il2CppObject* r = el[gQueuePos++];
        n++;
        handleRenderer(s, r);
        if ((n & 3) == 0 &&
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() >= kBudgetMs)
            break;
    }
    if (gQueuePos >= gQueueN) drop(gQueueArr);
}

// ---------------------------------------------------------------- post-processing, lights, draw distance
const char* kFxNames[] = {"PostProcess", "Bloom", "Blur", "SSAO", "SunShafts", "DepthOfField", "Antialiasing", "ScreenSpace"};
std::unordered_map<Il2CppObject*, Held> gFx;
double gFxNext = 0, gLightNext = 0, gCamNext = 0;

bool isFxName(const char* n) {
    std::string s = n;
    for (const char* k : kFxNames)
        if (containsI(s, k)) return true;
    return false;
}

void postFxTick(const FxSettings& s, double now) {
    if (!s.noPostFx) {
        for (auto& kv : gFx) {
            if (alive(kv.first)) bSetEnabled(kv.first, true);
            drop(kv.second);
        }
        gFx.clear();
        return;
    }
    if (now < gFxNext) return;
    gFxNext = now + 2.0;
    Il2CppObject* cams = findObjectsOfType(G.cCamera);
    if (!cams) return;
    size_t nc = A.array_length(cams);
    Il2CppObject** ce = arrayData(cams);
    Il2CppObject* behType = typeObject(G.cBehaviour);
    for (size_t i = 0; i < nc && i < 64; i++) {
        Il2CppObject* c = ce[i];
        if (!c || !alive(c)) continue;
        void* a[1] = {behType};
        Il2CppObject* comps = invoke(G.mGetComponents, c, a);
        if (!comps) continue;
        size_t n = A.array_length(comps);
        Il2CppObject** el = arrayData(comps);
        for (size_t k = 0; k < n && k < 64; k++) {
            Il2CppObject* b = el[k];
            if (!b || !alive(b) || !callBool(G.mBehGetEnabled, b, nullptr, false)) continue;
            if (!isFxName(className(b))) continue;
            if (!gFx.count(b)) gFx[b] = hold(b);
            bSetEnabled(b, false);
        }
        keepAlive(comps);
    }
    keepAlive(cams);
}

// No shadow: real-time shadows (quality + every light), baked shadows (lightmaps), fake shadows (objects named shadow/blob)
struct LightEntry {
    Held h;
    int orig = 0;
};
std::unordered_map<Il2CppObject*, LightEntry> gLights;
struct NsQuality {
    bool saved = false;
    int shadows = 0;
    float dist = 0;
} Ns;

void applyShadowQuality() {
    if (Q.saved) return;  // the quality level already switches shadows off and puts them back
    if (!Ns.saved) {
        if (G.qShadows.get) Ns.shadows = G.qShadows.get();
        if (G.qShadowDist.get) Ns.dist = G.qShadowDist.get();
        Ns.saved = true;
    }
    setIf(G.qShadows, 0);
    if (G.qShadowDist.ok() && G.qShadowDist.get() > 0.01f) G.qShadowDist.set(0.f);
}

void restoreShadowQuality() {
    if (!Ns.saved) return;
    if (!Q.saved) {
        restoreP(G.qShadows, Ns.shadows);
        restoreP(G.qShadowDist, Ns.dist);
    }
    Ns.saved = false;
}

void lightTick(const FxSettings& s, double now) {
    if (!s.noShadow) {
        restoreLightmaps();
        restoreShadowQuality();
        if (!gShadowParts.empty()) restoreHeld(gShadowParts);
        for (auto& kv : gLights) {
            if (alive(kv.first) && G.lightSetShadows) G.lightSetShadows(kv.first, kv.second.orig);
            drop(kv.second.h);
        }
        gLights.clear();
        gNsLogged = false;
        gNsNames = 0;
        return;
    }
    if (now < gLightNext) return;
    gLightNext = now + 0.5;
    applyShadowQuality();
    if (!G.lightSetShadows) return;

    Il2CppObject* arr = findObjectsOfType(G.cLight);
    int withShadow = 0;
    size_t total = 0;
    if (arr) {
        total = A.array_length(arr);
        Il2CppObject** el = arrayData(arr);
        for (size_t i = 0; i < total && i < 256; i++) {
            Il2CppObject* l = el[i];
            if (!l || !alive(l)) continue;
            int sh = callInt(G.mLightGetShadows, l, nullptr, 0);
            if (sh == 0) continue;
            withShadow++;
            if (!gLights.count(l)) {
                LightEntry e;
                e.h = hold(l);
                e.orig = sh;
                gLights[l] = e;
            }
            G.lightSetShadows(l, 0);
        }
        keepAlive(arr);
    }
    if (!gNsLogged) {
        gNsLogged = true;
        logf("no shadow: %d lights, %d with shadows switched off", (int)total, withShadow);
    }
}

struct CamEntry {
    Held h;
    float far = 0;
};
std::unordered_map<Il2CppObject*, CamEntry> gCams;

void distanceTick(const FxSettings& s, double now) {
    if (!s.drawDistOn) {
        for (auto& kv : gCams) {
            if (alive(kv.first)) camSetFar(kv.first, kv.second.far);
            drop(kv.second.h);
        }
        gCams.clear();
        return;
    }
    if (now < gCamNext) return;
    gCamNext = now + 2.0;
    Il2CppObject* arr = findObjectsOfType(G.cCamera);
    if (!arr) return;
    size_t n = A.array_length(arr);
    Il2CppObject** el = arrayData(arr);
    for (size_t i = 0; i < n && i < 64; i++) {
        Il2CppObject* c = el[i];
        if (!c || !alive(c) || callBool(G.mCamGetOrtho, c, nullptr, false)) continue;
        if (!gCams.count(c)) {
            CamEntry e;
            e.h = hold(c);
            e.far = camFar(c);
            gCams[c] = e;
        }
        if (std::fabs(camFar(c) - s.drawDist) > 0.5f) camSetFar(c, s.drawDist);
    }
    keepAlive(arr);
}

// ---------------------------------------------------------------- memory
double gCleanAt = -1;
int gLastCleanReq = -1;

void cleanMemory() {
    if (G.unloadUnused) G.unloadUnused();
    if (A.gc_collect) A.gc_collect(0);
    logf("perf: memory cleaned");
    postToast("Memory cleaned");
}

// ---------------------------------------------------------------- scene changes
int gSceneHandle = -999999;
double gSceneNext = 0;

template <class V>
void pruneDead(std::unordered_map<Il2CppObject*, V>& m, Held V::*h) {
    for (auto it = m.begin(); it != m.end();) {
        if (!alive(it->first)) {
            drop(it->second.*h);
            it = m.erase(it);
        } else {
            ++it;
        }
    }
}
void pruneHeld(std::unordered_map<Il2CppObject*, Held>& m) {
    for (auto it = m.begin(); it != m.end();) {
        if (!alive(it->first)) {
            drop(it->second);
            it = m.erase(it);
        } else {
            ++it;
        }
    }
}

void onSceneLoaded(double now) {
    // objects of the old scene are gone: forget them, and look at the new scene soon (not instantly:
    // the scene is still being set up and the game is busy)
    for (auto it = gMats.begin(); it != gMats.end();) {
        if (!alive(it->first)) {
            for (TexRef& r : it->second.refs) drop(r.tex);
            drop(it->second.mat);
            it = gMats.erase(it);
        } else {
            ++it;
        }
    }
    pruneHeld(gParts);
    pruneHeld(gFx);
    pruneHeld(gShadowParts);
    pruneDead(gLights, &LightEntry::h);
    for (auto it = gLm.begin(); it != gLm.end();) {  // belonged to the old scene
        drop(it->second.r);
        it = gLm.erase(it);
    }
    drop(gQueueArr);
    gQueueN = gQueuePos = 0;
    gNextScan = now + 0.4;
    gBurstUntil = now + 10.0;
    gFxNext = now + 1.0;
    gLightNext = now + 0.3;
    gCamNext = now + 1.0;
    Q.next = 0;
    Ad.skipUntil = now + 4.0;  // the frame rate of a loading scene says nothing
    fog::onSceneLoaded();
    scene::onSceneLoaded();
}

std::string buildStatus() {
    std::string res = R.active ? std::to_string(R.appW) + "x" + std::to_string(R.appH) : "native";
    return "Textures removed: " + std::to_string(gMats.size()) + " materials\n" +
           "Particles hidden: " + std::to_string(gParts.size()) + "   Shadow objects hidden: " +
           std::to_string(gShadowParts.size()) + "   Light shadows off: " + std::to_string(gLights.size()) + "\n" +
           "Effects off: " + std::to_string(gFx.size()) + "   Resolution: " + res;
}

std::string resolutionInfo() {
    std::string now = R.active ? std::to_string(R.appW) + "x" + std::to_string(R.appH) : "the screen's own size";
    int w = R.origW > 0 ? R.origW : screenW(), h = R.origW > 0 ? R.origH : screenH();
    std::string r = "Now: " + now + " (screen " + std::to_string(w) + "x" + std::to_string(h) +
                    "). A size with another shape than the screen is stretched to fit it.";
    if (gAdaptiveOn) r = "Adaptive: " + std::to_string((int)std::lround(kResSteps[Ad.idx] * 100)) + "% of the screen. " + r;
    return r;
}

double gStatusNext = 0;
int64_t gLastTickMs = 0;

}  // namespace

void tick(const FxSettings& s) {
    int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (ms - gLastTickMs < 16) return;  // about once per frame
    gLastTickMs = ms;
    if (!resolve()) return;
    double now = nowS();

    if (now >= gSceneNext && G.mActiveScene && G.mSceneHandle) {
        gSceneNext = now + 0.25;
        Il2CppObject* sc = invoke(G.mActiveScene, nullptr, nullptr);
        void* p = unbox(sc);
        int handle = p ? *static_cast<int*>(p) : gSceneHandle;
        if (handle != gSceneHandle) {
            bool first = gSceneHandle == -999999;
            gSceneHandle = handle;
            if (!first) {
                logf("scene changed");
                onSceneLoaded(now);
                if (s.autoClean) gCleanAt = now + 3.0;
            }
        }
    }

    qualityTick(s, now);
    gfxTick(s, now);
    adaptiveTick(s, now);
    resolutionTick(s, now);
    squashTick(s, now);
    fpsTick(s, now);
    rendererTick(s, now);
    postFxTick(s, now);
    lightTick(s, now);
    distanceTick(s, now);

    if (gLastCleanReq < 0) gLastCleanReq = s.cleanReq;
    if (s.cleanReq != gLastCleanReq) {
        gLastCleanReq = s.cleanReq;
        cleanMemory();
    }
    if (gCleanAt > 0 && now >= gCleanAt) {
        gCleanAt = -1;
        cleanMemory();
    }

    if (now >= gStatusNext) {
        gStatusNext = now + 0.5;
        setPerfStatus(buildStatus(), resolutionInfo(), R.active ? R.appW : screenW(), R.active ? R.appH : screenH());
    }
}

}  // namespace perf
}  // namespace vm
