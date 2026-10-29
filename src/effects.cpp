#include "effects.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

#include "il2cpp.h"
#include "log.h"
#include "shared.h"

// The colour effects. This is the same idea as the C# version: a global PostProcessVolume that sits above the
// game's own volumes, with a ColorGrading effect whose values are overridden only for the switched-on effects.
// Everything is found by class / field name through the il2cpp API.

namespace vm {
namespace fx {
namespace {

using namespace il2;

struct Param {
    Il2CppObject* obj = nullptr;
    size_t offValue = 0;
    size_t offOverride = 0;
};

struct Ctx {
    bool resolved = false;
    bool gaveUp = false;
    int tries = 0;
    int64_t nextTryMs = 0;

    Il2CppClass *cObject = nullptr, *cScriptable = nullptr, *cGameObject = nullptr, *cBehaviour = nullptr;
    Il2CppClass *cVol = nullptr, *cProf = nullptr, *cLayer = nullptr, *cFxBase = nullptr, *cCG = nullptr;

    const MethodInfo *mFindObjs = nullptr, *mDontDestroy = nullptr, *mCreateSO = nullptr, *mAddSettings = nullptr;
    const MethodInfo *mGoCtor = nullptr, *mAddComp = nullptr, *mSetLayer = nullptr, *mSetActive = nullptr, *mGetEnabled = nullptr;

    size_t offCachedPtr = 0x10;
    size_t offVolumeLayer = 0;
    size_t offIsGlobal = 0, offPriority = 0, offWeight = 0, offSharedProfile = 0;

    // touch
    bool touchTried = false;
    const MethodInfo *mTouchCount = nullptr, *mGetTouch = nullptr;
} C;

struct Built {
    bool built = false;
    Il2CppObject* profile = nullptr;
    Il2CppObject* cg = nullptr;
    std::vector<Il2CppObject*> gos;
    std::string sig;
    bool active = true;

