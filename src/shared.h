#pragma once
#include <atomic>
#include <string>

// State shared between the three threads: the game's main thread (applies effects, reads touch),
// the render thread (draws the menu) and the loader thread.

namespace vm {

constexpr const char* kVersion = "1.1.0";

// Everything the main thread needs from the menu: screen effects, fog and performance options.
// The names and ranges follow the C# mod (VisualMobile for MelonLoader).
struct FxSettings {
    // ---- screen effects
    bool on = false;
    bool exposureOn = false;  float exposure = 0.f;          // EV, -5 .. 5
    bool satOn = false;       float saturation = 0.f;        // -100 .. 100
    bool contrastOn = false;  float contrast = 0.f;          // -100 .. 100
    bool hueOn = false;       float hue = 0.f;               // -180 .. 180
    bool warmOn = false;      float temperature = 0.f;       // -100 .. 100
                              float tint = 0.f;              // -100 .. 100
    bool filterOn = false;    float fr = 1.f, fg = 1.f, fb = 1.f, filterStrength = 1.f;
    bool blurOn = false;      float blurStrength = 180.f;    // shutter angle, 0 .. 360
                              int blurSamples = 8;           // 4 .. 32
    bool gammaOn = false;     float gamma = 0.f;             // -1 .. 1
    bool bloomOn = false;     float bloom = 1.f;             // 0 .. 10
                              float bloomThreshold = 0.8f;   // 0 .. 2
    bool vignetteOn = false;  float vignette = 0.4f;         // 0 .. 1
                              float vignetteSmooth = 0.4f;   // 0.05 .. 1
    bool grainOn = false;     float grain = 0.3f;            // 0 .. 1
                              float grainSize = 1.f;         // 0.3 .. 3
    bool chromaOn = false;    float chroma = 0.3f;           // 0 .. 1
    bool lensOn = false;      float lens = 20.f;             // -100 .. 100

    // ---- dark scenes
    bool ambientOn = false;   float ambient = 30.f;          // 0 .. 100 % flat ambient light
    bool lightBoostOn = false; float lightBoost = 1.5f;      // 1 .. 3 times the intensity of every light
    bool fovOn = false;       float fov = 70.f;              // 40 .. 110 degrees

    // ---- fog: 0 = the game's own, 1 = none, 2 = custom
    int fogMode = 0;
    float fogR = 0.60f, fogG = 0.60f, fogB = 0.65f;
    float fogThick = 30.f;                                   // 0 .. 100 %

    // ---- performance
    int perfLevel = 0;                                       // 0 off, 1 low, 2 ultra low
    bool noTextures = false, hideParticles = false, noPostFx = false, noShadow = false;
    bool drawDistOn = false;  float drawDist = 80.f;         // 20 .. 300
    int resIndex = 0;                                        // index into the percentage list
    bool customRes = false;   int customW = 0, customH = 0;  // 0 = not set yet
    bool squashOn = false;    float squash = 0.8f;           // 0.3 .. 3
    int fpsIndex = 0;                                        // index into the FPS list
    bool adaptive = false;                                   // the render size follows the FPS
    int adaptTarget = 0;                                     // 0 = 30, 1 = 45, 2 = 60 FPS
    bool autoClean = false;
    int cleanReq = 0;                                        // raised by the "Clean memory now" button
};

// What the main thread tells the menu (texts for the pages, the screen size, short messages).
struct MainStatus {
    std::string visual = "Off";
    std::string perf;
    std::string resolution;
    int screenW = 0, screenH = 0;   // the size the game renders at now
    int toastSeq = 0;
    std::string toast;
};

struct UiSettings {
    bool open = false;
    int tab = 0;          // 0 General, 1 Color, 2 Effects, 3 Advanced, 4 Performance, 5 Settings
    int colorSub = 0;     // 0 Basic, 1 Temperature, 2 Tint, 3 RGB
    bool showFps = false;
    bool showGraph = false;
    bool lock = false;
    bool rainbow = false;
    bool rounded = true;
    float scale = 1.f;
    float panelAlpha = 0.94f;
    float iconSize = 1.f;
    float iconAlpha = 1.f;
    float rainbowSpeed = 0.5f;
    float accent[3] = {0.12f, 0.58f, 1.00f};
    float bg[3] = {0.055f, 0.060f, 0.072f};
    float eyeX = -1.f, eyeY = -1.f;
    float panX = -1.f, panY = -1.f;
    float fpsX = -1.f, fpsY = -1.f;
};

struct Touch {
    bool valid = false;  // a finger is on the screen (or was lifted this frame)
    bool down = false;
    float x = 0.f, y = 0.f;  // Unity screen pixels, origin bottom-left
};

// settings: written by the menu, read by the main thread
void publishFx(const FxSettings& f);
bool takeFx(FxSettings& out, unsigned& lastVersion);  // true when it changed since lastVersion
FxSettings currentFx();

// status: written by the main thread, read by the menu
void setVisualStatus(const std::string& s);
void setPerfStatus(const std::string& perf, const std::string& resolution, int screenW, int screenH);
MainStatus currentStatus();
void postToast(const char* text);  // a short message in the footer of the panel

// touch: written by the main thread, read by the render thread
void setTouch(const Touch& t);
Touch getTouch();

// frames per second as the render thread sees them (the adaptive resolution reads it)
void setMeasuredFps(float fps);
float measuredFps();

// true while a finger that started on the menu is down: the game ignores that touch
extern std::atomic<bool> gBlockTouch;

// settings file
void settingsLoad(UiSettings& ui, FxSettings& fx);
void settingsSave(const UiSettings& ui, const FxSettings& fx);

}  // namespace vm
