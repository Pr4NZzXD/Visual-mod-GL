#include "shared.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "log.h"

namespace vm {

static std::mutex gMutex;
static FxSettings gFx;
static unsigned gFxVersion = 1;
static Touch gTouch;

std::atomic<bool> gBlockTouch{false};

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

void setTouch(const Touch& t) {
    std::lock_guard<std::mutex> l(gMutex);
    gTouch = t;
}

Touch getTouch() {
    std::lock_guard<std::mutex> l(gMutex);
    return gTouch;
}

// ---------------------------------------------------------------- settings file (key=value lines)

static std::string path() {
    const char* d = dataDir();
    if (!d || !*d) return "";
    return std::string(d) + "/visualmobile.cfg";
}

static bool pb(const char* v) { return v[0] == '1'; }
static float pf(const char* v, float lo, float hi, float fb) {
    char* end = nullptr;
    float f = strtof(v, &end);
    if (end == v) return fb;
    return f < lo ? lo : (f > hi ? hi : f);
}

void settingsLoad(UiSettings& ui) {
    FxSettings fx;
    std::string p = path();
    FILE* f = p.empty() ? nullptr : fopen(p.c_str(), "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            const char* k = line;
            char* v = eq + 1;
            v[strcspn(v, "\r\n")] = 0;
            if (!strcmp(k, "on")) fx.on = pb(v);
            else if (!strcmp(k, "exposureOn")) fx.exposureOn = pb(v);
            else if (!strcmp(k, "exposure")) fx.exposure = pf(v, -5.f, 5.f, 0.f);
            else if (!strcmp(k, "satOn")) fx.satOn = pb(v);
            else if (!strcmp(k, "saturation")) fx.saturation = pf(v, -100.f, 100.f, 0.f);
            else if (!strcmp(k, "contrastOn")) fx.contrastOn = pb(v);
            else if (!strcmp(k, "contrast")) fx.contrast = pf(v, -100.f, 100.f, 0.f);
            else if (!strcmp(k, "hueOn")) fx.hueOn = pb(v);
            else if (!strcmp(k, "hue")) fx.hue = pf(v, -180.f, 180.f, 0.f);
            else if (!strcmp(k, "warmOn")) fx.warmOn = pb(v);
            else if (!strcmp(k, "temperature")) fx.temperature = pf(v, -100.f, 100.f, 0.f);
            else if (!strcmp(k, "tint")) fx.tint = pf(v, -100.f, 100.f, 0.f);
            else if (!strcmp(k, "open")) ui.open = pb(v);
            else if (!strcmp(k, "tab")) ui.tab = (int)pf(v, 0.f, 2.f, 0.f);
            else if (!strcmp(k, "showFps")) ui.showFps = pb(v);
            else if (!strcmp(k, "lock")) ui.lock = pb(v);
            else if (!strcmp(k, "scale")) ui.scale = pf(v, 0.6f, 1.8f, 1.f);
            else if (!strcmp(k, "panelAlpha")) ui.panelAlpha = pf(v, 0.3f, 1.f, 0.94f);
            else if (!strcmp(k, "iconSize")) ui.iconSize = pf(v, 0.6f, 2.f, 1.f);
            else if (!strcmp(k, "eyeX")) ui.eyeX = pf(v, -1.f, 20000.f, -1.f);
            else if (!strcmp(k, "eyeY")) ui.eyeY = pf(v, -1.f, 20000.f, -1.f);
            else if (!strcmp(k, "panX")) ui.panX = pf(v, -1.f, 20000.f, -1.f);
            else if (!strcmp(k, "panY")) ui.panY = pf(v, -1.f, 20000.f, -1.f);
            else if (!strcmp(k, "fpsX")) ui.fpsX = pf(v, -1.f, 20000.f, -1.f);
            else if (!strcmp(k, "fpsY")) ui.fpsY = pf(v, -1.f, 20000.f, -1.f);
        }
        fclose(f);
        logf("settings loaded from %s", p.c_str());
    }
    publishFx(fx);
}

void settingsSave(const UiSettings& ui) {
    std::string p = path();
    if (p.empty()) return;
    FxSettings fx = currentFx();
    FILE* f = fopen(p.c_str(), "w");
    if (!f) return;
    fprintf(f, "on=%d\nexposureOn=%d\nexposure=%.3f\nsatOn=%d\nsaturation=%.3f\n", fx.on, fx.exposureOn, fx.exposure, fx.satOn, fx.saturation);
    fprintf(f, "contrastOn=%d\ncontrast=%.3f\nhueOn=%d\nhue=%.3f\nwarmOn=%d\ntemperature=%.3f\ntint=%.3f\n", fx.contrastOn, fx.contrast, fx.hueOn, fx.hue, fx.warmOn, fx.temperature, fx.tint);
    fprintf(f, "open=%d\ntab=%d\nshowFps=%d\nlock=%d\nscale=%.3f\npanelAlpha=%.3f\niconSize=%.3f\n", ui.open, ui.tab, ui.showFps, ui.lock, ui.scale, ui.panelAlpha, ui.iconSize);
    fprintf(f, "eyeX=%.1f\neyeY=%.1f\npanX=%.1f\npanY=%.1f\nfpsX=%.1f\nfpsY=%.1f\n", ui.eyeX, ui.eyeY, ui.panX, ui.panY, ui.fpsX, ui.fpsY);
    fclose(f);
}

}  // namespace vm
