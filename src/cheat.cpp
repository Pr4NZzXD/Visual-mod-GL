#include "cheat.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <vector>

#include "il2cpp.h"
#include "log.h"

// The Cheat tab, game side.
//
// Nothing here is a fixed address. Classes, fields and methods are looked up by name when the game starts (the names
// were checked against the dump of this build), and every field is checked for the type the code expects before it is
// ever written. A name that is missing or has another type turns off only the feature that needs it, with a line in the
// log ("cheat: ..."), the rest keeps working.
//
// How the features work (same ideas as the PC cheat, which was written against the same C# code):
//   God mode        the game's death / "caught you" methods of PlayerStatus are skipped (the enemy forgets you instead)
//   Speed           CharacterController.Move is scaled sideways, for the player's own controller only
//   No clip         the player's CharacterController is switched off and the game's own Move call is turned into a plain
//                   change of position (height follows where the camera looks); falling is suppressed meanwhile
//   No fall damage  the fall timer of FallingHolder is kept short
//   No cracking     TriggerCreakNoise.OnTriggerEnter is skipped
//   No trap         the OnTriggerEnter of the four trap classes is skipped
//   Infinite ammo / spray   the amount is put back to the highest value seen
//   Enemies         every enemy class is hooked at its own update: freeze (the update is skipped, the NavMeshAgent is
//                   stopped and the position pinned), blind, deaf, kill
//   Items           spawn (the game's own dropper prefab), auto grab (presses the interact button while an item is in
//                   front of you), long reach (the ray length of PickRay, DoorRay and RayRemoveTraps)
//   ESP             positions are projected to the screen here (main thread) and drawn by the menu (render thread)