    size_t offActive = 0;
    Param enabled, postExposure, brightness, saturation, contrast, hueShift, temperature, tint;
} B;

bool gWasOn = false;
bool gForce = true;
int64_t gNextBuildMs = 0, gNextCheckMs = 0;
int gFails = 0;
int64_t gRetryAtMs = 0;
unsigned gVersion = 0;
FxSettings gCur;
bool gLoggedApplied = false;

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

const char* PP = "UnityEngine.Rendering.PostProcessing";

bool resolve() {
    if (C.resolved) return true;
    if (C.gaveUp) return false;
    int64_t now = nowMs();
    if (now < C.nextTryMs) return false;
    C.nextTryMs = now + 2000;
    C.tries++;

    std::string missing;
    auto cls = [&](Il2CppClass*& out, const char* ns, const char* name) {
        out = findClass(ns, name);
        if (!out) missing += std::string(" ") + name;
    };
    cls(C.cObject, "UnityEngine", "Object");
    cls(C.cScriptable, "UnityEngine", "ScriptableObject");
    cls(C.cGameObject, "UnityEngine", "GameObject");
    cls(C.cBehaviour, "UnityEngine", "Behaviour");
    cls(C.cVol, PP, "PostProcessVolume");
    cls(C.cProf, PP, "PostProcessProfile");
    cls(C.cLayer, PP, "PostProcessLayer");
    cls(C.cFxBase, PP, "PostProcessEffectSettings");
    cls(C.cCG, PP, "ColorGrading");

    if (missing.empty()) {
        C.mFindObjs = findMethod(C.cObject, "FindObjectsOfType", 1, "System.Type");
        C.mDontDestroy = findMethod(C.cObject, "DontDestroyOnLoad", 1);
        C.mCreateSO = findMethod(C.cScriptable, "CreateInstance", 1, "System.Type");
        C.mAddSettings = findMethod(C.cProf, "AddSettings", 1, "PostProcessEffectSettings");
        C.mGoCtor = findMethod(C.cGameObject, ".ctor", 1, "System.String");
        C.mAddComp = findMethod(C.cGameObject, "AddComponent", 1, "System.Type");
        C.mSetLayer = findMethod(C.cGameObject, "set_layer", 1);
        C.mSetActive = findMethod(C.cGameObject, "SetActive", 1);
        C.mGetEnabled = findMethod(C.cBehaviour, "get_enabled", 0);

        if (!C.mFindObjs) missing += " Object.FindObjectsOfType(Type)";
        if (!C.mDontDestroy) missing += " Object.DontDestroyOnLoad";
        if (!C.mCreateSO) missing += " ScriptableObject.CreateInstance(Type)";
        if (!C.mAddSettings) missing += " PostProcessProfile.AddSettings";
        if (!C.mGoCtor) missing += " GameObject..ctor(string)";
        if (!C.mAddComp) missing += " GameObject.AddComponent(Type)";
        if (!C.mSetLayer) missing += " GameObject.set_layer";
        if (!C.mSetActive) missing += " GameObject.SetActive";
        if (!C.mGetEnabled) missing += " Behaviour.get_enabled";

        size_t o = fieldOffset(C.cObject, "m_CachedPtr");
        if (o) C.offCachedPtr = o;
        C.offVolumeLayer = fieldOffset(C.cLayer, "volumeLayer");
        C.offIsGlobal = fieldOffset(C.cVol, "isGlobal");
        C.offPriority = fieldOffset(C.cVol, "priority");
        C.offWeight = fieldOffset(C.cVol, "weight");
        C.offSharedProfile = fieldOffset(C.cVol, "sharedProfile");
        if (!C.offVolumeLayer) missing += " PostProcessLayer.volumeLayer";
        if (!C.offIsGlobal) missing += " PostProcessVolume.isGlobal";
        if (!C.offPriority) missing += " PostProcessVolume.priority";
        if (!C.offWeight) missing += " PostProcessVolume.weight";
        if (!C.offSharedProfile) missing += " PostProcessVolume.sharedProfile";
    }

    if (!missing.empty()) {
        if (C.tries == 1 || C.tries % 15 == 0) logf("effects: not found yet:%s (try %d)", missing.c_str(), C.tries);
        if (C.tries >= 60) {
            C.gaveUp = true;
            logf("effects: giving up, this game does not seem to have post-processing v2");
        }
        return false;
    }
    C.resolved = true;
    logf("effects: post-processing classes found");
    return true;
}

bool isAlive(Il2CppObject* o) { return o && *fieldPtr<void*>(o, C.offCachedPtr) != nullptr; }

bool isEnabled(Il2CppObject* o) {
    Il2CppObject* b = invoke(C.mGetEnabled, o, nullptr);
    void* p = unbox(b);
    return p && *static_cast<bool*>(p);
}

std::vector<int> collectMasks() {
    std::vector<int> masks;
    void* args[1] = {typeObject(C.cLayer)};
    Il2CppObject* arr = invoke(C.mFindObjs, nullptr, args);
    if (!arr) return masks;
    size_t n = A.array_length(arr);
    Il2CppObject** el = reinterpret_cast<Il2CppObject**>(reinterpret_cast<char*>(arr) + 0x20);
    for (size_t i = 0; i < n && i < 64; i++) {
        Il2CppObject* o = el[i];
        if (!o || !isEnabled(o)) continue;
        int m = *fieldPtr<int>(o, C.offVolumeLayer);
        if (m != 0 && std::find(masks.begin(), masks.end(), m) == masks.end()) masks.push_back(m);
    }
    return masks;
}

std::string sigOf(std::vector<int> masks) {
    std::sort(masks.begin(), masks.end());
    std::string s;
    for (int m : masks) s += std::to_string(m) + ",";
    return s;
}

// Fewest layers such that every camera layer has one of them in its mask.
std::vector<int> pickLayers(const std::vector<int>& masks) {
    std::vector<int> layers;
    for (int mask : masks) {
        bool covered = false;
        for (int l : layers)
            if ((mask >> l) & 1) { covered = true; break; }
        if (covered) continue;
        for (int i = 0; i < 32; i++)
            if ((mask >> i) & 1) { layers.push_back(i); break; }
    }
    return layers;
}

Param getParam(Il2CppObject* fx, const char* name) {
    Param p;
    size_t off = fieldOffset(classOf(fx), name);
    if (!off) {
        logf("effects: field %s not found", name);
        return p;
    }
    Il2CppObject* po = *fieldPtr<Il2CppObject*>(fx, off);
    if (!po) return p;
    Il2CppClass* pc = classOf(po);
    size_t ov = fieldOffset(pc, "value");
    size_t oo = fieldOffset(pc, "overrideState");
    if (!ov || !oo) {
        logf("effects: %s has no value/overrideState", name);
        return p;
    }
    p.obj = po;
    p.offValue = ov;
    p.offOverride = oo;
    return p;
}

void setF(const Param& p, float v, bool ov) {
    if (!p.obj) return;
    *fieldPtr<float>(p.obj, p.offValue) = v;
    *fieldPtr<bool>(p.obj, p.offOverride) = ov;
}

void setB(const Param& p, bool v, bool ov) {
    if (!p.obj) return;
    *fieldPtr<bool>(p.obj, p.offValue) = v;
    *fieldPtr<bool>(p.obj, p.offOverride) = ov;
}

Il2CppObject* newEffect(Il2CppClass* k) {
    void* a[1] = {typeObject(k)};
    Il2CppObject* fx = invoke(C.mCreateSO, nullptr, a);
    if (!fx) return nullptr;
    void* b[1] = {fx};
    invoke(C.mAddSettings, B.profile, b);
    return fx;
}

void destroyAll() {
    // the objects are left to Unity; we only forget them (a destroyed volume unregisters itself)
    B = Built();
}

bool anyGone() {
    if (!B.built) return true;
    if (B.gos.empty()) return true;
    for (Il2CppObject* g : B.gos)
        if (!isAlive(g)) return true;
    return false;
}

void setAllActive(bool on) {
    for (Il2CppObject* g : B.gos) {
        bool v = on;
        void* a[1] = {&v};
        invoke(C.mSetActive, g, a);
    }
    B.active = on;
}

bool build() {
    std::vector<int> masks = collectMasks();
    if (masks.empty()) return false;  // no post-processing camera yet
    std::vector<int> layers = pickLayers(masks);

    B = Built();
    void* pa[1] = {typeObject(C.cProf)};
    B.profile = invoke(C.mCreateSO, nullptr, pa);
    if (!B.profile) {
        logf("effects: could not create the profile");
        return false;
    }
    A.gchandle_new(B.profile, 0);
    B.cg = newEffect(C.cCG);
    if (!B.cg) {
        logf("effects: could not create ColorGrading");
        B = Built();
        return false;
    }
    A.gchandle_new(B.cg, 0);

    B.offActive = fieldOffset(classOf(B.cg), "active");
    B.enabled = getParam(B.cg, "enabled");
    B.postExposure = getParam(B.cg, "postExposure");
    B.brightness = getParam(B.cg, "brightness");
    B.saturation = getParam(B.cg, "saturation");
    B.contrast = getParam(B.cg, "contrast");
    B.hueShift = getParam(B.cg, "hueShift");
    B.temperature = getParam(B.cg, "temperature");
    B.tint = getParam(B.cg, "tint");

    std::string layerText;
    for (int layer : layers) {
        Il2CppObject* go = A.object_new(C.cGameObject);
        void* na[1] = {A.string_new("VisualMobile Visual")};
        invoke(C.mGoCtor, go, na);
        int l = layer;
        void* la[1] = {&l};
        invoke(C.mSetLayer, go, la);
        void* da[1] = {go};
        invoke(C.mDontDestroy, nullptr, da);

        void* ca[1] = {typeObject(C.cVol)};
        Il2CppObject* vol = invoke(C.mAddComp, go, ca);
        if (!vol) {
            logf("effects: could not add the volume to layer %d", layer);
            continue;
        }
        *fieldPtr<bool>(vol, C.offIsGlobal) = true;
        *fieldPtr<float>(vol, C.offPriority) = 1000000.f;
        *fieldPtr<float>(vol, C.offWeight) = 1.f;
        void** target = fieldPtr<void*>(vol, C.offSharedProfile);
        if (A.gc_wbarrier_set_field) A.gc_wbarrier_set_field(vol, target, B.profile);
        else *target = B.profile;

        A.gchandle_new(go, 0);
        B.gos.push_back(go);
        layerText += std::to_string(layer) + " ";
    }
    if (B.gos.empty()) {
        B = Built();
        return false;
    }
    B.sig = sigOf(masks);
    B.built = true;
    B.active = true;
    gForce = true;
    gLoggedApplied = false;
    logf("effects: volume created on layer(s) %s", layerText.c_str());
    return true;
}

void apply(const FxSettings& s) {
    bool any = s.exposureOn || s.satOn || s.contrastOn || s.hueOn || s.warmOn;
    setB(B.enabled, any, any);
    if (B.offActive) *fieldPtr<bool>(B.cg, B.offActive) = any;

    // postExposure works when the grading is HDR, brightness when it is LDR: both are set so it works either way
    setF(B.postExposure, s.exposure, s.exposureOn);
    float br = std::pow(2.f, s.exposure) * 100.f - 100.f;
    setF(B.brightness, std::min(100.f, std::max(-100.f, br)), s.exposureOn);
    setF(B.saturation, s.saturation, s.satOn);
    setF(B.contrast, s.contrast, s.contrastOn);
    setF(B.hueShift, s.hue, s.hueOn);
    setF(B.temperature, s.temperature, s.warmOn);
    setF(B.tint, s.tint, s.warmOn);

    if (!gLoggedApplied) {
        gLoggedApplied = true;
        logf("effects: applied (colour effects %s)", any ? "on" : "off");
    }
    gForce = false;
}

}  // namespace

void tick() {
    FxSettings s;
    bool changed = takeFx(s, gVersion);
    if (changed) {
        gCur = s;
        gForce = true;
    }

    if (!resolve()) return;
    int64_t now = nowMs();
    if (now < gRetryAtMs) return;

    if (!gCur.on) {
        if (gWasOn && B.built) setAllActive(false);
        gWasOn = false;
        return;
    }
    if (!gWasOn) {
        gWasOn = true;
        gForce = true;
    }

    if (anyGone()) {
        destroyAll();
        if (now < gNextBuildMs) return;
        gNextBuildMs = now + 1000;
        if (!build()) return;
    } else {
        if (!B.active) {
            setAllActive(true);
            gForce = true;
        }
        // the cameras can change after the volume was made (the player's camera shows up a moment after the scene loads):
        // look again now and then and make the volume again when they are not the same
        if (now >= gNextCheckMs) {
            gNextCheckMs = now + 1500;
            std::string sig = sigOf(collectMasks());
            if (!sig.empty() && sig != B.sig) {
                logf("effects: the cameras changed, making the volume again");
                for (Il2CppObject* g : B.gos) {
                    // hide the old one at once; Unity cleans it up when its scene goes
                    bool v = false;
                    void* a[1] = {&v};
                    invoke(C.mSetActive, g, a);
                }
                destroyAll();
                gNextBuildMs = 0;
                if (!build()) return;
            }
        }
    }

    if (gForce) apply(gCur);
}

void pollTouch() {
    if (!C.touchTried) {
        C.touchTried = true;
        Il2CppClass* k = findClass("UnityEngine", "Input");
        if (k) {
            C.mTouchCount = findMethod(k, "get_touchCount", 0);
            C.mGetTouch = findMethod(k, "GetTouch", 1);
        }
        if (!C.mTouchCount || !C.mGetTouch) logf("touch: UnityEngine.Input.touchCount / GetTouch not found, the menu cannot be used");
        else logf("touch: input found");
    }
    if (!C.mTouchCount || !C.mGetTouch) return;

    Touch t;
    bool ok = true;
    Il2CppObject* nb = invoke(C.mTouchCount, nullptr, nullptr, &ok);
    void* np = ok ? unbox(nb) : nullptr;
    int n = np ? *static_cast<int*>(np) : 0;
    if (n > 0) {
        int idx = 0;
        void* a[1] = {&idx};
        Il2CppObject* tb = invoke(C.mGetTouch, nullptr, a, &ok);
        char* p = ok ? static_cast<char*>(unbox(tb)) : nullptr;
        if (p) {
            // UnityEngine.Touch: m_FingerId @0, m_Position @4 (x, y), ..., m_Phase @36
            int phase = *reinterpret_cast<int*>(p + 36);
            t.valid = true;
            t.down = (phase != 3 && phase != 4);  // Ended, Canceled
            t.x = *reinterpret_cast<float*>(p + 4);
            t.y = *reinterpret_cast<float*>(p + 8);
        }
    }
    setTouch(t);
}

}  // namespace fx
}  // namespace vm
