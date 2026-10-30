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
//   ESP             positions are projected to the screen here (main thread) and drawn by the menu (render thread).
//                   The outline is the real shape: the mesh of the object is projected and the edges where the surface
//                   turns away from the camera are kept. A mesh the game does not allow to be read gets a box instead.

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
    void (*l2w)(void*, float*) = nullptr;          // Transform.localToWorldMatrix
    void (*bounds)(void*, float*) = nullptr;       // Renderer.bounds: centre xyz, extents xyz
    void (*bake)(void*, void*, bool) = nullptr;    // SkinnedMeshRenderer.BakeMesh(mesh, useScale)
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

Il2CppClass *cMesh, *cMeshFilter, *cSmr;
const MethodInfo *mMeshVerts, *mMeshTris, *mMeshReadable, *mMfShared, *mSmrShared, *mMeshCtor;
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

// ---------------------------------------------------------------- shapes for the ESP
// The welded mesh of one Mesh object (vertices at the same place are one vertex, so neighbouring triangles share edges even
// where the model has texture seams). Built once, then reused by every object that uses the mesh.
struct MeshData {
    uint32_t handle = 0;
    int nOrig = 0;               // vertices of the original mesh
    std::vector<int> rep;        // welded vertex -> a vertex of the original mesh
    std::vector<float> pos;      // welded positions, xyz (what a static mesh needs every frame)
    std::vector<int> tri;        // welded indices, 3 per triangle
    struct Edge {
        int a, b, t0, t1;        // t1 = -1: the edge belongs to one triangle only
    };
    std::vector<Edge> edges;
    bool ok = false;
};
std::unordered_map<Il2CppObject*, MeshData> gMeshes;

struct Part {                    // one MeshFilter or SkinnedMeshRenderer of an object
    Il2CppObject* comp = nullptr;
    uint32_t handle = 0;
    Il2CppObject* mesh = nullptr;  // key into gMeshes
    bool skinned = false;
    Il2CppObject* bake = nullptr;  // scratch mesh the current pose is baked into (skinned only)
    uint32_t bakeHandle = 0;
    int mode = 0;                  // skinned: 1 = the baked mesh needs the full matrix, 2 = without the scale (decided once)
};
struct Outline {
    std::vector<Part> parts;
    bool built = false;
    int64_t retryAt = 0;
};
void freeOutline(Outline& o) {
    for (Part& p : o.parts) {
        if (p.handle) release(p.handle);
        if (p.bakeHandle) release(p.bakeHandle);
    }
    o.parts.clear();
    o.built = false;
}

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
    Outline ol;
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

    ic.l2w = reinterpret_cast<void (*)(void*, float*)>(icall("UnityEngine.Transform::get_localToWorldMatrix_Injected"));
    ic.bounds = reinterpret_cast<void (*)(void*, float*)>(icall("UnityEngine.Renderer::get_bounds_Injected"));
    ic.bake = reinterpret_cast<void (*)(void*, void*, bool)>(icall("UnityEngine.SkinnedMeshRenderer::BakeMesh"));
    cMesh = findClass("UnityEngine", "Mesh");
    cMeshFilter = findClass("UnityEngine", "MeshFilter");
    cSmr = findClass("UnityEngine", "SkinnedMeshRenderer");
    mMeshVerts = findMethod(cMesh, "get_vertices", 0);
    mMeshTris = findMethod(cMesh, "get_triangles", 0);
    mMeshReadable = findMethod(cMesh, "get_isReadable", 0);
    mMeshCtor = findMethod(cMesh, ".ctor", 0);
    mMfShared = findMethod(cMeshFilter, "get_sharedMesh", 0);
    mSmrShared = findMethod(cSmr, "get_sharedMesh", 0);
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
    O.isCount = fieldChecked(cItemSpawn, "ItemSpawn", "CountItem", kTypeR4);  // a float in this game
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
    if (!(mInstantiate && mGetComp && O.prItemDrop && O.prDropP && O.isCount))
        logf("cheat: spawn is missing:%s%s%s%s%s", mInstantiate ? "" : " Instantiate", mGetComp ? "" : " GetComponent",
             O.prItemDrop ? "" : " ItemDrop", O.prDropP ? "" : " DropP", O.isCount ? "" : " CountItem");
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
    Outline ol;
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
            for (auto& kv : gItems) {
                freeOutline(kv.second.ol);
                release(kv.second.handle);
            }
            gItems.clear();
        }
        return;
    }
    if (now < gItemScanAt || !cSeed) return;
    gItemScanAt = now + (gC.autoGrab ? 250 : 1000);
    for (auto it = gItems.begin(); it != gItems.end();) {
        if (!valid(it->first)) {
            freeOutline(it->second.ol);
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
        *P<float>(c2, O.isCount) = (float)(index + 1);
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
    float v[16], p[16], vp[16];
    float pos[3] = {0, 0, 0};
    float fwd[3] = {0, 0, 1};
    float aspect = 1.f;
};

void mul4(const float* a, const float* b, float* r) {  // column-major: r = a * b
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++) {
            float sum = 0.f;
            for (int k = 0; k < 4; k++) sum += a[k * 4 + row] * b[c * 4 + k];
            r[c * 4 + row] = sum;
        }
}

