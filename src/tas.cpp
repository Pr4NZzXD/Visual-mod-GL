#include "tas.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "il2cpp.h"
#include "log.h"

// The TAS tab: time scale (speedhack) with audio that follows it, the item splits, restart, skipping of the bed and
// ending cutscenes. Nothing here uses a fixed address: classes and methods are found by name. Every Unity setting that
// is changed is put back when the option is switched off.

namespace vm {
namespace tas {
namespace {

using namespace il2;

double nowS() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---------------------------------------------------------------- what Unity offers
struct U {
    bool tried = false;
    float (*tsGet)() = nullptr;
    void (*tsSet)(float) = nullptr;
    Il2CppClass* cAudio = nullptr;
    float (*apGet)(void*) = nullptr;
    void (*apSet)(void*, float) = nullptr;
    const MethodInfo *mApGet = nullptr, *mApSet = nullptr;
    const MethodInfo *mActiveScene = nullptr, *mBuildIdx = nullptr, *mNameOf = nullptr, *mLoadInt = nullptr, *mLoadStr = nullptr;
} u;

void resolveUnity() {
    if (u.tried) return;
    u.tried = true;
    u.tsGet = reinterpret_cast<float (*)()>(icall("UnityEngine.Time::get_timeScale"));
    u.tsSet = reinterpret_cast<void (*)(float)>(icall("UnityEngine.Time::set_timeScale"));
    u.cAudio = findClass("UnityEngine", "AudioSource");
    u.apGet = reinterpret_cast<float (*)(void*)>(icall("UnityEngine.AudioSource::get_pitch"));
    u.apSet = reinterpret_cast<void (*)(void*, float)>(icall("UnityEngine.AudioSource::set_pitch"));
    if (u.cAudio) {
        u.mApGet = findMethod(u.cAudio, "get_pitch", 0);
        u.mApSet = findMethod(u.cAudio, "set_pitch", 1);
    }
    Il2CppClass* cMgr = findClass("UnityEngine.SceneManagement", "SceneManager");
    Il2CppClass* cScene = findClass("UnityEngine.SceneManagement", "Scene");
    if (cMgr) {
        u.mActiveScene = findMethod(cMgr, "GetActiveScene", 0);
        u.mLoadInt = findMethod(cMgr, "LoadScene", 1, "System.Int32");
        u.mLoadStr = findMethod(cMgr, "LoadScene", 1, "System.String");
    }
    if (cScene) {
        u.mBuildIdx = findMethod(cScene, "GetBuildIndexInternal", 1);
        u.mNameOf = findMethod(cScene, "GetNameInternal", 1);
    }
    logf("tas: time scale %s, audio pitch %s, restart %s", (u.tsGet && u.tsSet) ? "ok" : "MISSING",
         ((u.apGet && u.apSet) || (u.mApGet && u.mApSet)) ? "ok" : "MISSING",
         (u.mActiveScene && (u.mLoadInt || u.mLoadStr)) ? "ok" : "MISSING");
}

// ---------------------------------------------------------------- the settings the hooks look at
TasSettings S;           // the newest settings (main thread only)
double gFfUntil = 0.0;   // fast-forward through a cutscene until this time (real seconds)
float gFfSpeed = 1.f;

// ---------------------------------------------------------------- time scale
bool gTsApplied = false;
float gTsOrig = 1.f, gTsLast = 1.f;

void timeTick(double now) {
    if (!u.tsGet || !u.tsSet) return;
    float want = -1.f;
    if (now < gFfUntil) want = gFfSpeed;
    else if (S.speedOn) want = clampf(S.speed, 0.01f, 5.f);
    float cur = u.tsGet();
    if (want > 0.f) {
        if (!gTsApplied) {
            gTsOrig = cur > 0.001f ? cur : 1.f;
            gTsApplied = true;
        }
        // a time scale of 0 is the game's pause: leave it alone
        if (cur > 0.0001f && std::fabs(cur - want) > 0.0005f) u.tsSet(want);
        gTsLast = want;
    } else if (gTsApplied) {
        if (cur > 0.0001f && std::fabs(cur - gTsLast) < 0.01f) u.tsSet(gTsOrig > 0.001f ? gTsOrig : 1.f);
        gTsApplied = false;
    }
}

// ---------------------------------------------------------------- audio follows the time scale
struct AState {
    float base = 1.f, applied = 1.f;
    bool skip = false;  // the game keeps this source in step with the time scale itself: leave it alone
};
std::unordered_map<void*, AState> gAudio;
bool gAudioActive = false;
float gAudioScale = 1.f;
double gAudioNext = 0.0;

float getPitch(void* a) {
    if (u.apGet) return u.apGet(a);
    return callFloat(u.mApGet, a, nullptr, 1.f);
}
void setPitch(void* a, float v) {
    if (u.apSet) {
        u.apSet(a, v);
        return;
    }
    if (u.mApSet) {
        void* args[1] = {&v};
        invoke(u.mApSet, a, args);
    }
}

// Some sounds are already in step: the game sets their pitch to Time.timeScale itself. Scaling those twice
// would make them far too slow, so they are left alone.
bool gameSynced(float cur, float s) {
    float ts = u.tsGet ? u.tsGet() : 1.f;
    if (std::fabs(ts - 1.f) < 0.01f && std::fabs(s - 1.f) < 0.01f) return false;
    return std::fabs(cur - clampf(ts, 0.05f, 3.f)) < 0.03f || std::fabs(cur - clampf(s, 0.05f, 3.f)) < 0.03f;
}

void applyOne(void* src, float s) {
    float cur = getPitch(src);
    auto it = gAudio.find(src);
    if (it == gAudio.end()) {
        AState st;
        st.base = st.applied = cur;
        if (gameSynced(cur, s)) st.skip = true;
        it = gAudio.emplace(src, st).first;
        if (st.skip) return;
    }
    AState& st = it->second;
    if (st.skip) {
        if (gameSynced(cur, s)) return;
        st.skip = false;
        st.base = st.applied = cur;
    } else if (std::fabs(cur - st.applied) > 0.001f) {
        st.base = cur;  // the game changed the pitch itself: that is the new original
    }
    float target = clampf(st.base * s, -3.f, 3.f);
    if (std::fabs(target - cur) > 0.0005f) setPitch(src, target);
    st.applied = target;
}

void sweepAudio(float s, bool apply) {
    if (!u.cAudio) return;
    Il2CppObject* arr = findObjectsOfType(u.cAudio);
    if (!arr) {
        if (!apply) gAudio.clear();
        return;
    }
    size_t n = A.array_length(arr);
    Il2CppObject** d = arrayData(arr);
    size_t seen = 0;
    for (size_t i = 0; i < n; i++) {
        Il2CppObject* o = d[i];
        if (!o || !alive(o)) continue;
        seen++;
        if (apply) {
            applyOne(o, s);
        } else {
            auto it = gAudio.find(o);
            if (it != gAudio.end() && !it->second.skip) setPitch(o, it->second.base);
        }
    }
    if (!apply) {
        gAudio.clear();
        return;
    }
    if (gAudio.size() > seen + 8) {  // sources that are gone
        std::unordered_map<void*, AState> keep;
        for (size_t i = 0; i < n; i++) {
            auto it = gAudio.find(d[i]);
            if (d[i] && it != gAudio.end()) keep.emplace(d[i], it->second);
        }
        gAudio.swap(keep);
    }
}

void audioTick(double now) {
    if (!u.cAudio || !((u.apGet && u.apSet) || (u.mApGet && u.mApSet))) return;
    float cur = u.tsGet ? u.tsGet() : 1.f;
    // follow the real time scale, but not a pause (0)
    float scale = cur > 0.0001f ? cur : gAudioScale;
    bool want = S.audioSync && std::fabs(scale - 1.f) > 0.001f;
    if (!want && !gAudioActive) return;
    float s = want ? clampf(scale, 0.05f, 3.f) : 1.f;
    bool changed = want != gAudioActive || std::fabs(s - gAudioScale) > 0.0005f;
    if (!changed && (!want || now < gAudioNext)) return;
    gAudioNext = now + 0.08;  // every source is looked at about 12 times a second: short sounds are made on the spot
    gAudioScale = s;
    gAudioActive = want;
    sweepAudio(s, want);
}

// ---------------------------------------------------------------- scene changes and restart
int gScene = -999999;
bool gSceneChanged = false;
double gSceneNext = 0.0;

int activeSceneHandle() {
    if (!u.mActiveScene) return gScene;
    void* p = unbox(invoke(u.mActiveScene, nullptr, nullptr));
    return p ? *static_cast<int*>(p) : gScene;
}

void restartScene() {
    resolveUnity();
    if (!u.mActiveScene) {
        postToast("Restart: the scene functions were not found");
        logf("tas: restart: SceneManager.GetActiveScene not found");
        return;
    }
    int handle = activeSceneHandle();
    int index = -1;
    if (u.mBuildIdx) {
        void* args[1] = {&handle};
        index = callInt(u.mBuildIdx, nullptr, args, -1);
    }
    if (index >= 0 && u.mLoadInt) {
        void* args[1] = {&index};
        bool ok = true;
        invoke(u.mLoadInt, nullptr, args, &ok);
        logf("tas: restart: LoadScene(%d) %s", index, ok ? "called" : "FAILED");
        postToast(ok ? "Restarting..." : "Restart failed");
        return;
    }
    if (u.mNameOf && u.mLoadStr) {
        void* args[1] = {&handle};
        Il2CppString* name = reinterpret_cast<Il2CppString*>(invoke(u.mNameOf, nullptr, args));
        if (name) {
            void* a2[1] = {name};
            bool ok = true;
            invoke(u.mLoadStr, nullptr, a2, &ok);
            logf("tas: restart: LoadScene(\"%s\") %s", str(name).c_str(), ok ? "called" : "FAILED");
            postToast(ok ? "Restarting..." : "Restart failed");
            return;
        }
    }
    logf("tas: restart: no way to load the scene again");
    postToast("Restart is not available in this game build");
}

// ---------------------------------------------------------------- the splits
struct Split {
    std::string name;
    double t = -1.0;  // seconds since the timer started; < 0 = not yet
};
std::vector<Split> gRoute;
bool gRouteLoaded = false;
bool gEscapeDone = false;
double gEscapeT = -1.0;
double gTimerStart = 0.0, gTimerStop = -1.0;  // gTimerStop >= 0: the timer is stopped (escaped)
std::vector<std::string> gPicks;              // names the hook saw, handled in tick
int gEscapeEvents = 0;
std::string gEscapeName;

std::string routePath() {
    std::string d = dataDir() ? dataDir() : "";
    return d.empty() ? "" : d + "/visualmobile.splits";
}

void routeSave() {
    std::string p = routePath();
    if (p.empty()) return;
    FILE* f = fopen(p.c_str(), "w");
    if (!f) return;
    for (const Split& s : gRoute) fprintf(f, "%s\n", s.name.c_str());
    fclose(f);
}

void routeLoad() {
    if (gRouteLoaded) return;
    gRouteLoaded = true;
    std::string p = routePath();
    FILE* f = p.empty() ? nullptr : fopen(p.c_str(), "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (n) {
            Split s;
            s.name = line;
            gRoute.push_back(s);
        }
    }
    fclose(f);
    logf("tas: %d items in the split list", (int)gRoute.size());
}

// "SpiderKey(Clone)" -> "Spider"; "padlock_key" -> "Padlock key"
std::string pretty(std::string n) {
    size_t c = n.find("(Clone)");
    if (c != std::string::npos) n.erase(c);
    std::string o;
    for (size_t i = 0; i < n.size(); i++) {
        char ch = n[i];
        if (ch == '_') ch = ' ';
        if (i > 0 && std::isupper((unsigned char)ch) && std::islower((unsigned char)n[i - 1])) o += ' ';
        o += ch;
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    while (!o.empty() && o.front() == ' ') o.erase(o.begin());
    std::string low;
    for (char ch : o) low += (char)std::tolower((unsigned char)ch);
    if (low == "spider key") return "Spider";
    if (!o.empty()) o[0] = (char)std::toupper((unsigned char)o[0]);
    return o.empty() ? "Item" : o;
}

void resetTimes(double now) {
    for (Split& s : gRoute) s.t = -1.0;
    gEscapeDone = false;
    gEscapeT = -1.0;
    gTimerStart = now;
    gTimerStop = -1.0;
}

void handlePicks(double now) {
    if (gPicks.empty()) return;
    std::vector<std::string> picks;
    picks.swap(gPicks);
    for (const std::string& raw : picks) {
        if (gTimerStop >= 0.0) continue;  // the run is over
        std::string name = pretty(raw);
        double t = now - gTimerStart;
        bool found = false;
        for (Split& s : gRoute) {
            if (s.name == name) {
                found = true;
                if (s.t < 0.0) {
                    s.t = t;
                    logf("tas: split %s %.2f", name.c_str(), t);
                }
                break;
            }
        }
        if (!found && S.autoList) {
            Split s;
            s.name = name;
            s.t = t;
            gRoute.push_back(s);
            routeSave();
            logf("tas: split %s %.2f (added to the list)", name.c_str(), t);
        }
    }
}

void handleEscape(double now) {
    if (!gEscapeEvents) return;
    gEscapeEvents = 0;
    if (gEscapeDone) return;
    gEscapeDone = true;
    gEscapeT = now - gTimerStart;
    gTimerStop = now;
    logf("tas: escape %s at %.2f", gEscapeName.c_str(), gEscapeT);
    if (S.skipEnding) {  // play the ending cutscene fast
        gFfUntil = now + 40.0;
        gFfSpeed = 10.f;
        logf("tas: skipping the ending cutscene (x10)");
    }
}

double gPublishNext = 0.0;

void publishSplits(double now, bool force) {
    if (!force && now < gPublishNext) return;
    gPublishNext = now + 0.1;
    SplitsView v;
    for (const Split& s : gRoute) {
        SplitRow r;
        r.name = s.name;
        r.seconds = s.t;
        v.rows.push_back(r);
    }
    if (S.escapeSplit) {
        SplitRow r;
        r.name = "Escape";
        r.seconds = gEscapeT;
        v.rows.push_back(r);
    }
    v.running = gTimerStop < 0.0;
    v.timer = (gTimerStop >= 0.0 ? gTimerStop : now) - gTimerStart;
    setSplitsView(v);
}

// ---------------------------------------------------------------- hooks
using Fn3 = uint64_t (*)(void*, void*, void*);
Fn3 oPick = nullptr, oEscDoor = nullptr, oEscCar = nullptr, oEscCellar = nullptr, oEscRobo = nullptr, oBedG = nullptr, oBedP = nullptr;
bool gPickIsString = true;

void onPick(void* arg) {
    if (!arg) return;
    std::string name;
    if (gPickIsString) {
        name = str(reinterpret_cast<Il2CppString*>(arg));
    } else {
        Il2CppObject* o = reinterpret_cast<Il2CppObject*>(arg);
        Il2CppClass* k = classOf(o);
        size_t off = k ? fieldOffset(k, "itemName") : 0;
        if (off) name = str(*fieldPtr<Il2CppString*>(o, off));
    }
    if (name.empty()) return;
    if (gPicks.size() < 64) gPicks.push_back(name);
}

uint64_t hPick(void* a, void* b, void* c) {
    uint64_t r = oPick ? oPick(a, b, c) : 0;
    onPick(b);
    return r;
}

#define ESCAPE_HOOK(FN, ORIG, LABEL)                                \
    uint64_t FN(void* a, void* b, void* c) {                        \
        uint64_t r = ORIG ? ORIG(a, b, c) : 0;                      \
        gEscapeEvents++;                                            \
        gEscapeName = LABEL;                                        \
        return r;                                                   \
    }
ESCAPE_HOOK(hEscDoor, oEscDoor, "door")
ESCAPE_HOOK(hEscCar, oEscCar, "car")
ESCAPE_HOOK(hEscCellar, oEscCellar, "cellar")
ESCAPE_HOOK(hEscRobo, oEscRobo, "robot")

// being caught while hiding in bed starts a cutscene: with the option on it is played fast
void bedCaught() {
    if (!S.skipBed) return;
    gFfUntil = nowS() + 8.0;
    gFfSpeed = 8.f;
    logf("tas: skipping the bed cutscene (x8)");
}
uint64_t hBedG(void* a, void* b, void* c) {
    bedCaught();
    return oBedG ? oBedG(a, b, c) : 0;
}
uint64_t hBedP(void* a, void* b, void* c) {
    bedCaught();
    return oBedP ? oBedP(a, b, c) : 0;
}

std::string gStatus = "TAS: waiting for the game to start.";
std::vector<std::string> gFailed;
int gHookOk = 0, gHookTotal = 0;

}  // namespace

void install(HookInstaller hook) {
    resolveUnity();
    routeLoad();

    auto H = [&](const char* cls, const char* method, int argc, void* repl, void** orig) {
        gHookTotal++;
        std::string what = std::string(cls) + "." + method;
        Il2CppClass* k = findClass("", cls);
        const MethodInfo* m = k ? findMethod(k, method, argc) : nullptr;
        if (!m) {
            logf("tas: %s not found", what.c_str());
            gFailed.push_back(what);
            return;
        }
        if (!hook(fnPtr(m), repl, orig, what.c_str())) {
            gFailed.push_back(what);
            return;
        }
        gHookOk++;
    };

    // what is picked up: the argument can be the item's name or an item definition
    {
        Il2CppClass* k = findClass("", "Inventory");
        const MethodInfo* m = k ? findMethod(k, "PickupItem", 1) : nullptr;
        if (m && A.method_get_param) {
            const Il2CppType* pt = A.method_get_param(m, 0);
            int tt = pt ? A.type_get_type(pt) : 0x0e;
            gPickIsString = tt == 0x0e;
            char* tn = pt ? A.type_get_name(pt) : nullptr;
            logf("tas: Inventory.PickupItem(%s)", tn ? tn : "?");
        }
    }
    H("Inventory", "PickupItem", 1, (void*)hPick, (void**)&oPick);
    H("Escapes", "EscapeDoor", 0, (void*)hEscDoor, (void**)&oEscDoor);
    H("Escapes", "EscapeCar", 0, (void*)hEscCar, (void**)&oEscCar);
    H("Escapes", "EscapeCellar", 0, (void*)hEscCellar, (void**)&oEscCellar);
    H("Escapes", "EscapeRobo", 0, (void*)hEscRobo, (void**)&oEscRobo);
    H("PlayerStatus", "GrannyCaughtYouBed", 0, (void*)hBedG, (void**)&oBedG);
    H("PlayerStatus", "GrandpaCaughtYouBed", 0, (void*)hBedP, (void**)&oBedP);

    gStatus = "TAS: " + std::to_string(gHookOk) + " of " + std::to_string(gHookTotal) + " hooks ready.";
    if (!gFailed.empty()) {
        gStatus += " Missing:";
        for (const std::string& f : gFailed) gStatus += " " + f;
    }
    setTasStatus(gStatus);
    logf("%s", gStatus.c_str());
}

void tick(const TasSettings& s) {
    S = s;
    resolveUnity();
    double now = nowS();

    static bool first = true;
    static int lastRestart = 0, lastReset = 0, lastClear = 0;
    if (first) {
        first = false;
        lastRestart = s.restartReq;
        lastReset = s.resetSplitsReq;
        lastClear = s.clearListReq;
        gTimerStart = now;
    }

    // a new scene: the timer starts again, a fast-forward ends
    if (now >= gSceneNext) {
        gSceneNext = now + 0.25;
        int h = activeSceneHandle();
        if (h != gScene) {
            bool firstScene = gScene == -999999;
            gScene = h;
            if (!firstScene) {
                logf("tas: scene changed, the timer starts again");
                if (gAudioActive) sweepAudio(1.f, false);
                gAudio.clear();
                gAudioNext = 0.0;
                gFfUntil = 0.0;
                resetTimes(now);
                gPicks.clear();
                gEscapeEvents = 0;
                publishSplits(now, true);
            }
        }
    }

    if (s.restartReq != lastRestart) {
        lastRestart = s.restartReq;
        restartScene();
    }
    if (s.resetSplitsReq != lastReset) {
        lastReset = s.resetSplitsReq;
        resetTimes(now);
        gPicks.clear();
        publishSplits(now, true);
        postToast("Splits reset, timer started");
    }
    if (s.clearListReq != lastClear) {
        lastClear = s.clearListReq;
        gRoute.clear();
        routeSave();
        resetTimes(now);
        publishSplits(now, true);
        postToast("Split list cleared");
    }

    handlePicks(now);
    handleEscape(now);
    timeTick(now);
    audioTick(now);
    publishSplits(now, false);

    // the line at the top of the tab
    static double statusNext = 0.0;
    if (now >= statusNext) {
        statusNext = now + 0.5;
        std::string t = gStatus;
        char buf[96];
        if (s.speedOn) {
            snprintf(buf, sizeof(buf), "\nTime scale now %.2f", u.tsGet ? u.tsGet() : 0.f);
            t += buf;
        }
        if (now < gFfUntil) t += "\nFast-forwarding a cutscene...";
        setTasStatus(t);
    }
}

}  // namespace tas
}  // namespace vm