namespace vm {
namespace cheat {
namespace {

using namespace il2;

CheatSettings gC;  // newest settings; written by tick(), read by the hooks (all on the main thread)
bool gResolved = false;

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

template <typename T>
inline T* P(void* o, size_t off) {
    return reinterpret_cast<T*>(reinterpret_cast<char*>(o) + off);
}
inline Il2CppObject* ref(void* o, size_t off) { return (o && off) ? *P<Il2CppObject*>(o, off) : nullptr; }

// ---------------------------------------------------------------- what the game offers
struct Ic {
    Il2CppObject* (*getTransform)(void*) = nullptr;     // Component.transform
    Il2CppObject* (*goTransform)(void*) = nullptr;      // GameObject.transform
    Il2CppObject* (*getGameObject)(void*) = nullptr;
    void (*getPos)(void*, float*) = nullptr;
    void (*setPos)(void*, const float*) = nullptr;
    void (*getRot)(void*, float*) = nullptr;
    void (*setRot)(void*, const float*) = nullptr;
    bool (*activeInH)(void*) = nullptr;
    bool (*activeSelf)(void*) = nullptr;
    bool (*behEnabled)(void*) = nullptr;
    void (*setBehEnabled)(void*, bool) = nullptr;
    bool (*onNav)(void*) = nullptr;
    void (*setStopped)(void*, bool) = nullptr;
    void (*setVel)(void*, const float*) = nullptr;
    void (*camW2C)(void*, float*) = nullptr;
    void (*camProj)(void*, float*) = nullptr;
    void (*colEnabled)(void*, bool) = nullptr;
    void (*rbKinematic)(void*, bool) = nullptr;
} ic;

struct Off {
    // MobileFPS
    size_t fpsCc = 0, fpsFh = 0, fpsPs = 0, fpsCam2 = 0, fpsCam = 0, fpsMoveDir = 0;
    // PlayerStatus
    size_t psPick = 0, psCam = 0;
    // PickRay
    size_t prAmmo = 0, prSpray = 0, prReach = 0, prDrop1 = 0, prItemDrop = 0, prDropP = 0, prPrompt = 0, prButton = 0;
    // FallingHolder
    size_t fhHold = 0, fhDur = 0, fhDamaged = 0, fhDeath = 0, fhFalling = 0, fhLanding = 0, fhFell = 0, fhSound = 0, fhDurate = 0;
    // others
    size_t isCount = 0, seedName = 0, doorReach = 0, trapReach = 0;
} O;

Il2CppClass *cFps, *cPs, *cPick, *cFall, *cItemSpawn, *cSeed, *cDoor, *cRayTraps, *cCollider, *cRigidbody, *cGo, *cTransform;
const MethodInfo *mClick, *mInstantiate, *mGetComp, *mGetCompsInChildren, *mSetParent, *mSetActive, *mIsChildOf;

// the names of the fields of ItemSpawn, in the same order as kItemNames
const char* const kItemFields[kItemCount] = {
    "crossbow", "plier", "battery", "gas", "seed", "book", "winch", "carbattery", "carkey", "cutter", "code", "baton", "eckey",
    "hammer", "padlock", "mas", "kugg1", "kugg2", "meat", "melon", "spray", "plank", "playh", "remote", "data",
    "rusty", "safe", "screw", "shotgun", "shotgun2", "sp1", "sp2", "sp3", "spark", "special", "spider", "syringe",
    "t1", "t2", "t3", "t4", "teddy", "text", "topp", "wp", "vas", "vas2", "vas3", "wheel", "stick", "wrench", "rat",
    "ornamentfreeze", "ornamentbomb", "fuse",
};
static_assert(sizeof(kItemFields) / sizeof(kItemFields[0]) == (size_t)kItemCount, "item field names and item names differ");

// ---------------------------------------------------------------- enemies
struct EnemySpec {
    const char* cls;
    const char* method;       // the update that is hooked
    const char* agent;        // NavMeshAgent field
    const char* blind;        // bool: the enemy cannot see
    const char* det1;         // floats: detection ranges (blind for the Little One)
    const char* det2;
    const char* follow;       // deaf: bool, object, float
    const char* noise;
    const char* timer;
    const char* tag;          // string: the tag the enemy looks noise up by
};
const EnemySpec kSpec[kEnemyCount] = {
    {"AI_Granny", "FixedUpdate", "Agent", "IsBlind", nullptr, nullptr, "IsFollowingSound", "NoiseObj", "TimerNearNoise", "NoiseObjectTag"},
    {"AI_Grandpa", "FixedUpdate", "Agent", "IsBlind", nullptr, nullptr, "IsFollowingSound", "NoiseObj", "TimerNearNoise", "NoiseObjectTag"},
    {"AI_MomSpider", "Update", "Agent", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
    {"AI_Slendrina", "Update", "Agent", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
    {"AtticSpider", "Update", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
    {"CrowManage", "Update", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
    {"LittleFucker", "Update", "Agent", nullptr, "DetectionRange", "DetectionRangeNear", nullptr, nullptr, nullptr, nullptr},
};
struct EnemyOff {
    size_t agent = 0, blind = 0, det1 = 0, det2 = 0, follow = 0, noise = 0, timer = 0, tag = 0, dying = 0, health = 0;
};
Il2CppClass* gEnemyCls[kEnemyCount] = {};
EnemyOff gEO[kEnemyCount];
const MethodInfo* gKillM[kEnemyCount] = {};   // the method that kills / stuns, where the game has one
const MethodInfo *gStopAI[2] = {}, *gDeadDisable[2] = {}, *gResetDecision[2] = {};

// size of the box drawn around an enemy: height in metres, width as a fraction of the height
const float kEspH[kEnemyCount] = {1.75f, 1.85f, 1.6f, 1.9f, 0.7f, 0.5f, 1.1f};
const float kEspW[kEnemyCount] = {0.45f, 0.45f, 1.1f, 0.4f, 1.3f, 1.0f, 0.6f};

struct EnemyState {
    uint32_t handle = 0;      // keeps the object alive while we know it
    int64_t seenAt = 0;
    bool frozen = false;
    float pos[3] = {0, 0, 0};
    float rot[4] = {0, 0, 0, 1};
    bool blindSet = false, origBlind = false;
    bool detSet = false;
    float origDet1 = 0, origDet2 = 0;
    bool dead = false;
};
std::unordered_map<Il2CppObject*, EnemyState> gSt[kEnemyCount];
Il2CppObject* gSeen[kEnemyCount] = {};
int64_t gSeenAt[kEnemyCount] = {};
int gKillAsked[kEnemyCount] = {};
bool gKillPending[kEnemyCount] = {};
bool gInDeaf = false;
std::string gNoiseTag;

// ---------------------------------------------------------------- the player
Il2CppObject *gFps = nullptr, *gPs = nullptr, *gPr = nullptr, *gCamObj = nullptr, *gPlayerCc = nullptr;
int64_t gFpsAt = 0;
float gAmmoMax = 0.f, gSprayMax = 0.f;
bool gNoclipOn = false;       // the controller is switched off and the player is moved by hand
int64_t gNoclipOffAt = -100000;
int gSpawnAsked = 0;
bool gSpawnPending = false;
int gSpawnIndex = 0;

// ---------------------------------------------------------------- bookkeeping for the menu
int gHookTotal = 0, gHookOk = 0;
std::vector<std::string> gFailed;
bool gGodHooksOk = true;

// ---------------------------------------------------------------- small helpers
bool valid(Il2CppObject* o) { return o && alive(o); }

bool activeIn(Il2CppObject* comp) {
    if (!comp || !ic.getGameObject || !ic.activeInH) return false;
    Il2CppObject* go = ic.getGameObject(comp);
    return go && ic.activeInH(go);
}

bool position(Il2CppObject* comp, float* out) {
    if (!comp || !ic.getTransform || !ic.getPos) return false;
    Il2CppObject* tr = ic.getTransform(comp);
    if (!tr) return false;
    ic.getPos(tr, out);
    return true;
}

void stopAgent(Il2CppObject* agent, bool stop) {
    if (!valid(agent) || !ic.onNav || !ic.setStopped) return;
    if (!ic.onNav(agent)) return;  // Unity complains when an agent that is not on the NavMesh is told to stop
    ic.setStopped(agent, stop);
    if (stop && ic.setVel) {
        const float zero[3] = {0, 0, 0};
        ic.setVel(agent, zero);
    }
}

const MethodInfo* findExact(Il2CppClass* k, const char* name, std::initializer_list<const char*> params) {
    if (!k) return nullptr;
    void* it = nullptr;
    while (const MethodInfo* m = A.class_get_methods(k, &it)) {
        if (strcmp(A.method_get_name(m), name) != 0) continue;
        if (A.method_get_param_count(m) != params.size()) continue;
        bool ok = true;
        uint32_t i = 0;
        for (const char* p : params) {
            const char* tn = A.type_get_name(A.method_get_param(m, i++));
            if (!tn || strcmp(tn, p) != 0) {
                ok = false;
                break;
            }
        }
        if (ok) return m;
    }
    return nullptr;
}

Il2CppObject* getComponent(Il2CppObject* go, Il2CppClass* k) {
    if (!go || !k || !mGetComp) return nullptr;
    void* a[1] = {typeObject(k)};
    return invoke(mGetComp, go, a);
}

// ---------------------------------------------------------------- resolve everything by name
void resolve() {
    if (gResolved) return;
    gResolved = true;

    ic.getTransform = reinterpret_cast<Il2CppObject* (*)(void*)>(icall("UnityEngine.Component::get_transform"));
    ic.goTransform = reinterpret_cast<Il2CppObject* (*)(void*)>(icall("UnityEngine.GameObject::get_transform"));
    ic.getGameObject = reinterpret_cast<Il2CppObject* (*)(void*)>(icall("UnityEngine.Component::get_gameObject"));
    ic.getPos = reinterpret_cast<void (*)(void*, float*)>(icall("UnityEngine.Transform::get_position_Injected"));
    ic.setPos = reinterpret_cast<void (*)(void*, const float*)>(icall("UnityEngine.Transform::set_position_Injected"));
    ic.getRot = reinterpret_cast<void (*)(void*, float*)>(icall("UnityEngine.Transform::get_rotation_Injected"));
    ic.setRot = reinterpret_cast<void (*)(void*, const float*)>(icall("UnityEngine.Transform::set_rotation_Injected"));
    ic.activeInH = reinterpret_cast<bool (*)(void*)>(icall("UnityEngine.GameObject::get_activeInHierarchy"));
    ic.activeSelf = reinterpret_cast<bool (*)(void*)>(icall("UnityEngine.GameObject::get_activeSelf"));
    ic.behEnabled = reinterpret_cast<bool (*)(void*)>(icall("UnityEngine.Behaviour::get_enabled"));
    ic.setBehEnabled = reinterpret_cast<void (*)(void*, bool)>(icall("UnityEngine.Behaviour::set_enabled"));
    ic.onNav = reinterpret_cast<bool (*)(void*)>(icall("UnityEngine.AI.NavMeshAgent::get_isOnNavMesh"));
    ic.setStopped = reinterpret_cast<void (*)(void*, bool)>(icall("UnityEngine.AI.NavMeshAgent::set_isStopped"));
    ic.setVel = reinterpret_cast<void (*)(void*, const float*)>(icall("UnityEngine.AI.NavMeshAgent::set_velocity_Injected"));
    ic.camW2C = reinterpret_cast<void (*)(void*, float*)>(icall("UnityEngine.Camera::get_worldToCameraMatrix_Injected"));
    ic.camProj = reinterpret_cast<void (*)(void*, float*)>(icall("UnityEngine.Camera::get_projectionMatrix_Injected"));
    ic.colEnabled = reinterpret_cast<void (*)(void*, bool)>(icall("UnityEngine.Collider::set_enabled"));
    ic.rbKinematic = reinterpret_cast<void (*)(void*, bool)>(icall("UnityEngine.Rigidbody::set_isKinematic"));

    cFps = findClass("", "MobileFPS");
    cPs = findClass("", "PlayerStatus");
    cPick = findClass("", "PickRay");
    cFall = findClass("", "FallingHolder");
    cItemSpawn = findClass("", "ItemSpawn");
    cSeed = findClass("", "ItemSeedData");
    cDoor = findClass("", "DoorRay");
    cRayTraps = findClass("", "RayRemoveTraps");
    cCollider = findClass("UnityEngine", "Collider");
    cRigidbody = findClass("UnityEngine", "Rigidbody");
    cGo = findClass("UnityEngine", "GameObject");
    cTransform = findClass("UnityEngine", "Transform");

    O.fpsCc = fieldChecked(cFps, "MobileFPS", "characterController", kTypeRef);
    O.fpsFh = fieldChecked(cFps, "MobileFPS", "FallingHolder", kTypeRef);
    O.fpsPs = fieldChecked(cFps, "MobileFPS", "PS", kTypeRef);
    O.fpsCam2 = fieldChecked(cFps, "MobileFPS", "playerCamera2", kTypeRef);
    O.fpsCam = fieldChecked(cFps, "MobileFPS", "playerCamera", kTypeRef);
    O.fpsMoveDir = fieldChecked(cFps, "MobileFPS", "moveDirection", kTypeValue);
    O.psPick = fieldChecked(cPs, "PlayerStatus", "PickRay", kTypeRef);
    O.psCam = fieldChecked(cPs, "PlayerStatus", "PlayerCam", kTypeRef);
    O.prAmmo = fieldChecked(cPick, "PickRay", "AmmoAmount", kTypeR4);
    O.prSpray = fieldChecked(cPick, "PickRay", "SprayLeft", kTypeR4);
    O.prReach = fieldChecked(cPick, "PickRay", "RaycastDis", kTypeR4);
    O.prDrop1 = fieldChecked(cPick, "PickRay", "Drop1", kTypeRef);
    O.prItemDrop = fieldChecked(cPick, "PickRay", "ItemDrop", kTypeRef);
    O.prDropP = fieldChecked(cPick, "PickRay", "DropP", kTypeRef);
    O.prPrompt = fieldChecked(cPick, "PickRay", "EButton_Pick", kTypeRef);
    O.prButton = fieldChecked(cPick, "PickRay", "Button", kTypeRef);
    O.fhHold = fieldChecked(cFall, "FallingHolder", "FallDurationHold", kTypeR4);
    O.fhDur = fieldChecked(cFall, "FallingHolder", "fallDuration", kTypeR4);
    O.fhDamaged = fieldChecked(cFall, "FallingHolder", "Damaged", kTypeBool);
    O.fhDeath = fieldChecked(cFall, "FallingHolder", "DeathFall", kTypeBool);
    O.fhFalling = fieldChecked(cFall, "FallingHolder", "isFalling", kTypeBool);
    O.fhLanding = fieldChecked(cFall, "FallingHolder", "isLanding", kTypeBool);
    O.fhFell = fieldChecked(cFall, "FallingHolder", "Fell", kTypeBool);
    O.fhSound = fieldChecked(cFall, "FallingHolder", "CanFallSound", kTypeBool);
    O.fhDurate = fieldChecked(cFall, "FallingHolder", "DurateCan", kTypeR4);
    O.isCount = fieldChecked(cItemSpawn, "ItemSpawn", "CountItem", kTypeI4);
    O.seedName = fieldChecked(cSeed, "ItemSeedData", "itemName", kTypeRef);
    O.doorReach = fieldChecked(cDoor, "DoorRay", "RaycastDis", kTypeR4);
    O.trapReach = fieldChecked(cRayTraps, "RayRemoveTraps", "RaycastDis", kTypeR4);

    mClick = findMethod(cPick, "HandleButtonClick", 0);
    mInstantiate = findExact(findClass("UnityEngine", "Object"), "Instantiate",
                             {"UnityEngine.Object", "UnityEngine.Vector3", "UnityEngine.Quaternion"});
    mGetComp = findMethod(cGo, "GetComponent", 1, "System.Type");
    mGetCompsInChildren = findExact(cGo, "GetComponentsInChildren", {"System.Type", "System.Boolean"});
    mSetParent = findExact(cTransform, "SetParent", {"UnityEngine.Transform", "System.Boolean"});
    mSetActive = findMethod(cGo, "SetActive", 1);
    mIsChildOf = findMethod(cTransform, "IsChildOf", 1);

    for (int k = 0; k < kEnemyCount; k++) {
        const EnemySpec& sp = kSpec[k];
        Il2CppClass* c = findClass("", sp.cls);
        gEnemyCls[k] = c;
        if (!c) {
            logf("cheat: class %s not found", sp.cls);
            continue;
        }
        EnemyOff& e = gEO[k];
        if (sp.agent) e.agent = fieldChecked(c, sp.cls, sp.agent, kTypeRef);
        if (sp.blind) e.blind = fieldChecked(c, sp.cls, sp.blind, kTypeBool);
        if (sp.det1) e.det1 = fieldChecked(c, sp.cls, sp.det1, kTypeR4);
        if (sp.det2) e.det2 = fieldChecked(c, sp.cls, sp.det2, kTypeR4);
        if (sp.follow) e.follow = fieldChecked(c, sp.cls, sp.follow, kTypeBool);
        if (sp.noise) e.noise = fieldChecked(c, sp.cls, sp.noise, kTypeRef);
        if (sp.timer) e.timer = fieldChecked(c, sp.cls, sp.timer, kTypeR4);
        if (sp.tag) e.tag = fieldChecked(c, sp.cls, sp.tag, kTypeRef);
        if (k < 2) {
            e.dying = fieldChecked(c, sp.cls, "IsDying", kTypeBool);
            gStopAI[k] = findMethod(c, "StopAI", 0);
            gDeadDisable[k] = findMethod(c, "DeadDisable", 0);
            gResetDecision[k] = findMethod(c, "ResetAIDecision", 0);
        }
        if (k == 2) gKillM[k] = findMethod(c, "HitMom", 0);
        if (k == 3) e.health = fieldChecked(c, sp.cls, "Health", kTypeR4);
        if (k == 4) gKillM[k] = findMethod(c, "Dies", 0);
        if (k == 5) gKillM[k] = findMethod(c, "KillAssBird", 0);
        if (k == 6) gKillM[k] = findMethod(c, "KnockedOut", 0);
    }
    logf("cheat: resolved (spawn %s, auto grab %s, ESP %s)", (mInstantiate && mGetComp && O.prItemDrop && O.prDropP && O.isCount) ? "ok" : "unavailable",
         (mClick && O.prDrop1 && O.prPrompt) ? "ok" : "unavailable", (ic.camW2C && ic.camProj && ic.getPos) ? "ok" : "unavailable");
}

// ---------------------------------------------------------------- hooks: god mode
void (*gOrigPs[9])(void*, void*) = {};
const char* const kPsNames[9] = {"NormalDeath",       "KnockDeath",          "PlayerGettingStopped", "MomDeath",          "DamageSpider",
                                 "GrannyCaughtYou",   "GrannyCaughtYouBed",  "GrandpaCaughtYou",     "GrandpaCaughtYouBed"};

void forgetPlayer(int enemy) {  // the enemy "forgets" the player instead of running its caught sequence
    if (enemy < 0 || enemy > 1) return;
    Il2CppObject* o = gSeen[enemy];
    if (!o || nowMs() - gSeenAt[enemy] > 3000 || !gResetDecision[enemy] || !valid(o)) return;
    invoke(gResetDecision[enemy], o, nullptr);
}

template <int I>
void psHook(void* self, void* mi) {
    if (!gOrigPs[I]) return;
    if (gC.godMode) {
        if (I == 5 || I == 6) forgetPlayer(0);
        if (I == 7 || I == 8) forgetPlayer(1);
        return;
    }
    gOrigPs[I](self, mi);
}

// ---------------------------------------------------------------- hooks: traps and the cracking floor
void (*gOrigTrap[5])(void*, void*, void*) = {};
template <int I>
void trapHook(void* self, void* col, void* mi) {
    if (!gOrigTrap[I]) return;
    if (I == 4 ? gC.noCrackFloor : gC.noTrap) return;
    gOrigTrap[I](self, col, mi);
}

// ---------------------------------------------------------------- hooks: no fall damage (and no falling while in no clip)
void (*gOrigFall[3])(void*, void*) = {};
void suppressFall(void* fh) {  // everything that says "we are falling / have fallen" goes back to "no"
    if (!fh) return;
    if (O.fhSound) *P<bool>(fh, O.fhSound) = false;
    if (O.fhFalling) *P<bool>(fh, O.fhFalling) = false;
    if (O.fhLanding) *P<bool>(fh, O.fhLanding) = false;
    if (O.fhFell) *P<bool>(fh, O.fhFell) = false;
    if (O.fhDamaged) *P<bool>(fh, O.fhDamaged) = false;
    if (O.fhDeath) *P<bool>(fh, O.fhDeath) = false;
    if (O.fhDur) *P<float>(fh, O.fhDur) = 0.f;
    if (O.fhDurate) *P<float>(fh, O.fhDurate) = 0.f;
}
bool noclipGrace(int64_t now) { return gNoclipOn || now - gNoclipOffAt < 2500; }  // a little after no clip ends too: the player is still in the air
void keepFallShort(void* fh) {
    if (!fh) return;
    if (noclipGrace(nowMs())) {
        suppressFall(fh);
        return;
    }
    if (!gC.noFallDamage) return;
    if (O.fhHold && O.fhDur) {
        float hold = *P<float>(fh, O.fhHold);
        float* d = P<float>(fh, O.fhDur);
        if (hold > 0.f && *d > hold * 0.5f) *d = hold * 0.5f;
    }
    if (O.fhDamaged) *P<bool>(fh, O.fhDamaged) = false;
    if (O.fhDeath) *P<bool>(fh, O.fhDeath) = false;
}
template <int I>
void fallHook(void* self, void* mi) {
    if (!gOrigFall[I]) return;
    keepFallShort(self);
    if (gNoclipOn) return;  // nothing to land on: the game's fall code is not run at all
    gOrigFall[I](self, mi);
    keepFallShort(self);
}

// ---------------------------------------------------------------- hooks: speed
struct V3 {
    float x, y, z;
};
int (*gOrigMove)(void*, V3, void*) = nullptr;

// No clip: the game asks the controller to move by `v` (already a distance for this frame). That is added to the
// position directly. The height comes from where the camera looks: walking forward while looking up rises.
void noclipMove(const V3& v, float mult) {
    Il2CppObject* tr = ic.getTransform ? ic.getTransform(gPlayerCc) : nullptr;
    if (!tr || !ic.getPos || !ic.setPos) return;
    float dx = v.x * mult, dz = v.z * mult, dy = 0.f;
    if (valid(gCamObj) && ic.getRot) {
        Il2CppObject* ct = ic.getTransform(gCamObj);
        if (ct) {
            float q[4];
            ic.getRot(ct, q);
            float fx = 2.f * (q[0] * q[2] + q[3] * q[1]);
            float fy = 2.f * (q[1] * q[2] - q[3] * q[0]);
            float fz = 1.f - 2.f * (q[0] * q[0] + q[1] * q[1]);
            float hl = std::sqrt(fx * fx + fz * fz);
            if (hl > 0.05f) {
                float along = (dx * fx + dz * fz) / hl;  // how far the move goes in the direction the camera faces
                float slope = fy / hl;                   // tan of the camera pitch
                if (slope > 3.f) slope = 3.f;
                if (slope < -3.f) slope = -3.f;
                if (std::fabs(slope) < 0.12f) slope = 0.f;  // looking straight ahead: stay level
                dy = along * slope;
            }
        }
    }
    float p[3];
    ic.getPos(tr, p);
    p[0] += dx;
    p[1] += dy;
    p[2] += dz;
    ic.setPos(tr, p);
}

int hookMove(void* self, V3 v, void* mi) {
    if (!gOrigMove) return 0;
    float m = 1.f;
    if (gC.speedOn) m = gC.speed < 0.1f ? 0.1f : (gC.speed > 6.f ? 6.f : gC.speed);
    if (self && self == gPlayerCc) {
        if (gNoclipOn) {
            noclipMove(v, m);
            return 0;
        }
        v.x *= m;
        v.z *= m;
    }
    return gOrigMove(self, v, mi);
}

// ---------------------------------------------------------------- hooks: deaf (the enemy asks Unity for the noise object by tag)
Il2CppObject* (*gOrigFindTag)(Il2CppString*, void*) = nullptr;
Il2CppObject* hookFindTag(Il2CppString* tag, void* mi) {
    if (!gOrigFindTag) return nullptr;
    if (gInDeaf && tag && !gNoiseTag.empty() && str(tag) == gNoiseTag) return nullptr;
    return gOrigFindTag(tag, mi);
}

// ---------------------------------------------------------------- enemies
EnemyState& stateFor(int k, Il2CppObject* o) {
    auto it = gSt[k].find(o);
    if (it != gSt[k].end()) return it->second;
    EnemyState s;
    s.handle = retain(o);
    return gSt[k].emplace(o, s).first->second;
}

void releaseFreeze(int k, Il2CppObject* o, EnemyState& s) {
    if (!s.frozen) return;
    s.frozen = false;
    if (gEO[k].agent) stopAgent(ref(o, gEO[k].agent), false);
}

void killEnemy(int k, Il2CppObject* o, EnemyState& s) {
    bool ok = true;
    bool permanent = true;
    if (k < 2) {
        if (gEO[k].dying) *P<bool>(o, gEO[k].dying) = true;
        if (gStopAI[k]) invoke(gStopAI[k], o, nullptr);
        if (gDeadDisable[k]) invoke(gDeadDisable[k], o, nullptr);
        if (gEO[k].agent) stopAgent(ref(o, gEO[k].agent), true);
        if (ic.setBehEnabled) ic.setBehEnabled(o, false);  // its update stops for good
    } else if (k == 3) {
        if (!gEO[k].health) ok = false;
        else *P<float>(o, gEO[k].health) = 0.f;
        if (gEO[k].agent) stopAgent(ref(o, gEO[k].agent), true);
    } else {
        if (!gKillM[k]) ok = false;
        else invoke(gKillM[k], o, nullptr);
        permanent = (k != 2 && k != 6);  // a stunned Mom Spider and a knocked out Little One get up again
    }
    char msg[64];
    if (!ok) {
        snprintf(msg, sizeof(msg), "%s: %s is not available", kEnemyNames[k], kEnemyKill[k]);
    } else {
        snprintf(msg, sizeof(msg), "%s: %s", kEnemyNames[k], k == 2 ? "stunned" : (k == 6 ? "knocked out" : "killed"));
        if (permanent) s.dead = true;
        releaseFreeze(k, o, s);
    }
    logf("cheat: %s", msg);
    postToast(msg);
}

// Returns false when the game's own update has to be skipped (the enemy is frozen).
bool enemyPre(int k, Il2CppObject* o) {
    EnemyState& s = stateFor(k, o);
    int64_t now = nowMs();
    s.seenAt = now;
    gSeen[k] = o;
    gSeenAt[k] = now;
    const EnemyOff& e = gEO[k];
    const CheatSettings::Enemy& want = gC.enemy[k];

    if (gKillPending[k]) {
        gKillPending[k] = false;
        killEnemy(k, o, s);
    }
    if (s.dead) return true;

    // blind
    if (kEnemyCan[k][1]) {
        if (e.blind) {
            bool* p = P<bool>(o, e.blind);
            if (want.blind) {
                if (!s.blindSet) {
                    s.origBlind = *p;
                    s.blindSet = true;
                }
                *p = true;
            } else if (s.blindSet) {
                *p = s.origBlind;
                s.blindSet = false;
            }
        } else if (e.det1 && e.det2) {
            float* a = P<float>(o, e.det1);
            float* b = P<float>(o, e.det2);
            if (want.blind) {
                if (!s.detSet) {
                    s.origDet1 = *a;
                    s.origDet2 = *b;
                    s.detSet = true;
                }
                *a = 0.f;
                *b = 0.f;
            } else if (s.detSet) {
                *a = s.origDet1;
                *b = s.origDet2;
                s.detSet = false;
            }
        }
    }

    // deaf
    gInDeaf = false;
    if (kEnemyCan[k][2] && want.deaf && e.follow && e.noise && e.timer) {
        *P<bool>(o, e.follow) = false;
        *P<Il2CppObject*>(o, e.noise) = nullptr;
        *P<float>(o, e.timer) = 0.f;
        if (e.tag) gNoiseTag = str(reinterpret_cast<Il2CppString*>(ref(o, e.tag)));
        gInDeaf = true;
    }

    // freeze: the update is skipped, the agent stopped, the position held
    if (want.freeze) {
        if (!s.frozen) {
            s.frozen = true;
            if (ic.getTransform && ic.getPos && ic.getRot) {
                Il2CppObject* tr = ic.getTransform(o);
                if (tr) {
                    ic.getPos(tr, s.pos);
                    ic.getRot(tr, s.rot);
                }
            }
        }
        if (e.agent) stopAgent(ref(o, e.agent), true);
        if (ic.getTransform && ic.getPos && ic.setPos) {
            Il2CppObject* tr = ic.getTransform(o);
            if (tr) {
                float cur[3];
                ic.getPos(tr, cur);
                float dx = cur[0] - s.pos[0], dy = cur[1] - s.pos[1], dz = cur[2] - s.pos[2];
                if (dx * dx + dy * dy + dz * dz > 1e-6f) ic.setPos(tr, s.pos);
            }
        }
        return false;
    }
    releaseFreeze(k, o, s);
    return true;
}

void (*gOrigEnemy[kEnemyCount])(void*, void*) = {};
template <int K>
void enemyHook(void* self, void* mi) {
    if (!gOrigEnemy[K]) return;
    if (!self || !enemyPre(K, reinterpret_cast<Il2CppObject*>(self))) return;
    gOrigEnemy[K](self, mi);
    gInDeaf = false;
}

// ---------------------------------------------------------------- items
struct ItemEntry {
    uint32_t handle = 0;
    char name[28] = {0};
};
std::unordered_map<Il2CppObject*, ItemEntry> gItems;
int64_t gItemScanAt = 0;

struct Reach {
    float orig = 0, last = 0;
};
std::unordered_map<void*, Reach> gReach;

void reachApply(Il2CppObject* o, size_t off, bool on, float want) {
    if (!o || !off) return;
    float* p = P<float>(o, off);
    float cur = *p;
    auto it = gReach.find(o);
    if (on) {
        if (it == gReach.end()) it = gReach.emplace(o, Reach{cur, cur}).first;
        else if (std::fabs(cur - it->second.last) > 0.001f && std::fabs(cur - want) > 0.001f) it->second.orig = cur;  // the game set a new value
        if (std::fabs(cur - want) > 0.001f) *p = want;
        it->second.last = want;
    } else if (it != gReach.end()) {
        *p = it->second.orig;
        gReach.erase(it);
    }
}

std::vector<Il2CppObject*> gRayObjs;  // DoorRay and RayRemoveTraps instances, kept alive
std::vector<uint32_t> gRayHandles;
std::vector<int> gRayKind;
int64_t gRayScanAt = 0;

void clearRayObjs() {
    for (uint32_t h : gRayHandles) release(h);
    gRayObjs.clear();
    gRayHandles.clear();
    gRayKind.clear();
}

void scanRayObjs(int64_t now) {
    if (now < gRayScanAt) return;
    gRayScanAt = now + 1000;
    clearRayObjs();
    for (int kind = 0; kind < 2; kind++) {
        Il2CppClass* c = kind == 0 ? cDoor : cRayTraps;
        if (!c || !(kind == 0 ? O.doorReach : O.trapReach)) continue;
        Il2CppObject* arr = findObjectsOfType(c);
        if (!arr) continue;
        size_t n = A.array_length(arr);
        Il2CppObject** el = arrayData(arr);
        for (size_t i = 0; i < n && i < 8; i++) {
            if (!valid(el[i])) continue;
            gRayObjs.push_back(el[i]);
            gRayHandles.push_back(retain(el[i]));
            gRayKind.push_back(kind);
        }
    }
}

void scanItems(int64_t now) {
    bool need = gC.espItem || gC.autoGrab;
    if (!need) {
        if (!gItems.empty()) {
            for (auto& kv : gItems) release(kv.second.handle);
            gItems.clear();
        }
        return;
    }
    if (now < gItemScanAt || !cSeed) return;
    gItemScanAt = now + 1000;
    for (auto it = gItems.begin(); it != gItems.end();) {
        if (!valid(it->first)) {
            release(it->second.handle);
            it = gItems.erase(it);
        } else {
            ++it;
        }
    }
    Il2CppObject* arr = findObjectsOfType(cSeed);
    if (!arr) return;
    size_t n = A.array_length(arr);
    Il2CppObject** el = arrayData(arr);
    for (size_t i = 0; i < n && gItems.size() < 300; i++) {
        Il2CppObject* o = el[i];
        if (!valid(o) || gItems.count(o)) continue;
        ItemEntry e;
        e.handle = retain(o);
        std::string nm = O.seedName ? str(reinterpret_cast<Il2CppString*>(ref(o, O.seedName))) : "";
        if (nm.empty()) nm = "Item";
        snprintf(e.name, sizeof(e.name), "%s", nm.c_str());
        gItems.emplace(o, e);
    }
}

// Spawn: the same thing the game does when you drop an item (PickRay.CheckItemDropping): copy the ItemDrop prefab at
// the drop point and write the item number into its ItemSpawn.
bool spawnItem(Il2CppObject* pr, int index, const char** why) {
    *why = "";
    if (!mInstantiate || !mGetComp || !O.prItemDrop || !O.prDropP || !O.isCount || !ic.getPos || !ic.getRot) {
        *why = "not available in this game version";
        return false;
    }
    Il2CppObject* prefab = ref(pr, O.prItemDrop);
    Il2CppObject* drop = ref(pr, O.prDropP);
    if (!valid(prefab) || !valid(drop)) {
        *why = "no drop point yet";
        return false;
    }
    float pos[3], rot[4];
    ic.getPos(drop, pos);
    ic.getRot(drop, rot);

    // Some items (Grandpa's shotgun) are an object that already lives in the level, not part of the prefab: the dropper
    // only moves that one. For those a real copy is made instead.
    Il2CppObject* original = nullptr;
    Il2CppObject* comp = getComponent(prefab, cItemSpawn);
    if (comp && index >= 0 && index < kItemCount) {
        size_t off = fieldChecked(cItemSpawn, "ItemSpawn", kItemFields[index], kTypeRef);
        if (off) original = ref(comp, off);
    }
    bool copyPath = false;
    if (valid(original) && mIsChildOf && ic.goTransform) {
        Il2CppObject* t1 = ic.goTransform(original);
        Il2CppObject* t2 = ic.goTransform(prefab);
        if (t1 && t2) {
            void* a[1] = {t2};
            copyPath = !callBool(mIsChildOf, t1, a, true);
        }
    }

    Il2CppObject* src = copyPath ? original : prefab;
    void* args[3] = {src, pos, rot};
    bool ok = true;
    Il2CppObject* copy = invoke(mInstantiate, nullptr, args, &ok);
    if (!ok || !copy) {
        *why = "the game refused";
        return false;
    }
    if (!copyPath) {
        Il2CppObject* c2 = getComponent(copy, cItemSpawn);
        if (!c2) {
            *why = "the copy has no ItemSpawn";
            return false;
        }
        *P<int>(c2, O.isCount) = index + 1;
        return true;
    }
    // a copy of an object from the level: free it from whoever holds the original and make it solid again
    if (mSetParent) {
        Il2CppObject* tr = ic.goTransform ? ic.goTransform(copy) : nullptr;
        bool keep = true;
        void* a[2] = {nullptr, &keep};
        if (tr) invoke(mSetParent, tr, a);
    }
    if (mSetActive) {
        bool on = true;
        void* a[1] = {&on};
        invoke(mSetActive, copy, a);
    }
    if (mGetCompsInChildren && cCollider && ic.colEnabled) {
        bool inactive = true;
        void* a[2] = {typeObject(cCollider), &inactive};
        Il2CppObject* arr = invoke(mGetCompsInChildren, copy, a);
        if (arr) {
            size_t n = A.array_length(arr);
            Il2CppObject** el = arrayData(arr);
            for (size_t i = 0; i < n && i < 32; i++)
                if (valid(el[i])) ic.colEnabled(el[i], true);
        }
    }
    if (cRigidbody && ic.rbKinematic) {
        Il2CppObject* rb = getComponent(copy, cRigidbody);
        if (valid(rb)) ic.rbKinematic(rb, false);
    }
    return true;
}

// ---------------------------------------------------------------- camera maths (shared by ESP and auto grab)
struct Cam {
    bool ok = false;
    float v[16], p[16];
    float pos[3] = {0, 0, 0};
    float fwd[3] = {0, 0, 1};
    float aspect = 1.f;
};

bool getCam(Cam& c) {
    Il2CppObject* cam = gCamObj;
    if (!valid(cam) || nowMs() - gFpsAt > 500 || !ic.camW2C || !ic.camProj || !ic.getTransform || !ic.getPos || !ic.getRot) return false;
    ic.camW2C(cam, c.v);
    ic.camProj(cam, c.p);
    Il2CppObject* tr = ic.getTransform(cam);
    if (!tr) return false;
    ic.getPos(tr, c.pos);
    float q[4];
    ic.getRot(tr, q);
    c.fwd[0] = 2.f * (q[0] * q[2] + q[3] * q[1]);
    c.fwd[1] = 2.f * (q[1] * q[2] - q[3] * q[0]);
    c.fwd[2] = 1.f - 2.f * (q[0] * q[0] + q[1] * q[1]);
    c.aspect = std::fabs(c.p[0]) > 1e-6f ? c.p[5] / c.p[0] : 1.f;
    if (c.aspect < 0.1f || c.aspect > 10.f) c.aspect = 1.78f;
    c.ok = true;
    return true;
}

// Unity matrices are column-major: element (row r, column c) is m[c * 4 + r]. Result: 0..1, origin top-left.
bool project(const Cam& c, const float* w, float* sx, float* sy) {
    const float* V = c.v;
    const float* M = c.p;
    float vx = V[0] * w[0] + V[4] * w[1] + V[8] * w[2] + V[12];
    float vy = V[1] * w[0] + V[5] * w[1] + V[9] * w[2] + V[13];
    float vz = V[2] * w[0] + V[6] * w[1] + V[10] * w[2] + V[14];
    float vw = V[3] * w[0] + V[7] * w[1] + V[11] * w[2] + V[15];
    float cx = M[0] * vx + M[4] * vy + M[8] * vz + M[12] * vw;
    float cy = M[1] * vx + M[5] * vy + M[9] * vz + M[13] * vw;
    float cw = M[3] * vx + M[7] * vy + M[11] * vz + M[15] * vw;
    if (cw < 0.05f) return false;  // behind the camera
    *sx = cx / cw * 0.5f + 0.5f;
    *sy = 0.5f - cy / cw * 0.5f;
    return true;
}

// ---------------------------------------------------------------- auto grab
Il2CppObject* gGrabTarget = nullptr;
int64_t gGrabSince = 0, gGrabNext = 0;
int gGrabTries = 0;
bool gHandsWereFull = false;
std::unordered_map<Il2CppObject*, int64_t> gGrabSkip;

bool promptShown(Il2CppObject* pr) {
    Il2CppObject* img = ref(pr, O.prPrompt);
    if (!valid(img) || !ic.behEnabled || !ic.behEnabled(img)) return false;
    if (!activeIn(img)) return false;
    Il2CppObject* btn = ref(pr, O.prButton);
    if (btn && valid(btn) && ic.activeInH && !ic.activeInH(btn)) return false;
    return true;
}

void autoGrab(Il2CppObject* pr, int64_t now) {
    if (!gC.autoGrab || !mClick || !O.prDrop1 || !O.prPrompt) return;
    Il2CppObject* hand = ref(pr, O.prDrop1);
    bool full = valid(hand) && ic.activeSelf && ic.activeSelf(hand);
    if (full) {
        gHandsWereFull = true;
        gGrabTarget = nullptr;
        gGrabTries = 0;
        return;
    }
    if (gHandsWereFull) {  // just dropped something: let it fall away before looking for the next one
        gHandsWereFull = false;
        gGrabNext = now + 600;
    }
    if (now < gGrabNext || !promptShown(pr)) return;

    Cam cam;
    if (!getCam(cam)) return;
    float reach = O.prReach ? *P<float>(pr, O.prReach) : 3.f;
    if (reach < 0.5f) reach = 0.5f;

    Il2CppObject* best = nullptr;
    float bestDot = 0.985f;  // about 10 degrees around the crosshair
    for (auto& kv : gItems) {
        Il2CppObject* it = kv.first;
        if (!valid(it) || !activeIn(it)) continue;
        auto sk = gGrabSkip.find(it);
        if (sk != gGrabSkip.end()) {
            if (now < sk->second) continue;
            gGrabSkip.erase(sk);
        }
        float p[3];
        if (!position(it, p)) continue;
        float dx = p[0] - cam.pos[0], dy = p[1] - cam.pos[1], dz = p[2] - cam.pos[2];
        float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d < 0.05f || d > reach + 0.5f) continue;
        float dot = (dx * cam.fwd[0] + dy * cam.fwd[1] + dz * cam.fwd[2]) / d;
        if (dot > bestDot) {
            bestDot = dot;
            best = it;
        }
    }
    if (!best) {
        gGrabTarget = nullptr;
        gGrabTries = 0;
        return;
    }
    if (best != gGrabTarget) {
        gGrabTarget = best;
        gGrabTries = 0;
        gGrabSince = now;
    }
    if (now - gGrabSince < 60) return;
    if (++gGrabTries > 5) {  // it does not want to be picked up (locked, held by something): leave it for a while
        gGrabSkip[best] = now + 8000;
        gGrabTarget = nullptr;
        gGrabTries = 0;
        return;
    }
    gGrabNext = now + 150 * gGrabTries;
    invoke(mClick, pr, nullptr);
}

// ---------------------------------------------------------------- the player's update
void playerTick(Il2CppObject* fps) {
    int64_t now = nowMs();
    if (fps != gFps) {
        gFps = fps;
        gReach.clear();
        gAmmoMax = gSprayMax = 0.f;
        gGrabTarget = nullptr;
        gGrabSkip.clear();
        gNoclipOn = false;
        logf("cheat: player found");
    }
    gFpsAt = now;
    gPlayerCc = ref(fps, O.fpsCc);
    Il2CppObject* ps = ref(fps, O.fpsPs);
    gPs = ps;
    Il2CppObject* pr = ref(ps, O.psPick);
    gPr = valid(pr) ? pr : nullptr;

    Il2CppObject* cam = ref(ps, O.psCam);
    if (!valid(cam)) cam = ref(fps, O.fpsCam2);
    if (!valid(cam)) cam = ref(fps, O.fpsCam);
    gCamObj = valid(cam) ? cam : nullptr;

    // no clip: the controller is switched off while it is on, and switched back on afterwards
    Il2CppObject* fh = ref(fps, O.fpsFh);
    bool canNoclip = valid(gPlayerCc) && O.fpsMoveDir && ic.colEnabled && ic.getTransform && ic.getPos && ic.setPos;
    if (gC.noclip && canNoclip) {
        if (!gNoclipOn) {
            gNoclipOn = true;
            ic.colEnabled(gPlayerCc, false);
            logf("cheat: no clip on");
        }
        suppressFall(fh);
        *P<float>(fps, O.fpsMoveDir + 4) = 0.f;  // the game's gravity must not pile up while nothing is touching the ground
    } else if (gNoclipOn) {
        gNoclipOn = false;
        gNoclipOffAt = now;
        if (valid(gPlayerCc) && ic.colEnabled) ic.colEnabled(gPlayerCc, true);
        suppressFall(fh);
        *P<float>(fps, O.fpsMoveDir + 4) = 0.f;
        logf("cheat: no clip off");
    } else if (now - gNoclipOffAt < 2500) {
        suppressFall(fh);
    }

    if (!gPr) return;

    if (gC.infAmmo && O.prAmmo) {
        float* a = P<float>(gPr, O.prAmmo);
        if (*a > gAmmoMax) gAmmoMax = *a;
        else if (*a < gAmmoMax) *a = gAmmoMax;
    } else {
        gAmmoMax = 0.f;
    }
    if (gC.infSpray && O.prSpray) {
        float* s = P<float>(gPr, O.prSpray);
        if (*s > gSprayMax) gSprayMax = *s;
        else if (*s < gSprayMax) *s = gSprayMax;
    } else {
        gSprayMax = 0.f;
    }

    float reach = gC.reach < 2.f ? 2.f : (gC.reach > 40.f ? 40.f : gC.reach);
    reachApply(gPr, O.prReach, gC.longReachOn, reach);

    if (gSpawnPending) {
        gSpawnPending = false;
        const char* why = "";
        char msg[72];
        int i = gSpawnIndex < 0 ? 0 : (gSpawnIndex >= kItemCount ? kItemCount - 1 : gSpawnIndex);
        if (spawnItem(gPr, i, &why)) snprintf(msg, sizeof(msg), "Spawned: %s", kItemNames[i]);
        else snprintf(msg, sizeof(msg), "Spawn failed: %s", why);
        logf("cheat: %s", msg);
        postToast(msg);
    }
    autoGrab(gPr, now);
}

void (*gOrigFps)(void*, void*) = nullptr;
void fpsHook(void* self, void* mi) {
    if (!gOrigFps) return;
    if (self) playerTick(reinterpret_cast<Il2CppObject*>(self));
    gOrigFps(self, mi);
}

// ---------------------------------------------------------------- ESP
bool gEspPublished = false;
int64_t gEspAt = 0;

void espTick(int64_t now) {
    if (now < gEspAt) return;
    gEspAt = now + 33;
    if (!gC.espEnemy && !gC.espItem) {
        if (gEspPublished) {
            publishEsp({});
            gEspPublished = false;
        }
        return;
    }
    Cam cam;
    if (!getCam(cam)) return;
    std::vector<EspMark> marks;
    marks.reserve(48);

    auto add = [&](const float* foot, float h, float wRatio, float maxDist, bool enemy, const char* name) {
        float dx = foot[0] - cam.pos[0], dy = foot[1] - cam.pos[1], dz = foot[2] - cam.pos[2];
        float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d > maxDist || marks.size() >= 96) return;
        float head[3] = {foot[0], foot[1] + h, foot[2]};
        float fx, fy, hx, hy;
        if (!project(cam, foot, &fx, &fy) || !project(cam, head, &hx, &hy)) return;
        float bh = std::fabs(fy - hy);
        if (bh < 0.003f) bh = 0.003f;
        EspMark m;
        m.x = fx;
        m.y = (fy + hy) * 0.5f;
        m.h = bh;
        m.w = bh * wRatio / cam.aspect;
        m.dist = d;
        m.enemy = enemy;
        snprintf(m.name, sizeof(m.name), "%s", name);
        if (m.x < -0.3f || m.x > 1.3f || m.y < -0.3f || m.y > 1.3f) return;
        marks.push_back(m);
    };

    if (gC.espEnemy) {
        for (int k = 0; k < kEnemyCount; k++) {
            for (auto& kv : gSt[k]) {
                if (kv.second.dead || now - kv.second.seenAt > 1500 || !valid(kv.first) || !activeIn(kv.first)) continue;
                float p[3];
                if (position(kv.first, p)) add(p, kEspH[k], kEspW[k], gC.espEnemyMax, true, kEnemyNames[k]);
            }
        }
    }
    if (gC.espItem) {
        for (auto& kv : gItems) {
            if (!valid(kv.first) || !activeIn(kv.first)) continue;
            float p[3];
            if (!position(kv.first, p)) continue;
            p[1] -= 0.12f;
            add(p, 0.32f, 1.0f, gC.espItemMax, false, kv.second.name);
        }
    }
    publishEsp(marks);
    gEspPublished = true;
}

// ---------------------------------------------------------------- housekeeping
int64_t gPruneAt = 0;
void prune(int64_t now) {
    if (now < gPruneAt) return;
    gPruneAt = now + 2000;
    for (int k = 0; k < kEnemyCount; k++) {
        for (auto it = gSt[k].begin(); it != gSt[k].end();) {
            if (!valid(it->first)) {  // destroyed (new scene)
                if (gSeen[k] == it->first) gSeen[k] = nullptr;
                release(it->second.handle);
                it = gSt[k].erase(it);
            } else {
                ++it;
            }
        }
    }
    if (gFps && !valid(gFps)) {
        gFps = gPs = gPr = gCamObj = gPlayerCc = nullptr;
        gReach.clear();
    }
}

int64_t gStatusAt = 0;
std::string gStatusLast;
void status(int64_t now) {
    if (now < gStatusAt) return;
    gStatusAt = now + 500;
    char buf[200];
    std::string t;
    if (gHookOk == gHookTotal) snprintf(buf, sizeof(buf), "Cheats ready: all %d hooks installed.", gHookTotal);
    else snprintf(buf, sizeof(buf), "Cheats: %d of %d hooks installed.", gHookOk, gHookTotal);
    t = buf;
    if (gHookOk != gHookTotal && !gFailed.empty()) {
        t += " Missing: " + gFailed[0];
        if (gFailed.size() > 1) t += " +" + std::to_string(gFailed.size() - 1);
        t += ".";
    }
    bool inGame = gFps && now - gFpsAt < 1000;
    t += inGame ? "\nPlayer found." : "\nPlayer not found yet: start a level.";
    if (gC.noclip && !O.fpsMoveDir) t += " No clip is not available (moveDirection not found).";
    if (gC.godMode && !gGodHooksOk) t += " God mode is incomplete (hooks missing).";
    if (gC.espEnemy || gC.espItem) {
        Cam c;
        if (!getCam(c)) t += " ESP is waiting for the camera.";
    }
    if (t != gStatusLast) {
        gStatusLast = t;
        setCheatStatus(t);
    }
}

// ---------------------------------------------------------------- log what the menu changed (helps to see what reaches the game)
CheatSettings gPrev;
bool gFirst = true;
const char* onOff(bool v) { return v ? "on" : "off"; }
void note(const char* what, bool on) { logf("cheat: %s -> %s", what, onOff(on)); }

void logChanges(const CheatSettings& s) {
    if (s.godMode != gPrev.godMode) note("god mode", s.godMode);
    if (s.speedOn != gPrev.speedOn || s.speed != gPrev.speed) logf("cheat: speed -> %s x%.1f", onOff(s.speedOn), s.speed);
    if (s.noclip != gPrev.noclip) note("no clip", s.noclip);
    if (s.noFallDamage != gPrev.noFallDamage) note("no fall damage", s.noFallDamage);
    if (s.noCrackFloor != gPrev.noCrackFloor) note("no cracking floor", s.noCrackFloor);
    if (s.infAmmo != gPrev.infAmmo) note("infinite ammo", s.infAmmo);
    if (s.infSpray != gPrev.infSpray) note("infinite spray", s.infSpray);
    if (s.noTrap != gPrev.noTrap) note("no trap", s.noTrap);
    for (int i = 0; i < kEnemyCount; i++) {
        char what[48];
        if (s.enemy[i].freeze != gPrev.enemy[i].freeze) {
            snprintf(what, sizeof(what), "%s freeze", kEnemyNames[i]);
            note(what, s.enemy[i].freeze);
        }
        if (s.enemy[i].blind != gPrev.enemy[i].blind) {
            snprintf(what, sizeof(what), "%s blind", kEnemyNames[i]);
            note(what, s.enemy[i].blind);
        }
        if (s.enemy[i].deaf != gPrev.enemy[i].deaf) {
            snprintf(what, sizeof(what), "%s deaf", kEnemyNames[i]);
            note(what, s.enemy[i].deaf);
        }
    }
    if (s.autoGrab != gPrev.autoGrab) note("auto grab", s.autoGrab);
    if (s.longReachOn != gPrev.longReachOn || s.reach != gPrev.reach) logf("cheat: long reach -> %s %.0f m", onOff(s.longReachOn), s.reach);
    if (s.espEnemy != gPrev.espEnemy) note("enemy ESP", s.espEnemy);
    if (s.espItem != gPrev.espItem) note("item ESP", s.espItem);
}

}  // namespace

// ---------------------------------------------------------------- install
void install(HookInstaller hook) {
    resolve();

    auto H = [&](Il2CppClass* k, const char* cls, const char* method, int argc, void* repl, void** orig) -> bool {
        gHookTotal++;
        std::string what = std::string(cls) + "." + method;
        const MethodInfo* m = findMethod(k, method, argc);
        if (!m) {
            logf("cheat: %s not found", what.c_str());
            gFailed.push_back(what);
            return false;
        }
        if (!hook(fnPtr(m), repl, orig, what.c_str())) {
            gFailed.push_back(what);
            return false;
        }
        gHookOk++;
        return true;
    };

    // player
    H(cFps, "MobileFPS", "Update", 0, (void*)fpsHook, (void**)&gOrigFps);
    Il2CppClass* cCc = findClass("UnityEngine", "CharacterController");
    H(cCc, "CharacterController", "Move", 1, (void*)hookMove, (void**)&gOrigMove);
    H(cFall, "FallingHolder", "Update", 0, (void*)fallHook<0>, (void**)&gOrigFall[0]);
    H(cFall, "FallingHolder", "CheckForLanding", 0, (void*)fallHook<1>, (void**)&gOrigFall[1]);
    H(cFall, "FallingHolder", "HandleLanding", 0, (void*)fallHook<2>, (void**)&gOrigFall[2]);

    // god mode: all nine have to be there
    void* psRepl[9] = {(void*)psHook<0>, (void*)psHook<1>, (void*)psHook<2>, (void*)psHook<3>, (void*)psHook<4>,
                       (void*)psHook<5>, (void*)psHook<6>, (void*)psHook<7>, (void*)psHook<8>};
    for (int i = 0; i < 9; i++)
        if (!H(cPs, "PlayerStatus", kPsNames[i], 0, psRepl[i], (void**)&gOrigPs[i])) gGodHooksOk = false;

    // traps and the cracking floor
    H(findClass("", "TrapTrigger"), "TrapTrigger", "OnTriggerEnter", 1, (void*)trapHook<0>, (void**)&gOrigTrap[0]);
    H(findClass("", "BearTrapLogic"), "BearTrapLogic", "OnTriggerEnter", 1, (void*)trapHook<1>, (void**)&gOrigTrap[1]);
    H(findClass("", "ExploTrapTrigger"), "ExploTrapTrigger", "OnTriggerEnter", 1, (void*)trapHook<2>, (void**)&gOrigTrap[2]);
    H(findClass("", "TrapPoison"), "TrapPoison", "OnTriggerEnter", 1, (void*)trapHook<3>, (void**)&gOrigTrap[3]);
    H(findClass("", "TriggerCreakNoise"), "TriggerCreakNoise", "OnTriggerEnter", 1, (void*)trapHook<4>, (void**)&gOrigTrap[4]);

    // enemies, one update each
    void* enRepl[kEnemyCount] = {(void*)enemyHook<0>, (void*)enemyHook<1>, (void*)enemyHook<2>, (void*)enemyHook<3>,
                                 (void*)enemyHook<4>, (void*)enemyHook<5>, (void*)enemyHook<6>};
    for (int k = 0; k < kEnemyCount; k++) H(gEnemyCls[k], kSpec[k].cls, kSpec[k].method, 0, enRepl[k], (void**)&gOrigEnemy[k]);
    H(cGo, "GameObject", "FindGameObjectWithTag", 1, (void*)hookFindTag, (void**)&gOrigFindTag);

    logf("cheat: %d of %d hooks installed", gHookOk, gHookTotal);
}

// ---------------------------------------------------------------- tick
void tick(const CheatSettings& s) {
    if (gFirst) {
        gFirst = false;
        logf("cheat: ready");
    }
    gC = s;
    logChanges(s);

    // one-shot buttons
    for (int k = 0; k < kEnemyCount; k++) {
        if (s.killReq[k] == gKillAsked[k]) continue;
        gKillAsked[k] = s.killReq[k];
        if (gSeen[k] && nowMs() - gSeenAt[k] < 1500 && valid(gSeen[k])) {
            gKillPending[k] = true;  // done at the enemy's next update, a safe moment
        } else {
            char msg[64];
            snprintf(msg, sizeof(msg), "%s is not in this level", kEnemyNames[k]);
            logf("cheat: %s", msg);
            postToast(msg);
        }
    }
    if (s.spawnReq != gSpawnAsked) {
        gSpawnAsked = s.spawnReq;
        if (gFps && nowMs() - gFpsAt < 1000) {
            gSpawnPending = true;
            gSpawnIndex = s.spawnItem;
        } else {
            logf("cheat: spawn: no player yet");
            postToast("Spawn: start a level first");
        }
    }
    gPrev = s;

    if (!gResolved) return;
    int64_t now = nowMs();

    // DoorRay / RayRemoveTraps: their ray length follows the long reach switch too
    if (s.longReachOn || !gReach.empty()) {
        scanRayObjs(now);
        float reach = s.reach < 2.f ? 2.f : (s.reach > 40.f ? 40.f : s.reach);
        for (size_t i = 0; i < gRayObjs.size(); i++) {
            if (!valid(gRayObjs[i])) continue;
            reachApply(gRayObjs[i], gRayKind[i] == 0 ? O.doorReach : O.trapReach, s.longReachOn, reach);
        }
    } else if (!gRayObjs.empty()) {
        clearRayObjs();
    }

    scanItems(now);
    espTick(now);
    prune(now);
    status(now);
}

}  // namespace cheat
}  // namespace vm
