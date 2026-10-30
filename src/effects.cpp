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
    // optional: the game build may have stripped them
    Il2CppClass *cMB = nullptr, *cBloom = nullptr, *cVig = nullptr, *cGrain = nullptr, *cChroma = nullptr, *cLens = nullptr, *cAO = nullptr, *cDof = nullptr;
    const MethodInfo* mMotionVectors = nullptr;

    const MethodInfo *mFindObjs = nullptr, *mDontDestroy = nullptr, *mCreateSO = nullptr, *mAddSettings = nullptr;
    const MethodInfo *mGoCtor = nullptr, *mAddComp = nullptr, *mSetLayer = nullptr, *mSetActive = nullptr, *mGetEnabled = nullptr;

    size_t offCachedPtr = 0x10;
    size_t offVolumeLayer = 0;
    size_t offIsGlobal = 0, offPriority = 0, offWeight = 0, offSharedProfile = 0;

    // touch
    bool touchTried = false;
    const MethodInfo *mTouchCount = nullptr, *mGetTouch = nullptr;
} C;

// one effect of the profile: its switch, its "active" flag and up to two values
struct Eff {
    Il2CppObject* obj = nullptr;
    size_t offActive = 0;
    Param enabled, a, b, c, d, e;
};

struct Built {
    bool built = false;
    Il2CppObject* profile = nullptr;
    Il2CppObject* cg = nullptr;
    std::vector<Il2CppObject*> gos;
    std::string sig;
    bool active = true;

