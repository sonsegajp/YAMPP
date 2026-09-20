// Linux settings backend. Stores key=value pairs in a plain text file
// instead of the Windows XmlLite XML reader.
#include <aurora/aurora.h>
#include <aurora/gfx.h>
#include <dolphin/gx/GXAurora.h>
#include <SDL3/SDL.h>
#include <imgui.h>
#include "window.hpp"
#include "aspect_preference.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>

namespace {
struct Settings {
    int width = 960, height = 720, scale = 1, volume = 100;
    bool wide = false, fullscreen = false, vsync = false, mute = false, fps = false;
} current;
std::string netplayServer, netplayName = "Player";
std::atomic<float> gain{1.f};
std::atomic<bool> opened{false};
AspectPreference aspect;
std::atomic<bool> applyPending{false};
std::mutex settingsMtx;
std::string path;
std::string error;

std::string sanitized(const char* value, size_t cap) {
    std::string out;
    for (; value && *value && out.size() < cap; value++) {
        char c = *value;
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') || c == '.' || c == ':' || c == '-' ||
            c == '_' || c == ' ' || c == '[' || c == ']' || c == '/')
            out.push_back(c);
    }
    return out;
}

void load() {
    const char* p = getenv("MELEE_SETTINGS");
    if (p && *p) {
        path = p;
    } else {
        const char* xdg = getenv("XDG_DATA_HOME");
        const char* home = getenv("HOME");
        if (xdg && xdg[0])
            path = std::string(xdg) + "/melee-pc/settings.cfg";
        else if (home && home[0])
            path = std::string(home) + "/.local/share/melee-pc/settings.cfg";
        else
            path = "user/settings.cfg";
    }
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char key[64], val[256];
        if (sscanf(line, " %63[^= ] = %255[^\r\n]", key, val) != 2) continue;
        if (!strcmp(key, "width")) current.width = std::clamp(atoi(val), 640, 7680);
        else if (!strcmp(key, "height")) current.height = std::clamp(atoi(val), 480, 4320);
        else if (!strcmp(key, "renderScale")) current.scale = std::clamp(atoi(val), 0, 4);
        else if (!strcmp(key, "volume")) current.volume = std::clamp(atoi(val), 0, 100);
        else if (!strcmp(key, "widescreen")) current.wide = atoi(val) != 0;
        else if (!strcmp(key, "fullscreen")) current.fullscreen = atoi(val) != 0;
        else if (!strcmp(key, "vsync")) current.vsync = atoi(val) != 0;
        else if (!strcmp(key, "mute")) current.mute = atoi(val) != 0;
        else if (!strcmp(key, "showFps")) current.fps = atoi(val) != 0;
        else if (!strcmp(key, "netplayServer")) netplayServer = sanitized(val, 120);
        else if (!strcmp(key, "netplayName")) netplayName = sanitized(val, 31);
    }
    fclose(f);
    gain.store(current.mute ? 0.f : current.volume / 100.f);
}

void save() {
    std::error_code directoryError;
    auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, directoryError);
    if (directoryError) { error = "Cannot create settings directory"; return; }
    std::string temp = path + ".tmp";
    FILE* f = fopen(temp.c_str(), "w");
    if (!f) { error = "Cannot save settings"; return; }
    fprintf(f, "width = %d\n", current.width);
    fprintf(f, "height = %d\n", current.height);
    fprintf(f, "renderScale = %d\n", current.scale);
    fprintf(f, "volume = %d\n", current.volume);
    fprintf(f, "widescreen = %d\n", aspect.value());
    fprintf(f, "fullscreen = %d\n", current.fullscreen);
    fprintf(f, "vsync = %d\n", current.vsync);
    fprintf(f, "mute = %d\n", current.mute);
    fprintf(f, "showFps = %d\n", current.fps);
    fprintf(f, "netplayServer = %s\n", netplayServer.c_str());
    fprintf(f, "netplayName = %s\n", netplayName.c_str());
    bool ok = fclose(f) == 0;
    if (!ok || rename(temp.c_str(), path.c_str()) != 0)
        error = "Cannot commit settings";
    else
        error.clear();
}

void apply() {
    Settings applied;
    { std::lock_guard<std::mutex> lock(settingsMtx); applied = current; applied.wide=aspect.value()!=0; }
    aurora::window::set_fullscreen(applied.fullscreen);
    if (!applied.fullscreen)
        aurora::window::set_window_size(applied.width, applied.height);
    aurora::window::set_frame_buffer_scale((float)applied.scale);
    AuroraSetViewportPolicy(applied.wide ? AURORA_VIEWPORT_STRETCH : AURORA_VIEWPORT_FIT);
    aurora_enable_vsync(applied.vsync);
    gain.store(applied.mute ? 0.f : applied.volume / 100.f);
    fprintf(stderr, "[settings] %dx%d scale=%d wide=%d fullscreen=%d vsync=%d volume=%d mute=%d\n",
            applied.width, applied.height, applied.scale, applied.wide,
            applied.fullscreen, applied.vsync, applied.volume, applied.mute);
}
} // namespace

extern "C" void aushim_test_input();

extern "C" void aushim_settings_configure(AuroraConfig* cfg) {
    load();
    aspect.initialize(current.wide);
    cfg->windowWidth = current.width;
    cfg->windowHeight = current.height;
    cfg->vsync = current.vsync;
    cfg->startFullscreen = current.fullscreen;
}
extern "C" void aushim_settings_ready() { apply(); }
extern "C" void aushim_settings_update() {
    aushim_test_input();
    if (applyPending.exchange(false)) apply();
}
extern "C" float aushim_settings_gain() { return gain.load(); }
extern "C" int aushim_settings_captures_input() { return opened.load() ? 1 : 0; }

