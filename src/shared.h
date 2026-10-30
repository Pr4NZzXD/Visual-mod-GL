#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// State shared between the three threads: the game's main thread (applies effects, reads touch),
// the render thread (draws the menu) and the loader thread.

namespace vm {

constexpr const char* kVersion = "1.4.0";

// ---- the Cheat tab. Names that the menu and the cheat code both need.
constexpr int kEnemyCount = 7;
inline constexpr const char* kEnemyNames[kEnemyCount] = {"Granny", "Grandpa", "Mom Spider", "Slendrina",
                                                         "Attic Spider", "Crow", "Little One"};

// Same order as the PC version (ItemSpawn.CountItem is 1-based there, so index + 1). The Android names and the way to
// spawn each one still have to come from the dump.
inline constexpr const char* kItemNames[] = {
    "Crossbow", "Pliers", "Battery", "Gas", "Bird Seed", "Book", "Winch",
    "Car Battery", "Car Key", "Chain Cutter", "Code", "Baton", "EC Key",
    "Hammer", "Padlock Key", "Master Key", "Cog 1", "Cog 2", "Meat",
    "Melon", "Spray", "Plank", "Playhouse Key", "Remote", "Robo Data",
    "Rusty Key", "Safe Key", "Screwdriver", "Shotgun", "Shotgun 2",
    "SP 1", "SP 2", "SP 3", "Spark Plug", "Special", "Spider", "Syringe",
    "T1", "T2", "T3", "T4", "Teddy", "Text", "Engine Part", "WP Key", "Vase",
    "Vase 2", "Vase 3", "Wheel Crank", "Wooden Stick", "Wrench", "Rat",
    "Freeze Ornament", "Bomb Ornament", "Fuse",
};
constexpr int kItemCount = (int)(sizeof(kItemNames) / sizeof(kItemNames[0]));

// What each enemy can do: {freeze, blind, deaf}. The game has no blind or deaf behaviour for every enemy (it is in the
// code of Granny, Grandpa and the Little One only), so the menu shows only what an enemy really supports.
inline constexpr bool kEnemyCan[kEnemyCount][3] = {
    {true, true, true},    // Granny
    {true, true, true},    // Grandpa
    {true, false, false},  // Mom Spider
    {true, false, false},  // Slendrina
    {true, false, false},  // Attic Spider
    {true, false, false},  // Crow
    {true, true, false},   // Little One
};
// the Kill button: some enemies cannot die, they are stunned or knocked out (that is what the game's own weapons do)
inline constexpr const char* kEnemyKill[kEnemyCount] = {"Kill", "Kill", "Stun", "Kill", "Kill", "Kill", "Knock out"};

// One thing the ESP draws. The main thread works it out (it is the only one that may ask the game), the render thread
// draws it. x, y, w, h are fractions of the screen (0..1, origin top-left); the box is centred on x, y.
struct EspMark {
    float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
    float dist = 0.f;     // metres from the camera
    bool enemy = false;   // false = item
    char name[28] = {0};
};
void publishEsp(const std::vector<EspMark>& marks);
std::vector<EspMark> currentEsp();  // empty when the last frame is older than 400 ms

// What the Cheat tab switches. The menu writes it, the main thread reads it (cheat::tick).
// Counters (killReq, spawnReq) work like cleanReq: the menu raises them, the main thread does the job once per change.
struct CheatSettings {
    // ---- player
    bool godMode = false;         // enemies cannot attack the player
    bool speedOn = false;         float speed = 2.f;       // walking speed multiplier, 1 .. 6
    bool noclip = false;          // walk through walls; looking up / down while walking changes the height
    bool noFallDamage = false;
    bool noCrackFloor = false;    // the floor does not crack (no sound, no break)
    bool infAmmo = false;
    bool infSpray = false;
    bool noTrap = false;

    // ---- enemies: every enemy has its own switches
    struct Enemy {
        bool freeze = false;      // cannot move
        bool blind = false;       // cannot see the player
        bool deaf = false;        // hears nothing (cracking floor, dropped items)
    };
    Enemy enemy[kEnemyCount];
    int killReq[kEnemyCount] = {};

    // ---- items
    int spawnItem = 0;            // index into kItemNames
    int spawnReq = 0;
    bool autoGrab = false;
    bool longReachOn = false;     float reach = 10.f;      // metres, 2 .. 40

    // ---- ESP
    bool espEnemy = false;
    bool espEnemyName = true, espEnemyDist = true, espEnemyOutline = true;
    float espEnemyMax = 80.f;     // metres, 10 .. 300
    bool espItem = false;
    bool espItemName = true, espItemDist = true, espItemOutline = true;
    float espItemMax = 40.f;      // metres, 10 .. 300
};

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
    bool aoOn = false;        float aoIntensity = 1.f;       // ambient occlusion, 0 .. 4
                              float aoRadius = 0.5f;         // 0.05 .. 3
                              int aoQuality = 1;             // 0 low, 1 medium, 2 high
    bool dofOn = false;       float dofFocus = 6.f;          // depth of field: focus distance in metres, 0.5 .. 50
                              float dofAperture = 5.6f;      // 1 .. 32 (lower = more blur)
                              float dofLength = 50.f;        // focal length in mm, 10 .. 300
                              int dofQuality = 1;            // 0 small, 1 medium, 2 large blur

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
    int aaMode = 0;                                          // anti-aliasing: 0 game, 1 off, 2 = 2x, 3 = 4x, 4 = 8x
    int anisoMode = 0;                                       // anisotropic filtering: 0 game, 1 off, 2 on
    bool lodOn = false;       float lodBias = 1.f;           // LOD bias, 0.3 .. 2
    int resIndex = 0;                                        // index into the percentage list
    bool customRes = false;   int customW = 0, customH = 0;  // 0 = not set yet
    bool squashOn = false;    float squash = 0.8f;           // 0.3 .. 3
    int fpsIndex = 0;                                        // index into the FPS list
    bool adaptive = false;                                   // the render size follows the FPS
    int adaptTarget = 0;                                     // 0 = 30, 1 = 45, 2 = 60 FPS
    bool autoClean = false;
    int cleanReq = 0;                                        // raised by the "Clean memory now" button

    // ---- the Cheat tab
    CheatSettings cheat;
};

// What the main thread tells the menu (texts for the pages, the screen size, short messages).
struct MainStatus {
    std::string visual = "Off";
    std::string perf;
    std::string resolution;
    std::string scene;              // what the dark-scene options found (lights, cameras)
    std::string cheat = "Cheat: waiting for the game to start.";
    int screenW = 0, screenH = 0;   // the size the game renders at now
    int toastSeq = 0;
    std::string toast;
};

struct UiSettings {
    bool open = false;
    int tab = 0;          // 0 General, 1 Color, 2 Effects, 3 Advanced, 4 Performance, 5 Settings, 6 Cheat
    int colorSub = 0;     // 0 Basic, 1 Temperature, 2 Tint, 3 RGB
    int cheatSub = 0;     // 0 Player, 1 Enemies, 2 Items, 3 ESP
    int cheatEnemy = 0;   // the enemy whose switches the Enemies page shows
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
void setSceneStatus(const std::string& s);
void setCheatStatus(const std::string& s);
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