    size_t offActive = 0;
    Param enabled, postExposure, brightness, saturation, contrast, hueShift, temperature, tint, gamma, colorFilter;
    Eff mb, bloom, vig, grain, chroma, lens, ao, dof;
    int regAo = -1, regDof = -1;   // is the effect known to the game's post-processing manager? 1 yes, 0 no, -1 unknown
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
    C.cMB = findClass(PP, "MotionBlur");
    C.cBloom = findClass(PP, "Bloom");
    C.cVig = findClass(PP, "Vignette");
    C.cGrain = findClass(PP, "Grain");
    C.cChroma = findClass(PP, "ChromaticAberration");
    C.cLens = findClass(PP, "LensDistortion");
    C.cAO = findClass(PP, "AmbientOcclusion");
    C.cDof = findClass(PP, "DepthOfField");
    C.mMotionVectors = findMethod(findClass("UnityEngine", "SystemInfo"), "get_supportsMotionVectors", 0);
    C.resolved = true;
    logf("effects: post-processing classes found%s%s%s%s%s%s%s%s", C.cMB ? "" : " (no MotionBlur)", C.cBloom ? "" : " (no Bloom)",
         C.cVig ? "" : " (no Vignette)", C.cGrain ? "" : " (no Grain)", C.cChroma ? "" : " (no ChromaticAberration)",
         C.cLens ? "" : " (no LensDistortion)", C.cAO ? "" : " (no AmbientOcclusion)", C.cDof ? "" : " (no DepthOfField)");
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

void setI(const Param& p, int v, bool ov) {
    if (!p.obj) return;
    *fieldPtr<int>(p.obj, p.offValue) = v;
    *fieldPtr<bool>(p.obj, p.offOverride) = ov;
}

void setV4(const Param& p, float x, float y, float z, float w, bool ov) {
    if (!p.obj) return;
    float* d = fieldPtr<float>(p.obj, p.offValue);  // Vector4 and Color are both four floats
    d[0] = x;
    d[1] = y;
    d[2] = z;
    d[3] = w;
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

// An extra effect that may be missing or fail without taking the rest down.
Eff makeEff(Il2CppClass* k, const char* name, const char* a, const char* b) {
    Eff r;
    if (!k) return r;
    Il2CppObject* o = newEffect(k);
    if (!o) {
        logf("effects: could not create %s", name);
        return r;
    }
    A.gchandle_new(o, 0);
    r.obj = o;
    r.offActive = fieldOffset(classOf(o), "active");
    r.enabled = getParam(o, "enabled");
    if (a) r.a = getParam(o, a);
    if (b) r.b = getParam(o, b);
    if (!r.enabled.obj || (a && !r.a.obj)) {
        logf("effects: %s has no usable fields, left out", name);
        r.obj = nullptr;
    }
    return r;
}

// The post-processing manager keeps a list of the effect types the build kept. A type that is not on that list can be
// added to a profile but is never drawn: the layer has nothing to run it with.
int registered(Il2CppClass* k) {
    if (!k) return 0;
    Il2CppClass* cm = findClass(PP, "PostProcessManager");
    if (!cm) return -1;
    const MethodInfo* mi = findMethod(cm, "get_instance", 0);
    if (!mi) return -1;
    Il2CppObject* mgr = invoke(mi, nullptr, nullptr);
    if (!mgr) return -1;
    size_t off = fieldOffset(cm, "settingsTypes");
    if (!off) return -1;
    Il2CppObject* dict = *fieldPtr<Il2CppObject*>(mgr, off);
    if (!dict) return -1;
    const MethodInfo* ck = findMethod(classOf(dict), "ContainsKey", 1);
    if (!ck) return -1;
    void* a[1] = {typeObject(k)};
    void* p = unbox(invoke(ck, dict, a));
    if (!p) return -1;
    return *static_cast<bool*>(p) ? 1 : 0;
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
    B.gamma = getParam(B.cg, "gamma");
    B.colorFilter = getParam(B.cg, "colorFilter");
    B.mb = makeEff(C.cMB, "MotionBlur", "shutterAngle", "sampleCount");
    B.bloom = makeEff(C.cBloom, "Bloom", "intensity", "threshold");
    B.vig = makeEff(C.cVig, "Vignette", "intensity", "smoothness");
    B.grain = makeEff(C.cGrain, "Grain", "intensity", "size");
    B.chroma = makeEff(C.cChroma, "ChromaticAberration", "intensity", nullptr);
    B.lens = makeEff(C.cLens, "LensDistortion", "intensity", nullptr);
    B.ao = makeEff(C.cAO, "AmbientOcclusion", "intensity", "radius");
    if (B.ao.obj) {
        B.ao.c = getParam(B.ao.obj, "quality");
        B.ao.d = getParam(B.ao.obj, "ambientOnly");
        B.ao.e = getParam(B.ao.obj, "mode");
    }
    B.dof = makeEff(C.cDof, "DepthOfField", "focusDistance", "aperture");
    if (B.dof.obj) {
        B.dof.c = getParam(B.dof.obj, "focalLength");
        B.dof.d = getParam(B.dof.obj, "kernelSize");
    }

    B.regAo = registered(C.cAO);
    B.regDof = registered(C.cDof);
    logf("effects: ambient occlusion: %s, known to the manager: %d | depth of field: %s, known to the manager: %d",
         B.ao.obj ? "created" : "missing", B.regAo, B.dof.obj ? "created" : "missing", B.regDof);

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

float clamp01(float v) { return std::min(1.f, std::max(0.f, v)); }
float clampf(float v, float lo, float hi) { return std::min(hi, std::max(lo, v)); }

// switch of one extra effect: enabled + active + overridden
void effSwitch(const Eff& f, bool on) {
    if (!f.obj) return;
    setB(f.enabled, on, on);
    if (f.offActive) *fieldPtr<bool>(f.obj, f.offActive) = on;
}

void apply(const FxSettings& s) {
    // ---- colour grading: only the switched-on values are overridden; the rest stays the game's
    bool any = s.exposureOn || s.satOn || s.contrastOn || s.hueOn || s.warmOn || s.filterOn || s.gammaOn;
    setB(B.enabled, any, any);
    if (B.offActive) *fieldPtr<bool>(B.cg, B.offActive) = any;

    // postExposure works when the grading is HDR, brightness when it is LDR: both are set so it works either way
    setF(B.postExposure, s.exposure, s.exposureOn);
    float br = std::pow(2.f, s.exposure) * 100.f - 100.f;
    setF(B.brightness, clampf(br, -100.f, 100.f), s.exposureOn);
    // gamma: the fourth number of the gamma wheel brightens (+) or darkens (-) the mid-tones
    setV4(B.gamma, 1.f, 1.f, 1.f, s.gamma, s.gammaOn);
    setF(B.saturation, s.saturation, s.satOn);
    setF(B.contrast, s.contrast, s.contrastOn);
    setF(B.hueShift, s.hue, s.hueOn);
    setF(B.temperature, s.temperature, s.warmOn);
    setF(B.tint, s.tint, s.warmOn);
    float k = clamp01(s.filterStrength);
    setV4(B.colorFilter, 1.f + (s.fr - 1.f) * k, 1.f + (s.fg - 1.f) * k, 1.f + (s.fb - 1.f) * k, 1.f, s.filterOn);

    // ---- the extra effects (each one only when the game has it)
    effSwitch(B.mb, s.blurOn);
    setF(B.mb.a, clampf(s.blurStrength, 0.f, 360.f), s.blurOn);
    setI(B.mb.b, std::min(32, std::max(4, s.blurSamples)), s.blurOn);
    effSwitch(B.bloom, s.bloomOn);
    setF(B.bloom.a, std::max(0.f, s.bloom), s.bloomOn);
    setF(B.bloom.b, std::max(0.f, s.bloomThreshold), s.bloomOn);
    effSwitch(B.vig, s.vignetteOn);
    setF(B.vig.a, clamp01(s.vignette), s.vignetteOn);
    setF(B.vig.b, clampf(s.vignetteSmooth, 0.01f, 1.f), s.vignetteOn);
    effSwitch(B.grain, s.grainOn);
    setF(B.grain.a, clamp01(s.grain), s.grainOn);
    setF(B.grain.b, clampf(s.grainSize, 0.3f, 3.f), s.grainOn);
    effSwitch(B.chroma, s.chromaOn);
    setF(B.chroma.a, clamp01(s.chroma), s.chromaOn);
    effSwitch(B.lens, s.lensOn);
    setF(B.lens.a, clampf(s.lens, -100.f, 100.f), s.lensOn);

    // ambient occlusion: mode 0 is the standard one (works without compute shaders); quality 1..3 = low..high
    effSwitch(B.ao, s.aoOn);
    setF(B.ao.a, clampf(s.aoIntensity, 0.f, 4.f), s.aoOn);
    setF(B.ao.b, clampf(s.aoRadius, 0.05f, 3.f), s.aoOn);
    setI(B.ao.c, 1 + std::min(2, std::max(0, s.aoQuality)), s.aoOn);
    setB(B.ao.d, false, s.aoOn);
    setI(B.ao.e, 0, s.aoOn);

    // depth of field: kernel size 0 small .. 2 large
    effSwitch(B.dof, s.dofOn);
    setF(B.dof.a, clampf(s.dofFocus, 0.1f, 50.f), s.dofOn);
    setF(B.dof.b, clampf(s.dofAperture, 1.f, 32.f), s.dofOn);
    setF(B.dof.c, clampf(s.dofLength, 10.f, 300.f), s.dofOn);
    setI(B.dof.d, std::min(2, std::max(0, s.dofQuality)), s.dofOn);

    if (!gLoggedApplied) {
        gLoggedApplied = true;
        logf("effects: applied (colour effects %s)", any ? "on" : "off");
    }
    gForce = false;
}

// the line under "Effects on" in the General tab
std::string buildStatus(const FxSettings& s) {
    std::string t = "Active";
    if (s.blurOn) {
        if (!B.mb.obj) t += "  |  motion blur: not in this game";
        else if (C.mMotionVectors && !callBool(C.mMotionVectors, nullptr, nullptr, true)) t += "  |  motion blur: not supported on this device";
        else t += "  |  motion blur on";
    }
    if (s.bloomOn && !B.bloom.obj) t += "  |  bloom: not in this game";
    if (s.vignetteOn && !B.vig.obj) t += "  |  vignette: not in this game";
    if (s.grainOn && !B.grain.obj) t += "  |  grain: not in this game";
    if (s.chromaOn && !B.chroma.obj) t += "  |  chromatic: not in this game";
    if (s.lensOn && !B.lens.obj) t += "  |  distortion: not in this game";
    if (s.aoOn && !B.ao.obj) t += "  |  ambient occlusion: not in this game";
    else if (s.aoOn && B.regAo == 0) t += "  |  ambient occlusion: this game cannot draw it";
    if (s.dofOn && !B.dof.obj) t += "  |  depth of field: not in this game";
    else if (s.dofOn && B.regDof == 0) t += "  |  depth of field: this game cannot draw it";
    return t;
}

}  // namespace

void tick() {
    FxSettings s;
    bool changed = takeFx(s, gVersion);
    if (changed) {
        gCur = s;
        gForce = true;
    }

    if (!gCur.on) {
        if (gWasOn && B.built) setAllActive(false);
        if (gWasOn) setVisualStatus("Off");
        gWasOn = false;
        return;
    }

    if (!resolve()) {
        setVisualStatus(C.gaveUp ? "Post-processing types not found" : "Waiting for the post-processing classes");
        return;
    }
    int64_t now = nowMs();
    if (now < gRetryAtMs) return;
    if (!gWasOn) {
        gWasOn = true;
        gForce = true;
    }

    if (anyGone()) {
        destroyAll();
        if (now < gNextBuildMs) return;
        gNextBuildMs = now + 1000;
        if (!build()) {
            setVisualStatus(gCur.noPostFx ? "Off while \"No post-processing\" is on" : "Waiting for the post-processing camera");
            return;
        }
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

    if (gForce) {
        apply(gCur);
        setVisualStatus(buildStatus(gCur));
    }
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
