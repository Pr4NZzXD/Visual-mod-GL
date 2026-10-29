#include "menu.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "log.h"
#include "shared.h"

namespace vm {
namespace {

bool gInit = false;
bool gFailed = false;
UiSettings ui;
FxSettings fx;
float gScaleApplied = -1.f;
std::chrono::steady_clock::time_point gLast;
bool gLastDown = false;
bool gNeedSave = false;
float gSaveTimer = 0.f;

const ImVec4 kAccent = ImVec4(0.12f, 0.58f, 1.00f, 1.f);
const ImVec4 kBg = ImVec4(0.055f, 0.060f, 0.072f, 1.f);

void applyStyle(float s) {
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    ImGui::StyleColorsDark();
    st.ScaleAllSizes(s);
    st.WindowRounding = 12.f * s;
    st.ChildRounding = 9.f * s;
    st.FrameRounding = 8.f * s;
    st.GrabRounding = 8.f * s;
    st.PopupRounding = 8.f * s;
    st.ScrollbarRounding = 8.f * s;
    st.WindowBorderSize = 1.5f * s;
    st.ChildBorderSize = 0.f;
    st.FrameBorderSize = 0.f;
    st.WindowPadding = ImVec2(10.f * s, 8.f * s);
    st.FramePadding = ImVec2(10.f * s, 8.f * s);
    st.ItemSpacing = ImVec2(8.f * s, 8.f * s);
    st.GrabMinSize = 22.f * s;
    st.ScrollbarSize = 8.f * s;
    st.WindowTitleAlign = ImVec2(0.5f, 0.5f);

    ImVec4* c = st.Colors;
    ImVec4 card = ImVec4(0.095f, 0.105f, 0.130f, 1.f);
    c[ImGuiCol_Border] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.75f);
    c[ImGuiCol_TitleBg] = kBg;
    c[ImGuiCol_TitleBgActive] = kBg;
    c[ImGuiCol_TitleBgCollapsed] = kBg;
    c[ImGuiCol_ChildBg] = ImVec4(0.080f, 0.090f, 0.110f, 1.f);
    c[ImGuiCol_FrameBg] = card;
    c[ImGuiCol_FrameBgHovered] = card;
    c[ImGuiCol_FrameBgActive] = card;
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_SliderGrab] = ImVec4(1.f, 1.f, 1.f, 1.f);
    c[ImGuiCol_SliderGrabActive] = kAccent;
    c[ImGuiCol_Button] = card;
    c[ImGuiCol_ButtonHovered] = card;
    c[ImGuiCol_ButtonActive] = kAccent;
    c[ImGuiCol_ScrollbarGrab] = kAccent;
}

void initImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImFontConfig fc;
    fc.SizePixels = 26.f;
    io.Fonts->AddFontDefault(&fc);

    if (!ImGui_ImplOpenGL3_Init("#version 300 es")) {
        logf("menu: ImGui OpenGL backend failed to start");
        gFailed = true;
        return;
    }
    settingsLoad(ui);
    fx = currentFx();
    gLast = std::chrono::steady_clock::now();
    gInit = true;
    logf("menu: ImGui ready");
}

void touchSave() {
    gNeedSave = true;
    gSaveTimer = 0.f;
}

// a switch, a name and a slider; moving the slider also switches the effect on
bool fxRow(const char* label, bool* on, float* v, float mn, float mx, const char* fmt) {
    bool ch = false;
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    ch |= ImGui::Checkbox("##on", on);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderFloat("##v", v, mn, mx, fmt)) {
        *on = true;
        ch = true;
    }
    ImGui::PopID();
    return ch;
}