extern "C" int aushim_settings_widescreen(int set, int value) {
    if (set && aspect.set(value != 0)) { std::lock_guard<std::mutex> lk(settingsMtx); current.wide = aspect.value()!=0; applyPending.store(true); save(); }
    return aspect.value();
}
extern "C" int aushim_settings_aspect_lock(int command) {
    if(command<0)return aspect.locked()?1:0;
    if(command)return aspect.acquire();aspect.release();return aspect.value();
}
extern "C" void aushim_settings_netplay(char* server, unsigned cap_s,
                                        char* name, unsigned cap_n) {
    std::lock_guard<std::mutex> lk(settingsMtx);
    if (server && cap_s) snprintf(server, cap_s, "%s", netplayServer.c_str());
    if (name && cap_n) snprintf(name, cap_n, "%s", netplayName.c_str());
}
extern "C" void aushim_settings_set_netplay(const char* server, const char* name) {
    std::lock_guard<std::mutex> lk(settingsMtx);
    netplayServer = sanitized(server, 120);
    netplayName = sanitized(name, 31);
    save();
}
extern "C" void aushim_settings_menu() {
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false))
        opened.store(!opened.load());
    static bool tested = false;
    if (!tested) { tested = true; if (getenv("MELEE_TEST_SETTINGS_OPEN")) opened = true; }

    if (current.fps) {
        ImGui::SetNextWindowPos(ImVec2(10, 10));
        ImGui::SetNextWindowBgAlpha(.6f);
        ImGui::Begin("Performance", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs);
        ImGui::Text("%.1f FPS", aurora_get_fps());
        ImGui::End();
    }
    if (!opened) return;

    bool visible = true;
    ImGui::SetNextWindowSize(ImVec2(450, 470), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x / 2,
                                   ImGui::GetIO().DisplaySize.y / 2),
                            ImGuiCond_FirstUseEver, ImVec2(.5f, .5f));
    ImGui::Begin("YAMPP Settings - F1", &visible, ImGuiWindowFlags_NoCollapse);
    bool changed = false;
    Settings edit;
    { std::lock_guard<std::mutex> lk(settingsMtx); edit = current; }

    const int resolutions[][2] = {
        {640, 480}, {960, 720}, {1280, 720}, {1280, 960},
        {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
    char label[64];
    snprintf(label, sizeof label, "%d x %d", edit.width, edit.height);
    if (ImGui::BeginCombo("Window resolution", label)) {
        for (auto& r : resolutions) {
            snprintf(label, sizeof label, "%d x %d", r[0], r[1]);
            if (ImGui::Selectable(label, edit.width == r[0] && edit.height == r[1])) {
                edit.width = r[0]; edit.height = r[1]; changed = true;
            }
        }
        ImGui::EndCombo();
    }
    const char* scales[] = {"Match window", "1x (480p)", "2x (960p)", "3x (1440p)", "4x (1920p)"};
    changed |= ImGui::Combo("Render resolution", &edit.scale, scales, 5);
    edit.wide=aspect.value()!=0;
    ImGui::BeginDisabled(aspect.locked());
    if(ImGui::Checkbox("Widescreen (16:9)", &edit.wide))aushim_settings_widescreen(1,edit.wide);
    ImGui::EndDisabled();
    if(aspect.locked())ImGui::TextDisabled("Disconnect from Online to change aspect ratio.");
    changed |= ImGui::Checkbox("Fullscreen", &edit.fullscreen);
    changed |= ImGui::Checkbox("VSync", &edit.vsync);
    ImGui::Separator();
    changed |= ImGui::SliderInt("Master volume", &edit.volume, 0, 100, "%d%%");
    changed |= ImGui::Checkbox("Mute", &edit.mute);
    changed |= ImGui::Checkbox("Show FPS", &edit.fps);
    ImGui::Separator();

    static char player[32];
    static bool loadedNetplay = false;
    if (!loadedNetplay) {
        loadedNetplay = true;
        std::lock_guard<std::mutex> lk(settingsMtx);
        snprintf(player, sizeof player, "%s", netplayName.c_str());
    }
    ImGui::TextUnformatted("Online");
    if (ImGui::InputText("Player name", player, sizeof player)) {
        std::lock_guard<std::mutex> lk(settingsMtx);
        netplayName = sanitized(player, 31); save();
    }
    ImGui::TextDisabled("Choose Online on the main menu to connect.");
    ImGui::Separator();
    ImGui::TextWrapped("Keyboard: WASD move, X attack, Z special, C/V jump, Q/E shield, Enter start.");
    ImGui::TextWrapped("F1 closes this menu. Settings save automatically.");
    { std::lock_guard<std::mutex> lk(settingsMtx);
      if (!error.empty())
        ImGui::TextColored(ImVec4(1, .4f, .4f, 1), "%s", error.c_str());
    }
    if (ImGui::Button("Close settings")) visible = false;
    if (changed) {
        std::lock_guard<std::mutex> lk(settingsMtx);
        current = edit;
        gain.store(current.mute ? 0.f : current.volume / 100.f);
        current.wide=aspect.value()!=0;
        applyPending.store(true);
        save();
    }
    ImGui::End();
    if (!visible) opened = false;
}
