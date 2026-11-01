#include "shared.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "log.h"

namespace vm {

static std::mutex gMutex;
static FxSettings gFx;
static unsigned gFxVersion = 1;
static MainStatus gStatus;
static Touch gTouch;
static SplitsView gSplits;


std::atomic<bool> gBlockTouch{false};
std::atomic<int> gGameState{0};
std::atomic<int64_t> gPlayerSeenMs{0};
static std::atomic<float> gMeasuredFps{0.f};
void setMeasuredFps(float fps) { gMeasuredFps.store(fps); }
float measuredFps() { return gMeasuredFps.load(); }

void publishFx(const FxSettings& f) {
    std::lock_guard<std::mutex> l(gMutex);
    gFx = f;
    gFxVersion++;
}

bool takeFx(FxSettings& out, unsigned& lastVersion) {
    std::lock_guard<std::mutex> l(gMutex);
    if (gFxVersion == lastVersion) return false;
    out = gFx;
    lastVersion = gFxVersion;
    return true;
}

FxSettings currentFx() {
    std::lock_guard<std::mutex> l(gMutex);
    return gFx;
}

void setVisualStatus(const std::string& s) {
    std::lock_guard<std::mutex> l(gMutex);
    gStatus.visual = s;
}

void setSceneStatus(const std::string& s) {
    std::lock_guard<std::mutex> l(gMutex);
    gStatus.scene = s;
}

void setEspStatus(const std::string& s) {
    std::lock_guard<std::mutex> l(gMutex);
    gStatus.esp = s;
}

void setTasStatus(const std::string& s) {
    std::lock_guard<std::mutex> l(gMutex);
    gStatus.tas = s;
}

void setSplitsView(const SplitsView& v) {
    std::lock_guard<std::mutex> l(gMutex);
    gSplits = v;
}

static std::vector<std::pair<std::string, int>> gFilterPending;

void splitFilterSet(const std::string& name, bool on) {
    std::lock_guard<std::mutex> l(gMutex);
    gFilterPending.push_back(std::make_pair(name, on ? 1 : 0));
    // show the change at once: the main thread publishes the real state a moment later
    for (size_t i = 0; i < gSplits.items.size(); i++)
        if (name == "*" || gSplits.items[i] == name) gSplits.itemOn[i] = on ? 1 : 0;
}

bool splitFilterTake(std::vector<std::pair<std::string, int>>& out) {
    std::lock_guard<std::mutex> l(gMutex);
    if (gFilterPending.empty()) return false;
    out.swap(gFilterPending);
    gFilterPending.clear();
    return true;
}

SplitsView getSplitsView() {
    std::lock_guard<std::mutex> l(gMutex);
    return gSplits;
}

void setCheatStatus(const std::string& s) {
    std::lock_guard<std::mutex> l(gMutex);
    gStatus.cheat = s;
}

void setPerfStatus(const std::string& perf, const std::string& resolution, int screenW, int screenH) {
    std::lock_guard<std::mutex> l(gMutex);
    gStatus.perf = perf;
    gStatus.resolution = resolution;
    gStatus.screenW = screenW;
    gStatus.screenH = screenH;
}

MainStatus currentStatus() {
    std::lock_guard<std::mutex> l(gMutex);
    return gStatus;
}

void postToast(const char* text) {
    std::lock_guard<std::mutex> l(gMutex);
    gStatus.toast = text ? text : "";
    gStatus.toastSeq++;
}

void setTouch(const Touch& t) {
    std::lock_guard<std::mutex> l(gMutex);
    gTouch = t;
}

Touch getTouch() {
    std::lock_guard<std::mutex> l(gMutex);
    return gTouch;
}

// ---------------------------------------------------------------- settings file (key=value lines)
// The keys are the ones of the C# mod, so the two versions read each other's settings.

static std::string path() {
    const char* d = dataDir();
    if (!d || !*d) return "";
    return std::string(d) + "/visualmobile.cfg";
}

namespace {

enum Kind { kBool, kFloat, kInt, kHex };

struct Field {
    const char* key;
    Kind kind;
    void* p;
    float lo, hi;
};

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

bool parseHex(const char* v, float* rgb) {
    if (*v == '#') v++;
    if (strlen(v) != 6) return false;
    char* end = nullptr;
    long n = strtol(v, &end, 16);
    if (end == v || *end) return false;
    rgb[0] = ((n >> 16) & 255) / 255.f;
    rgb[1] = ((n >> 8) & 255) / 255.f;
    rgb[2] = (n & 255) / 255.f;
    return true;
}

void applyField(const Field& f, const char* v) {
    switch (f.kind) {
        case kBool:
            *static_cast<bool*>(f.p) = (v[0] == '1' || !strcasecmp(v, "true"));
            break;
        case kFloat: {
            char* end = nullptr;
            float x = strtof(v, &end);
            if (end != v) *static_cast<float*>(f.p) = clampf(x, f.lo, f.hi);
            break;
        }
        case kInt: {
            char* end = nullptr;
            long x = strtol(v, &end, 10);
            if (end != v) *static_cast<int*>(f.p) = static_cast<int>(clampf((float)x, f.lo, f.hi));
            break;
        }
        case kHex:
            parseHex(v, static_cast<float*>(f.p));
            break;
    }
}

void writeField(FILE* o, const Field& f) {
    switch (f.kind) {
        case kBool: fprintf(o, "%s=%d\n", f.key, *static_cast<bool*>(f.p) ? 1 : 0); break;
        case kFloat: fprintf(o, "%s=%.3f\n", f.key, *static_cast<float*>(f.p)); break;
        case kInt: fprintf(o, "%s=%d\n", f.key, *static_cast<int*>(f.p)); break;
        case kHex: {
            float* c = static_cast<float*>(f.p);
            auto b = [](float x) { return (int)(clampf(x, 0.f, 1.f) * 255.f + 0.5f); };
            fprintf(o, "%s=#%02X%02X%02X\n", f.key, b(c[0]), b(c[1]), b(c[2]));
            break;
        }
    }
}

void fields(UiSettings& ui, FxSettings& fx, std::vector<Field>& out) {
#define FB(key, v) out.push_back({key, kBool, &(v), 0.f, 0.f})
#define FF(key, v, lo, hi) out.push_back({key, kFloat, &(v), lo, hi})
#define FI(key, v, lo, hi) out.push_back({key, kInt, &(v), lo, hi})
    // ---- menu
    FF("scale", ui.scale, 0.6f, 1.8f);
    FF("panelAlpha", ui.panelAlpha, 0.f, 1.f);
    FF("iconSize", ui.iconSize, 0.6f, 2.f);
    FF("iconAlpha", ui.iconAlpha, 0.f, 1.f);
    FB("rainbow", ui.rainbow);
    FF("rainbowSpeed", ui.rainbowSpeed, 0.1f, 3.f);
    FB("rounded", ui.rounded);
    FB("lock", ui.lock);
    FB("hideInGame", ui.hideInGame);
    out.push_back({"accent", kHex, ui.accent, 0.f, 0.f});
    out.push_back({"bg", kHex, ui.bg, 0.f, 0.f});
    FB("open", ui.open);
    FI("tab", ui.tab, 0, 7);
    FI("colorSub", ui.colorSub, 0, 3);
    FI("cheatSub", ui.cheatSub, 0, 3);
    FI("tasSub", ui.tasSub, 0, 3);
    FI("cheatEnemy", ui.cheatEnemy, 0, kEnemyCount - 1);
    FB("showFps", ui.showFps);
    FB("showGraph", ui.showGraph);
    FF("fpsX", ui.fpsX, -1.f, 20000.f);
    FF("fpsY", ui.fpsY, -1.f, 20000.f);
    FF("splitX", ui.splitX, -1.f, 20000.f);
    FF("splitY", ui.splitY, -1.f, 20000.f);
    FF("restartX", ui.restartX, -1.f, 20000.f);
    FF("restartY", ui.restartY, -1.f, 20000.f);
    FF("eyeX", ui.eyeX, -1.f, 20000.f);
    FF("eyeY", ui.eyeY, -1.f, 20000.f);
    FF("panX", ui.panX, -1.f, 20000.f);
    FF("panY", ui.panY, -1.f, 20000.f);
    // ---- screen effects
    FB("visOn", fx.on);
    FB("visExpOn", fx.exposureOn);     FF("visExp", fx.exposure, -5.f, 5.f);
    FB("visSatOn", fx.satOn);          FF("visSat", fx.saturation, -100.f, 100.f);
    FB("visConOn", fx.contrastOn);     FF("visCon", fx.contrast, -100.f, 100.f);
    FB("visHueOn", fx.hueOn);          FF("visHue", fx.hue, -180.f, 180.f);
    FB("visWarmOn", fx.warmOn);        FF("visTemp", fx.temperature, -100.f, 100.f);
                                       FF("visTint", fx.tint, -100.f, 100.f);
    FB("visFilOn", fx.filterOn);       FF("visFR", fx.fr, 0.f, 1.f);
    FF("visFG", fx.fg, 0.f, 1.f);      FF("visFB", fx.fb, 0.f, 1.f);
    FF("visFilStr", fx.filterStrength, 0.f, 1.f);
    FB("visBlurOn", fx.blurOn);        FF("visBlur", fx.blurStrength, 0.f, 360.f);
    FI("visBlurN", fx.blurSamples, 4, 32);
    FB("visGamOn", fx.gammaOn);        FF("visGam", fx.gamma, -1.f, 1.f);
    FB("visBloomOn", fx.bloomOn);      FF("visBloom", fx.bloom, 0.f, 10.f);
    FF("visBloomTh", fx.bloomThreshold, 0.f, 2.f);
    FB("visVigOn", fx.vignetteOn);     FF("visVig", fx.vignette, 0.f, 1.f);
    FF("visVigSm", fx.vignetteSmooth, 0.05f, 1.f);
    FB("visGrainOn", fx.grainOn);      FF("visGrain", fx.grain, 0.f, 1.f);
    FF("visGrainSz", fx.grainSize, 0.3f, 3.f);
    FB("visChrOn", fx.chromaOn);       FF("visChr", fx.chroma, 0.f, 1.f);
    FB("visLensOn", fx.lensOn);        FF("visLens", fx.lens, -100.f, 100.f);
    FB("visAoOn", fx.aoOn);            FF("visAo", fx.aoIntensity, 0.f, 4.f);
    FF("visAoRad", fx.aoRadius, 0.05f, 3.f);
    FI("visAoQ", fx.aoQuality, 0, 2);
    FB("visDofOn", fx.dofOn);          FF("visDofFocus", fx.dofFocus, 0.5f, 50.f);
    FF("visDofAp", fx.dofAperture, 1.f, 32.f);
    FF("visDofLen", fx.dofLength, 10.f, 300.f);
    FI("visDofQ", fx.dofQuality, 0, 2);
    // ---- dark scenes
    FB("ambOn", fx.ambientOn);         FF("amb", fx.ambient, 0.f, 100.f);
    FB("lightOn", fx.lightBoostOn);    FF("lightMul", fx.lightBoost, 1.f, 3.f);
    FB("fovOn", fx.fovOn);             FF("fov", fx.fov, 40.f, 110.f);
    // ---- fog
    FI("fogMode", fx.fogMode, 0, 2);
    FF("fogR", fx.fogR, 0.f, 1.f);
    FF("fogG", fx.fogG, 0.f, 1.f);
    FF("fogB", fx.fogB, 0.f, 1.f);
    FF("fogThick", fx.fogThick, 0.f, 100.f);
    // ---- performance
    FI("perfLevel", fx.perfLevel, 0, 2);
    FI("aaMode", fx.aaMode, 0, 4);
    FI("anisoMode", fx.anisoMode, 0, 2);
    FB("lodOn", fx.lodOn);             FF("lodBias", fx.lodBias, 0.3f, 2.f);
    FB("perfNoTex", fx.noTextures);
    FB("perfParticles", fx.hideParticles);
    FB("perfPostFx", fx.noPostFx);
    FB("perfLights", fx.noShadow);
    FB("perfDist", fx.drawDistOn);     FF("perfDistV", fx.drawDist, 20.f, 300.f);
    FI("perfRes", fx.resIndex, 0, 4);
    FB("perfCustom", fx.customRes);
    FI("perfW", fx.customW, 0, 4096);
    FI("perfH", fx.customH, 0, 4096);
    FB("perfSquash", fx.squashOn);     FF("perfSquashV", fx.squash, 0.3f, 3.f);
    FI("perfFps", fx.fpsIndex, 0, 4);
    FB("perfAdaptive", fx.adaptive);
    FI("perfAdaptT", fx.adaptTarget, 0, 2);
    FB("perfAuto", fx.autoClean);
    FB("espItems", fx.espItems);
    FB("espEnemies", fx.espEnemies);
    FB("espWalls", fx.espWalls);
    FI("espItemColor", fx.espItemColor, 0, 7);
    FI("espEnemyColor", fx.espEnemyColor, 0, 7);
    FF("espItemWidth", fx.espItemWidth, 1.f, 8.f);
    FF("espEnemyWidth", fx.espEnemyWidth, 1.f, 8.f);

    // ---- cheat tab (the keys of the per-enemy switches are built once, the Field keeps a pointer to them)
    {
        CheatSettings& c = fx.cheat;
        FB("chGod", c.godMode);
        FB("chSpeedOn", c.speedOn);        FF("chSpeed", c.speed, 1.f, 6.f);
        FB("chNoclip", c.noclip);
        FB("chNoFall", c.noFallDamage);
        FB("chNoCrack", c.noCrackFloor);
        FB("chAmmo", c.infAmmo);
        FB("chSpray", c.infSpray);
        FB("chNoTrap", c.noTrap);
        static char enemyKeys[kEnemyCount][3][24];
        for (int i = 0; i < kEnemyCount; i++) {
            snprintf(enemyKeys[i][0], sizeof(enemyKeys[i][0]), "chFreeze%d", i);
            snprintf(enemyKeys[i][1], sizeof(enemyKeys[i][1]), "chBlind%d", i);
            snprintf(enemyKeys[i][2], sizeof(enemyKeys[i][2]), "chDeaf%d", i);
            FB(enemyKeys[i][0], c.enemy[i].freeze);
            FB(enemyKeys[i][1], c.enemy[i].blind);
            FB(enemyKeys[i][2], c.enemy[i].deaf);
        }
        FI("chSpawnItem", c.spawnItem, 0, kItemCount - 1);
        FB("chAutoGrab", c.autoGrab);
        FB("chReachOn", c.longReachOn);    FF("chReach", c.reach, 2.f, 40.f);
    }

    // ---- TAS tab
    {
        TasSettings& t = fx.tas;
        FB("tasSpeedOn", t.speedOn);       FF("tasSpeed", t.speed, 0.01f, 5.f);
        FB("tasAudio", t.audioSync);
        FB("tasSplits", t.splitsShow);
        FB("tasAuto", t.autoList);
        FB("tasEscape", t.escapeSplit);
        FF("tasSplitScale", t.splitsScale, 0.6f, 2.f);
        FF("tasSplitAlpha", t.splitsAlpha, 0.f, 1.f);
        FB("tasSkipBed", t.skipBed);
        FB("tasSkipEnd", t.skipEnding);
        FB("tasRestartBtn", t.restartBtn);
        FF("tasRestartSize", t.restartBtnSize, 0.6f, 2.f);
        FB("tasCross", t.crosshairOn);
        FI("tasCrossPreset", t.crosshairPreset, 0, 5);
        FF("tasCrossScale", t.crosshairScale, 0.5f, 3.f);
        FF("tasCrossAlpha", t.crosshairAlpha, 0.f, 1.f);
    }
#undef FB
#undef FF
#undef FI
}

}  // namespace

void settingsLoad(UiSettings& ui, FxSettings& fx) {
    std::string p = path();
    FILE* f = p.empty() ? nullptr : fopen(p.c_str(), "r");
    if (f) {
        std::vector<Field> fl;
        fields(ui, fx, fl);
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (line[0] == '#') continue;
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            const char* k = line;
            char* v = eq + 1;
            v[strcspn(v, "\r\n")] = 0;
            for (const Field& fd : fl) {
                if (!strcmp(k, fd.key)) {
                    applyField(fd, v);
                    break;
                }
            }
        }
        fclose(f);
        logf("settings loaded from %s", p.c_str());
    }
    publishFx(fx);
}

void settingsSave(const UiSettings& uiIn, const FxSettings& fxIn) {
    std::string p = path();
    if (p.empty()) return;
    UiSettings ui = uiIn;
    FxSettings fx = fxIn;
    FILE* f = fopen(p.c_str(), "w");
    if (!f) return;
    fprintf(f, "# VisualMobile settings - written by the menu. Delete this file to reset.\n");
    std::vector<Field> fl;
    fields(ui, fx, fl);
    for (const Field& fd : fl) writeField(f, fd);
    fclose(f);
}

}  // namespace vm