void drawEye(ImDrawList* dl, ImVec2 p, float size, ImU32 bg, ImU32 acc, float s) {
    float r = size * 0.5f;
    ImVec2 c(p.x + r, p.y + r);
    dl->AddCircleFilled(c, r, bg, 40);
    dl->AddCircle(c, r - 1.f * s, acc, 40, 2.f * s);

    // the lens of the eye: two arcs that meet at the corners
    const int N = 20;
    ImVec2 pts[2 * N];
    float w = r * 0.66f;
    for (int i = 0; i < N; i++) {
        float u = -1.f + 2.f * i / (N - 1);
        float hl = 0.55f * (1.f - u * u) * w;
        pts[i] = ImVec2(c.x + u * w, c.y - hl);
        pts[2 * N - 1 - i] = ImVec2(c.x + u * w, c.y + hl);
    }
    dl->AddPolyline(pts, 2 * N, acc, ImDrawFlags_Closed, 2.f * s);
    dl->AddCircle(c, 0.27f * w, acc, 24, 0.14f * w + 1.f);
    dl->AddCircleFilled(c, 0.11f * w, acc, 16);
}

void drawFps(int w, int h, float s) {
    ImGuiIO& io = ImGui::GetIO();
    float fw = 92.f * s, fh = 26.f * s;
    if (ui.fpsX < 0.f || ui.fpsY < 0.f) {
        ui.fpsX = 8.f * s;
        ui.fpsY = 8.f * s;
    }
    ui.fpsX = std::min(std::max(ui.fpsX, 0.f), std::max(0.f, w - fw));
    ui.fpsY = std::min(std::max(ui.fpsY, 0.f), std::max(0.f, h - fh));

    ImGui::SetNextWindowPos(ImVec2(ui.fpsX, ui.fpsY));
    ImGui::SetNextWindowSize(ImVec2(fw, fh));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::InvisibleButton("##fpsbtn", ImVec2(fw, fh));
    static bool moved = false;
    static ImVec2 down;
    if (ImGui::IsItemActivated()) {
        moved = false;
        down = io.MousePos;
    }
    if (ImGui::IsItemActive()) {
        float dx = io.MousePos.x - down.x, dy = io.MousePos.y - down.y;
        if (!moved && std::sqrt(dx * dx + dy * dy) > 10.f * s) moved = true;
        if (moved && !ui.lock) {
            ui.fpsX += io.MouseDelta.x;
            ui.fpsY += io.MouseDelta.y;
            touchSave();
        }
    }
    int fps = (int)std::lround(io.Framerate);
    ImU32 col = fps >= 50 ? IM_COL32(89, 214, 122, 255) : (fps >= 30 ? IM_COL32(242, 191, 51, 255) : IM_COL32(255, 92, 92, 255));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetWindowPos();
    ImVec2 p1(p0.x + fw, p0.y + fh);
    dl->AddRectFilled(p0, p1, IM_COL32(14, 15, 18, 210), fh * 0.5f);
    dl->AddRect(p0, p1, IM_COL32(31, 148, 255, 180), fh * 0.5f, 0, 1.f * s);
    char buf[32];
    snprintf(buf, sizeof(buf), "%d FPS", fps);
    ImVec2 ts = ImGui::CalcTextSize(buf);
    dl->AddText(ImVec2(p0.x + (fw - ts.x) * 0.5f, p0.y + (fh - ts.y) * 0.5f), col, buf);
    ImGui::End();
    ImGui::PopStyleVar(2);
}

