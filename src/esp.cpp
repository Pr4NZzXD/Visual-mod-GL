#include "esp.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "il2cpp.h"
#include "log.h"

// ESP outlines. The game ships the "Outline" component (the QuickOutline asset, which it uses for its own item
// outline option): it draws a coloured edge around everything a renderer shows, in the shape of the object, and
// can do it through walls. This file only drives that component:
//   - every Outline in the scene is found; the ones under an enemy count as entities, the rest as items
//   - an enemy that has none gets one added (and removed again when the ESP is switched off)
//   - objects that carry the game's "OutlineCheck" (the item marker) get one too if they have none
//   - colour, thickness and "through walls" are set from the menu; what the game had is put back afterwards
// Nothing is drawn by the mod itself, so the outlines are exactly what the game's own outline option looks like.

namespace vm {
namespace esp {
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

const char* kEnemyClasses[] = {"AI_Granny",   "AI_Grandpa",   "AI_MomSpider", "AI_RobotEnemy", "AI_Slendrina",
                               "RatEnemy",    "SpectralGranny", "SunnyGrannyEnemy", "GrannyRobot"};

struct Fns {
    bool tried = false, ok = false;
    Il2CppClass *cOutline = nullptr, *cOutlineCheck = nullptr;
    std::vector<Il2CppClass*> enemies;
    const MethodInfo *mGetMode = nullptr, *mSetMode = nullptr, *mGetColor = nullptr, *mSetColor = nullptr, *mGetWidth = nullptr,
                     *mSetWidth = nullptr, *mEnGet = nullptr, *mEnSet = nullptr, *mGetGo = nullptr, *mGetTr = nullptr,
                     *mIsChildOf = nullptr, *mAddComp = nullptr, *mDestroy = nullptr;
} F;

bool resolve() {
    if (F.tried) return F.ok;
    F.tried = true;
    F.cOutline = findClass("", "Outline");
    F.cOutlineCheck = findClass("", "OutlineCheck");
    Il2CppClass* cComponent = findClass("UnityEngine", "Component");
    Il2CppClass* cGameObject = findClass("UnityEngine", "GameObject");
    Il2CppClass* cObject = findClass("UnityEngine", "Object");
    Il2CppClass* cBehaviour = findClass("UnityEngine", "Behaviour");
    Il2CppClass* cTransform = findClass("UnityEngine", "Transform");
    for (const char* n : kEnemyClasses)
        if (Il2CppClass* k = findClass("", n)) F.enemies.push_back(k);
    if (!F.cOutline || !cComponent || !cGameObject || !cObject || !cBehaviour || !cTransform) {
        logf("esp: the game has no Outline component here, the ESP is not available");
        return false;
    }
    F.mGetMode = findMethod(F.cOutline, "get_OutlineMode", 0);
    F.mSetMode = findMethod(F.cOutline, "set_OutlineMode", 1);
    F.mGetColor = findMethod(F.cOutline, "get_OutlineColor", 0);
    F.mSetColor = findMethod(F.cOutline, "set_OutlineColor", 1);
    F.mGetWidth = findMethod(F.cOutline, "get_OutlineWidth", 0);
    F.mSetWidth = findMethod(F.cOutline, "set_OutlineWidth", 1);
    F.mEnGet = findMethod(cBehaviour, "get_enabled", 0);
    F.mEnSet = findMethod(cBehaviour, "set_enabled", 1);
    F.mGetGo = findMethod(cComponent, "get_gameObject", 0);
    F.mGetTr = findMethod(cComponent, "get_transform", 0);
    F.mIsChildOf = findMethod(cTransform, "IsChildOf", 1);
    F.mAddComp = findMethod(cGameObject, "AddComponent", 1, "System.Type");
    F.mDestroy = findMethod(cObject, "Destroy", 1, "UnityEngine.Object");
    F.ok = F.mGetMode && F.mSetMode && F.mGetColor && F.mSetColor && F.mGetWidth && F.mSetWidth && F.mEnGet && F.mEnSet &&
           F.mGetGo && F.mGetTr && F.mIsChildOf;
    logf("esp: %s (%d enemy classes%s%s)", F.ok ? "ready" : "some Outline functions are missing, the ESP is not available",
         (int)F.enemies.size(), F.mAddComp ? "" : ", cannot add outlines", F.cOutlineCheck ? ", item marker found" : "");
    return F.ok;
}

// ---------------------------------------------------------------- one Outline we know about
struct Entry {
    Held h;
    Il2CppObject* go = nullptr;  // only for the ones we added
    bool ours = false;           // added by the mod: removed again when the ESP is off
    int cls = 0;                 // 0 item, 1 entity
    bool applied = false;
    int origMode = 0;
    float origColor[4] = {1, 1, 1, 1};
    float origWidth = 2.f;
    bool origEnabled = true;
};
std::unordered_map<Il2CppObject*, Entry> gE;
std::unordered_set<Il2CppObject*> gAdded;  // game objects we added an Outline to

int getMode(Il2CppObject* o) { return callInt(F.mGetMode, o, nullptr, 0); }
float getWidth(Il2CppObject* o) { return callFloat(F.mGetWidth, o, nullptr, 2.f); }
bool getEnabled(Il2CppObject* o) { return callBool(F.mEnGet, o, nullptr, true); }
void getColor(Il2CppObject* o, float* out) {
    bool ok = true;
    void* p = unbox(invoke(F.mGetColor, o, nullptr, &ok));
    if (ok && p) memcpy(out, p, 16);
}
void setMode(Il2CppObject* o, int v) {
    void* a[1] = {&v};
    invoke(F.mSetMode, o, a);
}
void setWidth(Il2CppObject* o, float v) {
    void* a[1] = {&v};
    invoke(F.mSetWidth, o, a);
}
void setEnabled(Il2CppObject* o, bool v) {
    void* a[1] = {&v};
    invoke(F.mEnSet, o, a);
}
void setColor(Il2CppObject* o, const float* c) {
    float v[4] = {c[0], c[1], c[2], 1.f};
    void* a[1] = {v};
    invoke(F.mSetColor, o, a);
}

bool near4(const float* a, const float* b) {
    return std::fabs(a[0] - b[0]) < 0.01f && std::fabs(a[1] - b[1]) < 0.01f && std::fabs(a[2] - b[2]) < 0.01f;
}

void track(Il2CppObject* o, int cls, bool ours, Il2CppObject* go) {
    Entry e;
    e.h = hold(o);
    e.ours = ours;
    e.go = go;
    e.cls = cls;
    e.origMode = getMode(o);
    getColor(o, e.origColor);
    e.origWidth = getWidth(o);
    e.origEnabled = getEnabled(o);
    gE[o] = e;
}

// ---------------------------------------------------------------- finding what to outline
struct Enemy {
    Il2CppObject* comp;
    Il2CppObject* tr;
    Il2CppObject* go;
};

bool isUnder(Il2CppObject* tr, const std::vector<Enemy>& en) {
    for (const Enemy& e : en) {
        void* a[1] = {e.tr};
        if (callBool(F.mIsChildOf, tr, a, false)) return true;
    }
    return false;
}

void scan(const FxSettings& s) {
    std::vector<Il2CppObject*> keep;  // the managed arrays stay referenced while we use their elements
    std::vector<Enemy> enemies;
    for (Il2CppClass* k : F.enemies) {
        Il2CppObject* arr = findObjectsOfType(k);
        if (!arr) continue;
        keep.push_back(arr);
        size_t n = A.array_length(arr);
        Il2CppObject** el = arrayData(arr);
        for (size_t i = 0; i < n && i < 32; i++) {
            Il2CppObject* c = el[i];
            if (!c || !alive(c)) continue;
            Enemy e;
            e.comp = c;
            e.tr = invoke(F.mGetTr, c, nullptr);
            e.go = invoke(F.mGetGo, c, nullptr);
            if (e.tr && e.go) enemies.push_back(e);
        }
    }

    // the Outlines that exist: under an enemy -> entity, otherwise item
    std::unordered_set<Il2CppObject*> outlineGos;
    std::vector<bool> enemyHas(enemies.size(), false);
    Il2CppObject* outArr = findObjectsOfType(F.cOutline);
    if (outArr) {
        keep.push_back(outArr);
        size_t n = A.array_length(outArr);
        Il2CppObject** el = arrayData(outArr);
        for (size_t i = 0; i < n && i < 1024; i++) {
            Il2CppObject* o = el[i];
            if (!o || !alive(o)) continue;
            Il2CppObject* go = invoke(F.mGetGo, o, nullptr);
            Il2CppObject* tr = invoke(F.mGetTr, o, nullptr);
            if (go) outlineGos.insert(go);
            int cls = 0;
            for (size_t k = 0; k < enemies.size(); k++) {
                void* a[1] = {enemies[k].tr};
                if (tr && callBool(F.mIsChildOf, tr, a, false)) {
                    cls = 1;
                    enemyHas[k] = true;
                }
            }
            auto it = gE.find(o);
            if (it == gE.end()) track(o, cls, false, nullptr);
            else it->second.cls = cls;
        }
    }

    // an enemy without an outline gets one
    if (s.espEnemies && F.mAddComp) {
        Il2CppObject* type = typeObject(F.cOutline);
        std::unordered_set<Il2CppObject*> doneGo;
        for (size_t k = 0; k < enemies.size(); k++) {
            if (enemyHas[k] || !doneGo.insert(enemies[k].go).second || gAdded.count(enemies[k].go)) continue;
            void* a[1] = {type};
            Il2CppObject* comp = invoke(F.mAddComp, enemies[k].go, a);
            gAdded.insert(enemies[k].go);  // tried once; not again, even if it failed
            if (comp) track(comp, 1, true, enemies[k].go);
        }
    }

    // item markers without an outline get one
    if (s.espItems && F.mAddComp && F.cOutlineCheck) {
        Il2CppObject* type = typeObject(F.cOutline);
        Il2CppObject* arr = findObjectsOfType(F.cOutlineCheck);
        if (arr) {
            keep.push_back(arr);
            size_t n = A.array_length(arr);
            Il2CppObject** el = arrayData(arr);
            for (size_t i = 0; i < n && i < 512; i++) {
                Il2CppObject* c = el[i];
                if (!c || !alive(c)) continue;
                Il2CppObject* go = invoke(F.mGetGo, c, nullptr);
                Il2CppObject* tr = invoke(F.mGetTr, c, nullptr);
                if (!go || outlineGos.count(go) || gAdded.count(go) || (tr && isUnder(tr, enemies))) continue;
                void* a[1] = {type};
                Il2CppObject* comp = invoke(F.mAddComp, go, a);
                gAdded.insert(go);
                if (comp) track(comp, 0, true, go);
            }
        }
    }
    for (Il2CppObject* p : keep) keepAlive(p);
}

// ---------------------------------------------------------------- applying the menu settings
void applyAll(const FxSettings& s, bool verify) {
    for (auto it = gE.begin(); it != gE.end();) {
        Il2CppObject* o = it->first;
        Entry& e = it->second;
        if (!alive(o)) {
            if (e.go) gAdded.erase(e.go);
            drop(e.h);
            it = gE.erase(it);
            continue;
        }
        bool want = e.cls == 0 ? s.espItems : s.espEnemies;
        if (want) {
            if (!e.applied || verify) {
                int mode = s.espWalls ? 0 : 1;  // OutlineAll / OutlineVisible
                int ci = e.cls == 0 ? s.espItemColor : s.espEnemyColor;
                const float* c = kEspColors[ci < 0 ? 0 : (ci > 7 ? 7 : ci)];
                float w = e.cls == 0 ? s.espItemWidth : s.espEnemyWidth;
                float cur[4] = {1, 1, 1, 1};
                getColor(o, cur);
                if (!getEnabled(o)) setEnabled(o, true);
                if (getMode(o) != mode) setMode(o, mode);
                if (!near4(cur, c)) setColor(o, c);
                if (std::fabs(getWidth(o) - w) > 0.05f) setWidth(o, w);
                e.applied = true;
            }
            ++it;
        } else if (e.applied || e.ours) {
            if (e.ours) {  // our own: take it off again (OnDestroy gives the materials back)
                if (F.mDestroy) {
                    void* a[1] = {o};
                    invoke(F.mDestroy, nullptr, a);
                } else {
                    setEnabled(o, false);
                }
                if (e.go) gAdded.erase(e.go);
            } else {
                setMode(o, e.origMode);
                setColor(o, e.origColor);
                setWidth(o, e.origWidth);
                setEnabled(o, e.origEnabled);
            }
            drop(e.h);
            it = gE.erase(it);
        } else {
            drop(e.h);  // not ours and never changed: nothing to remember
            it = gE.erase(it);
        }
    }
}

int64_t gApplyNext = 0, gVerifyNext = 0, gScanNext = 0, gStatusNext = 0;
int gLastItems = -1, gLastEnemies = -1, gLastAdded = -1;

void status(const FxSettings& s, int64_t now) {
    if (now < gStatusNext) return;
    gStatusNext = now + 500;
    int items = 0, enemies = 0, added = 0;
    for (auto& kv : gE) {
        if (!kv.second.applied) continue;
        (kv.second.cls == 0 ? items : enemies)++;
        if (kv.second.ours) added++;
    }
    std::string t;
    if (s.espItems) t += "Items outlined: " + std::to_string(items) + (items ? "" : "  (none found in this scene)") + "\n";
    if (s.espEnemies) t += "Entities outlined: " + std::to_string(enemies) + (enemies ? "" : "  (none found yet)") + "\n";
    if (!t.empty()) t.pop_back();
    setEspStatus(t);
    if (items != gLastItems || enemies != gLastEnemies || added != gLastAdded) {
        gLastItems = items;
        gLastEnemies = enemies;
        gLastAdded = added;
        if (items || enemies) logf("esp: %d items, %d entities outlined (%d outlines added by the mod)", items, enemies, added);
    }
}

}  // namespace

void tick(const FxSettings& s) {
    bool any = s.espItems || s.espEnemies;
    if (!any && gE.empty()) {
        if (gLastItems != -1) {
            gLastItems = gLastEnemies = gLastAdded = -1;
            setEspStatus("");
        }
        return;
    }
    if (!resolve()) return;
    int64_t now = nowMs();
    if (any && now >= gScanNext) {  // the scene is looked through once a second
        gScanNext = now + 1000;
        scan(s);
    }
    if (now >= gApplyNext) {
        gApplyNext = now + 100;
        bool verify = now >= gVerifyNext;  // the game's own outline option may reset what was set
        if (verify) gVerifyNext = now + 500;
        applyAll(s, verify);
    }
    status(s, now);
}

}  // namespace esp
}  // namespace vm