bool getCam(Cam& c) {
    Il2CppObject* cam = gCamObj;
    if (!valid(cam) || nowMs() - gFpsAt > 500 || !ic.camW2C || !ic.camProj || !ic.getTransform || !ic.getPos || !ic.getRot) return false;
    ic.camW2C(cam, c.v);
    ic.camProj(cam, c.p);
    mul4(c.p, c.v, c.vp);
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

// ---------------------------------------------------------------- the shape of an object
struct Q {
    int x, y, z;
    bool operator==(const Q& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct QHash {
    size_t operator()(const Q& q) const {
        return (size_t)((uint32_t)q.x * 73856093u ^ (uint32_t)q.y * 19349663u ^ (uint32_t)q.z * 83492791u);
    }
};
int gUnreadable = 0;

MeshData* meshData(Il2CppObject* mesh) {
    auto it = gMeshes.find(mesh);
    if (it != gMeshes.end()) return it->second.ok ? &it->second : nullptr;
    MeshData& md = gMeshes[mesh];
    md.handle = retain(mesh);
    if (!mMeshVerts || !mMeshTris || !mMeshReadable || !callBool(mMeshReadable, mesh, nullptr, false)) {
        if (gUnreadable++ < 4) logf("cheat: ESP: a mesh is not readable, a box is drawn for it");
        return nullptr;
    }
    Il2CppObject* va = invoke(mMeshVerts, mesh, nullptr);
    Il2CppObject* ta = invoke(mMeshTris, mesh, nullptr);
    if (!va || !ta) return nullptr;
    size_t nv = A.array_length(va), nt = A.array_length(ta);
    if (nv < 3 || nt < 3 || nv > 30000 || nt > 60000) return nullptr;  // too heavy to outline every frame
    const float* v = reinterpret_cast<const float*>(arrayData(va));
    const int* t = reinterpret_cast<const int*>(arrayData(ta));
    md.nOrig = (int)nv;

    std::unordered_map<Q, int, QHash> weld;
    std::vector<int> w(nv);
    for (size_t i = 0; i < nv; i++) {
        Q q{(int)std::lround(v[i * 3] * 1e4f), (int)std::lround(v[i * 3 + 1] * 1e4f), (int)std::lround(v[i * 3 + 2] * 1e4f)};
        auto f = weld.find(q);
        if (f == weld.end()) {
            f = weld.emplace(q, (int)md.rep.size()).first;
            md.rep.push_back((int)i);
            md.pos.push_back(v[i * 3]);
            md.pos.push_back(v[i * 3 + 1]);
            md.pos.push_back(v[i * 3 + 2]);
        }
        w[i] = f->second;
    }
    std::unordered_map<uint64_t, int> edgeAt;
    for (size_t i = 0; i + 2 < nt; i += 3) {
        if (t[i] < 0 || t[i + 1] < 0 || t[i + 2] < 0 || (size_t)t[i] >= nv || (size_t)t[i + 1] >= nv || (size_t)t[i + 2] >= nv) continue;
        int a = w[t[i]], b = w[t[i + 1]], c = w[t[i + 2]];
        if (a == b || b == c || a == c) continue;  // collapsed by the welding
        int ti = (int)(md.tri.size() / 3);
        md.tri.push_back(a);
        md.tri.push_back(b);
        md.tri.push_back(c);
        int e[3][2] = {{a, b}, {b, c}, {c, a}};
        for (auto& pr : e) {
            int lo = pr[0] < pr[1] ? pr[0] : pr[1], hi = pr[0] < pr[1] ? pr[1] : pr[0];
            uint64_t key = ((uint64_t)(uint32_t)lo << 32) | (uint32_t)hi;
            auto f = edgeAt.find(key);
            if (f == edgeAt.end()) {
                edgeAt.emplace(key, (int)md.edges.size());
                md.edges.push_back({lo, hi, ti, -1});
            } else if (md.edges[f->second].t1 == -1) {
                md.edges[f->second].t1 = ti;
            }
        }
    }
    md.ok = !md.tri.empty();
    return md.ok ? &md : nullptr;
}

void buildOutline(Il2CppObject* owner, Outline& ol, int64_t now) {
    ol.built = true;
    Il2CppObject* go = ic.getGameObject ? ic.getGameObject(owner) : nullptr;
    if (!go || !mGetCompsInChildren) return;
    for (int pass = 0; pass < 2; pass++) {
        Il2CppClass* k = pass == 0 ? cMeshFilter : cSmr;
        if (!k || !(pass == 0 ? mMfShared : mSmrShared) || !mMeshVerts) continue;
        bool inactive = false;
        void* a[2] = {typeObject(k), &inactive};
        Il2CppObject* arr = invoke(mGetCompsInChildren, go, a);
        if (!arr) continue;
        size_t n = A.array_length(arr);
        Il2CppObject** el = arrayData(arr);
        for (size_t i = 0; i < n && ol.parts.size() < 8; i++) {
            Il2CppObject* comp = el[i];
            if (!valid(comp)) continue;
            Il2CppObject* mesh = invoke(pass == 0 ? mMfShared : mSmrShared, comp, nullptr);
            if (!valid(mesh) || !meshData(mesh)) continue;
            Part p;
            p.comp = comp;
            p.handle = retain(comp);
            p.mesh = mesh;
            p.skinned = pass == 1;
            if (p.skinned) {
                if (!ic.bake || !mMeshCtor || !A.object_new) {
                    release(p.handle);
                    continue;
                }
                Il2CppObject* bm = A.object_new(cMesh);
                invoke(mMeshCtor, bm, nullptr);
                p.bake = bm;
                p.bakeHandle = retain(bm);
            }
            ol.parts.push_back(p);
        }
    }
    if (ol.parts.empty()) {
        ol.built = false;
        ol.retryAt = now + 5000;  // maybe it was not set up yet
    }
}

// A baked mesh comes with or without the scale of the object in it (the documentation is vague, so it is measured once):
// the way of placing it that puts it where the renderer says the object is wins.
int chooseBakeMode(Il2CppObject* comp, const float* L, const float* verts, int n) {
    float rb[6];
    if (!ic.bounds) return 2;
    ic.bounds(comp, rb);
    float noScale[16];
    memcpy(noScale, L, sizeof(noScale));
    for (int c = 0; c < 3; c++) {
        float len = std::sqrt(L[c * 4] * L[c * 4] + L[c * 4 + 1] * L[c * 4 + 1] + L[c * 4 + 2] * L[c * 4 + 2]);
        if (len > 1e-6f)
            for (int r = 0; r < 3; r++) noScale[c * 4 + r] = L[c * 4 + r] / len;
    }
    float best[2] = {0, 0};
    for (int m = 0; m < 2; m++) {
        const float* M = m == 0 ? L : noScale;
        float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
        int step = n > 400 ? n / 400 : 1;
        for (int i = 0; i < n; i += step) {
            const float* v = verts + i * 3;
            for (int a = 0; a < 3; a++) {
                float w = M[a] * v[0] + M[4 + a] * v[1] + M[8 + a] * v[2] + M[12 + a];
                if (w < mn[a]) mn[a] = w;
                if (w > mx[a]) mx[a] = w;
            }
        }
        float sc = 0.f;
        for (int a = 0; a < 3; a++) sc += std::fabs((mn[a] + mx[a]) * 0.5f - rb[a]) + std::fabs((mx[a] - mn[a]) * 0.5f - rb[3 + a]);
        best[m] = sc;
    }
    return best[0] <= best[1] ? 1 : 2;
}

// Adds the outline of the object to `lines` (x1 y1 x2 y2 per segment). bb = the screen box of what was drawn.
bool emitOutline(const Cam& cam, Outline& ol, std::vector<float>& lines, size_t cap, float* bb) {
    static std::vector<float> wp, sx, sy;
    static std::vector<signed char> face, okv;
    bool any = false;
    bb[0] = bb[1] = 1e9f;
    bb[2] = bb[3] = -1e9f;
    for (Part& p : ol.parts) {
        if (lines.size() >= cap * 4) break;
        if (!valid(p.comp)) continue;
        auto mit = gMeshes.find(p.mesh);
        if (mit == gMeshes.end() || !mit->second.ok) continue;
        MeshData& md = mit->second;
        Il2CppObject* tr = ic.getTransform(p.comp);
        if (!tr || !ic.l2w) continue;
        float L[16];
        ic.l2w(tr, L);
        const float* pos = md.pos.data();
        if (p.skinned) {
            ic.bake(p.comp, p.bake, true);
            Il2CppObject* va = invoke(mMeshVerts, p.bake, nullptr);
            if (!va || (int)A.array_length(va) != md.nOrig) continue;
            const float* v = reinterpret_cast<const float*>(arrayData(va));
            if (p.mode == 0) p.mode = chooseBakeMode(p.comp, L, v, md.nOrig);
            if (p.mode == 2) {
                for (int c = 0; c < 3; c++) {
                    float len = std::sqrt(L[c * 4] * L[c * 4] + L[c * 4 + 1] * L[c * 4 + 1] + L[c * 4 + 2] * L[c * 4 + 2]);
                    if (len > 1e-6f)
                        for (int r = 0; r < 3; r++) L[c * 4 + r] /= len;
                }
            }
            wp.resize(md.rep.size() * 3);
            for (size_t i = 0; i < md.rep.size(); i++) {
                wp[i * 3] = v[md.rep[i] * 3];
                wp[i * 3 + 1] = v[md.rep[i] * 3 + 1];
                wp[i * 3 + 2] = v[md.rep[i] * 3 + 2];
            }
            pos = wp.data();
        }
        float M[16];
        mul4(cam.vp, L, M);
        size_t nvw = md.rep.size();
        sx.resize(nvw);
        sy.resize(nvw);
        okv.resize(nvw);
        for (size_t i = 0; i < nvw; i++) {
            float x = pos[i * 3], y = pos[i * 3 + 1], z = pos[i * 3 + 2];
            float cw = M[3] * x + M[7] * y + M[11] * z + M[15];
            if (cw < 0.05f) {
                okv[i] = 0;
                continue;
            }
            okv[i] = 1;
            sx[i] = (M[0] * x + M[4] * y + M[8] * z + M[12]) / cw * 0.5f + 0.5f;
            sy[i] = 0.5f - (M[1] * x + M[5] * y + M[9] * z + M[13]) / cw * 0.5f;
        }
        size_t ntri = md.tri.size() / 3;
        face.resize(ntri);
        for (size_t t = 0; t < ntri; t++) {
            int a = md.tri[t * 3], b = md.tri[t * 3 + 1], c = md.tri[t * 3 + 2];
            if (!okv[a] || !okv[b] || !okv[c]) {
                face[t] = 0;
                continue;
            }
            float area = (sx[b] - sx[a]) * (sy[c] - sy[a]) - (sx[c] - sx[a]) * (sy[b] - sy[a]);
            face[t] = area > 0.f ? 1 : -1;  // which side is "front" does not matter: the silhouette is where the two meet
        }
        for (const MeshData::Edge& e : md.edges) {
            signed char f0 = face[e.t0];
            if (f0 == 0) continue;
            if (e.t1 >= 0) {
                signed char f1 = face[e.t1];
                if (f1 == 0 || f1 == f0) continue;
            }
            if (lines.size() >= cap * 4) break;
            lines.push_back(sx[e.a]);
            lines.push_back(sy[e.a]);
            lines.push_back(sx[e.b]);
            lines.push_back(sy[e.b]);
            for (int v : {e.a, e.b}) {
                if (sx[v] < bb[0]) bb[0] = sx[v];
                if (sy[v] < bb[1]) bb[1] = sy[v];
                if (sx[v] > bb[2]) bb[2] = sx[v];
                if (sy[v] > bb[3]) bb[3] = sy[v];
            }
            any = true;
        }
    }
    return any;
}

// ---------------------------------------------------------------- auto grab
Il2CppObject* gGrabTarget = nullptr;
int64_t gGrabNext = 0;
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

// Auto grab: the moment the game itself shows the pick-up prompt (that is also when it shows the item's name) and an item
// is in front of you, the interact button is pressed. No waiting: it is tried on every frame until the hand is full.
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
    if (gHandsWereFull) {  // you just let go of something: a quarter of a second so it is not picked up again at once
        gHandsWereFull = false;
        gGrabNext = now + 250;
    }
    if (now < gGrabNext || !promptShown(pr)) {
        gGrabTarget = nullptr;
        gGrabTries = 0;
        return;
    }

    Cam cam;
    if (!getCam(cam)) return;
    float reach = O.prReach ? *P<float>(pr, O.prReach) : 3.f;
    if (reach < 0.5f) reach = 0.5f;

    Il2CppObject* best = nullptr;
    float bestDot = 0.87f;  // about 30 degrees: the prompt already says the game is aiming at something, this only picks which item
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
        if (d < 0.05f || d > reach + 1.0f) continue;
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
    }
    if (++gGrabTries > 40) {  // 40 frames in a row and nothing in the hand: it cannot be picked up (locked, held): leave it alone for a while
        gGrabSkip[best] = now + 3000;
        gGrabTarget = nullptr;
        gGrabTries = 0;
        return;
    }
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
            publishEsp({}, {});
            gEspPublished = false;
        }
        return;
    }
    Cam cam;
    if (!getCam(cam)) return;
    std::vector<EspMark> marks;
    std::vector<float> lines;
    marks.reserve(48);
    lines.reserve(4096);
    int builds = 0;  // outlines are built a couple per frame, so nothing stutters when many things appear at once

    // owner: the component of the object; ol: where its shape is kept; wantShape: the Outline switch; shapeDist: how far shapes are drawn
    auto add = [&](Il2CppObject* owner, Outline& ol, bool wantShape, float shapeDist, const float* foot, float h, float wRatio,
                   float maxDist, bool enemy, const char* name) {
        float dx = foot[0] - cam.pos[0], dy = foot[1] - cam.pos[1], dz = foot[2] - cam.pos[2];
        float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d > maxDist || marks.size() >= 96) return;
        EspMark m;
        m.dist = d;
        m.enemy = enemy;
        snprintf(m.name, sizeof(m.name), "%s", name);

        bool shape = false;
        float bb[4];
        if (wantShape && d <= shapeDist && lines.size() < 6000 * 4) {
            if (!ol.built && now >= ol.retryAt && builds < 2) {
                builds++;
                buildOutline(owner, ol, now);
            }
            if (!ol.parts.empty()) {
                size_t start = lines.size();
                if (emitOutline(cam, ol, lines, 1500, bb)) {
                    shape = true;
                    m.lineStart = (int)(start / 4);
                    m.lineCount = (int)((lines.size() - start) / 4);
                    m.x = (bb[0] + bb[2]) * 0.5f;
                    m.y = (bb[1] + bb[3]) * 0.5f;
                    m.w = bb[2] - bb[0];
                    m.h = bb[3] - bb[1];
                } else {
                    lines.resize(start);
                }
            }
        }
        if (!shape) {
            float head[3] = {foot[0], foot[1] + h, foot[2]};
            float fx, fy, hx, hy;
            if (!project(cam, foot, &fx, &fy) || !project(cam, head, &hx, &hy)) return;
            float bh = std::fabs(fy - hy);
            if (bh < 0.003f) bh = 0.003f;
            m.x = fx;
            m.y = (fy + hy) * 0.5f;
            m.h = bh;
            m.w = bh * wRatio / cam.aspect;
        }
        if (m.x < -0.3f || m.x > 1.3f || m.y < -0.3f || m.y > 1.3f) return;
        marks.push_back(m);
    };

    if (gC.espEnemy) {
        for (int k = 0; k < kEnemyCount; k++) {
            for (auto& kv : gSt[k]) {
                if (kv.second.dead || now - kv.second.seenAt > 1500 || !valid(kv.first) || !activeIn(kv.first)) continue;
                float p[3];
                if (position(kv.first, p))
                    add(kv.first, kv.second.ol, gC.espEnemyOutline, 90.f, p, kEspH[k], kEspW[k], gC.espEnemyMax, true, kEnemyNames[k]);
            }
        }
    }
    if (gC.espItem) {
        for (auto& kv : gItems) {
            if (!valid(kv.first) || !activeIn(kv.first)) continue;
            float p[3];
            if (!position(kv.first, p)) continue;
            p[1] -= 0.12f;
            add(kv.first, kv.second.ol, gC.espItemOutline, 40.f, p, 0.32f, 1.0f, gC.espItemMax, false, kv.second.name);
        }
    }
    publishEsp(marks, lines);
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
                freeOutline(it->second.ol);
                release(it->second.handle);
                it = gSt[k].erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto it = gMeshes.begin(); it != gMeshes.end();) {  // meshes of a level that is gone
        if (!valid(it->first)) {
            release(it->second.handle);
            it = gMeshes.erase(it);
        } else {
            ++it;
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