void drawEyeButton(int w, int h, float s) {
    ImGuiIO& io = ImGui::GetIO();
    float size = 44.f * s * ui.iconSize;
    if (ui.eyeX < 0.f || ui.eyeY < 0.f) {
        ui.eyeX = w - size - 10.f * s;
        ui.eyeY = h * 0.30f;
    }
    ui.eyeX = std::min(std::max(ui.eyeX, 0.f), std::max(0.f, w - size));
    ui.eyeY = std::min(std::max(ui.eyeY, 0.f), std::max(0.f, h - size));

    ImGui::SetNextWindowPos(ImVec2(ui.eyeX, ui.eyeY));
    ImGui::SetNextWindowSize(ImVec2(size, size));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::Begin("##eye", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::InvisibleButton("##eyebtn", ImVec2(size, size));
    static bool moved = false;
    static ImVec2 down;
    if (ImGui::IsItemActivated()) {
        moved = false;
        down = io.MousePos;
    }
    if (ImGui::IsItemActive()) {
        float dx = io.MousePos.x - down.x, dy = io.MousePos.y - down.y;
        if (!moved && std::sqrt(dx * dx + dy * dy) > 10.f * s) moved = true;
        if (moved && !ui.lock) {
            ui.eyeX += io.MouseDelta.x;
            ui.eyeY += io.MouseDelta.y;
            touchSave();
        }
    }
    if (ImGui::IsItemDeactivated() && !moved) {
        ui.open = !ui.open;
        touchSave();
    }
    drawEye(ImGui::GetWindowDrawList(), ImGui::GetWindowPos(), size, IM_COL32(14, 15, 18, 225), IM_COL32(31, 148, 255, 255), s);
    ImGui::End();
    ImGui::PopStyleVar(2);
}

void tabGeneral(bool& fxDirty) {
    if (ImGui::Checkbox("Effects on", &fx.on)) fxDirty = true;
    if (ImGui::Checkbox("Show FPS", &ui.showFps)) touchSave();
    if (ImGui::Button("Reset all effects", ImVec2(-FLT_MIN, 0.f))) {
        bool on = fx.on;
        fx = FxSettings();
        fx.on = on;
        fxDirty = true;
    }
    ImGui::TextDisabled("Turn on \"Effects on\", then pick an effect in the Color tab.");
}

void tabColor(bool& fxDirty) {
    bool d = false;
    d |= fxRow("Exposure", &fx.exposureOn, &fx.exposure, -5.f, 5.f, "%.1f");
    d |= fxRow("Saturation", &fx.satOn, &fx.saturation, -100.f, 100.f, "%.0f");
    d |= fxRow("Contrast", &fx.contrastOn, &fx.contrast, -100.f, 100.f, "%.0f");
    d |= fxRow("Hue", &fx.hueOn, &fx.hue, -180.f, 180.f, "%.0f");
    d |= fxRow("Warmth (cool to warm)", &fx.warmOn, &fx.temperature, -100.f, 100.f, "%.0f");
    d |= fxRow("Tint (green to magenta)", &fx.warmOn, &fx.tint, -100.f, 100.f, "%.0f");
    if (d) {
        fx.on = true;
        fxDirty = true;
    }
}

void tabSettings() {
    static float draft = 1.f;
    static bool sliderActive = false;
    if (!sliderActive) draft = ui.scale;
    ImGui::TextUnformatted("Menu size");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderFloat("##scale", &draft, 0.6f, 1.8f, "%.2f");
    sliderActive = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        ui.scale = draft;
        touchSave();
    }
    ImGui::TextUnformatted("Menu opacity");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderFloat("##alpha", &ui.panelAlpha, 0.3f, 1.f, "%.2f")) touchSave();
    ImGui::TextUnformatted("Icon size");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderFloat("##icon", &ui.iconSize, 0.6f, 2.f, "%.2f")) touchSave();
    if (ImGui::Checkbox("Lock icon and menu position", &ui.lock)) touchSave();
    if (ImGui::Button("Reset menu look", ImVec2(-FLT_MIN, 0.f))) {
        ui.scale = 1.f;
        ui.panelAlpha = 0.94f;
        ui.iconSize = 1.f;
        ui.lock = false;
        ui.eyeX = ui.eyeY = ui.panX = ui.panY = ui.fpsX = ui.fpsY = -1.f;
        touchSave();
    }
}

