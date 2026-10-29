#pragma once
#include <atomic>

// State shared between the three threads: the game's main thread (applies effects, reads touch),
// the render thread (draws the menu) and the loader thread.

namespace vm {

struct FxSettings {
    bool on = false;
    bool exposureOn = false;  float exposure = 0.f;      // -5 .. 5 EV
    bool satOn = false;       float saturation = 0.f;    // -100 .. 100
    bool contrastOn = false;  float contrast = 0.f;      // -100 .. 100
    bool hueOn = false;       float hue = 0.f;           // -180 .. 180
    bool warmOn = false;      float temperature = 0.f;   // -100 .. 100
                              float tint = 0.f;          // -100 .. 100
};

struct UiSettings {
    bool open = false;
    int tab = 0;
    bool showFps = false;
    bool lock = false;
    float scale = 1.f;
    float panelAlpha = 0.94f;
    float iconSize = 1.f;
    float eyeX = -1.f, eyeY = -1.f;
    float panX = -1.f, panY = -1.f;
    float fpsX = -1.f, fpsY = -1.f;
};

struct Touch {
    bool valid = false;  // a finger is on the screen (or was lifted this frame)
    bool down = false;
    float x = 0.f, y = 0.f;  // Unity screen pixels, origin bottom-left
};

// effect settings: written by the menu, read by the main thread
void publishFx(const FxSettings& f);
bool takeFx(FxSettings& out, unsigned& lastVersion);  // true when it changed since lastVersion
FxSettings currentFx();

// touch: written by the main thread, read by the render thread
void setTouch(const Touch& t);
Touch getTouch();

// true while a finger is on the menu: the game ignores that touch
extern std::atomic<bool> gBlockTouch;

// settings file
void settingsLoad(UiSettings& ui);
void settingsSave(const UiSettings& ui);

}  // namespace vm