void drawPanel(int w, int h, float s, bool& fxDirty) {
    ImGuiIO& io = ImGui::GetIO();
    float pw = std::min(w - 12.f * s, 520.f * s);
    float ph = std::min(h - 12.f * s, 330.f * s);
    if (ui.panX < 0.f || ui.panY < 0.f) {
        ui.panX = std::max(6.f * s, ui.eyeX - pw - 8.f * s);
        ui.panY = (h - ph) * 0.5f;
    }
    ui.panX = std::min(std::max(ui.panX, 0.f), std::max(0.f, w - pw));
    ui.panY = std::min(std::max(ui.panY, 0.f), std::max(0.f, h - ph));

    ImGui::SetNextWindowPos(ImVec2(ui.panX, ui.panY), ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(pw, ph), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(kBg.x, kBg.y, kBg.z, ui.panelAlpha));

    bool open = true;
    ImGuiWindowFlags fl = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize |
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (ui.lock) fl |= ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("VisualMobile", &open, fl)) {
        ImVec2 p = ImGui::GetWindowPos();
        if (std::fabs(p.x - ui.panX) > 0.5f || std::fabs(p.y - ui.panY) > 0.5f) {
            ui.panX = p.x;
            ui.panY = p.y;
            touchSave();
        }

        float sideW = 104.f * s;
        ImGui::BeginChild("##side", ImVec2(sideW, 0.f), 0, ImGuiWindowFlags_NoScrollbar);
        static const char* names[3] = {"General", "Color", "Settings"};
        for (int i = 0; i < 3; i++) {
            bool sel = ui.tab == i;
            ImGui::PushStyleColor(ImGuiCol_Button, sel ? kAccent : ImVec4(0.f, 0.f, 0.f, 0.f));
            if (ImGui::Button(names[i], ImVec2(-FLT_MIN, 38.f * s))) {
                ui.tab = i;
                touchSave();
            }
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("##content", ImVec2(0.f, 0.f), 0, 0);
        if (ui.tab == 0) tabGeneral(fxDirty);
        else if (ui.tab == 1) tabColor(fxDirty);
        else tabSettings();

        // scroll by dragging the page with a finger
        if (ImGui::IsWindowHovered() && ImGui::IsMouseDragging(0, 8.f * s) && !ImGui::IsAnyItemActive())
            ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
        ImGui::EndChild();
    }
    ImGui::End();
    ImGui::PopStyleColor();

    if (!open) {
        ui.open = false;
        touchSave();
    }
}

}  // namespace

void menuFrame(int w, int h) {
    if (gFailed || w <= 0 || h <= 0) return;
    if (!gInit) {
        initImGui();
        if (!gInit) return;
    }

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - gLast).count();
    gLast = now;
    if (dt <= 0.f || dt > 0.5f) dt = 1.f / 60.f;
    io.DeltaTime = dt;

    float s = std::min(4.f, std::max(0.6f, (h / 540.f) * ui.scale));
    if (std::fabs(s - gScaleApplied) > 0.001f) {
        applyStyle(s);
        gScaleApplied = s;
    }
    io.FontGlobalScale = s * 0.5f;  // the font is made at 26 px, which is right for s = 2 (1080 px high)

    // the finger, read by the game's main thread, becomes the mouse
    Touch t = getTouch();
    if (t.valid) {
        io.AddMousePosEvent(t.x, (float)h - t.y);
        if (t.down != gLastDown) {
            io.AddMouseButtonEvent(0, t.down);
            gLastDown = t.down;
        }
    } else {
        if (gLastDown) {
            io.AddMouseButtonEvent(0, false);
            gLastDown = false;
        }
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();

    bool fxDirty = false;
    if (ui.showFps) drawFps(w, h, s);
    drawEyeButton(w, h, s);
    if (ui.open) drawPanel(w, h, s, fxDirty);

    ImGui::Render();
    gBlockTouch.store(io.WantCaptureMouse);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    if (fxDirty) {
        publishFx(fx);
        touchSave();
    }
    if (gNeedSave) {
        gSaveTimer += dt;
        if (gSaveTimer > 1.5f) {
            gNeedSave = false;
            settingsSave(ui);
        }
    }
}

}  // namespace vm
