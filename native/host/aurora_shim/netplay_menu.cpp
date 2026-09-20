// In-match netplay status overlay. The lobby, room browser and rules are drawn
// by the game itself as original Melee menus (native/host/gxrt/hooks.c); the
// only thing left here is a corner readout the original art has no room for:
// the input delay, a warning while the game waits for the other player, and a
// desync notice.
#include "aurora_shim.h"
#include "profile.h"
#include <imgui.h>
#include <SDL3/SDL.h>
#include <atomic>
#include "../netplay_ui.h"
namespace {
MeleeNetplayUi* ui;
int (*name_input)(int,const char*);
std::atomic<bool> keyboard_captured{false};
SDL_Window* text_window;
bool watch_installed;
bool SDLCALL room_name_event(void*, SDL_Event* event) {
  if (!ui || !ui->room_name_open || !name_input || !text_window) return true;
  const SDL_WindowID window = SDL_GetWindowID(text_window);
  if (event->type == SDL_EVENT_TEXT_INPUT && event->text.windowID == window)
    name_input(NETPLAY_NAME_APPEND, event->text.text);
  else if (event->type == SDL_EVENT_KEY_DOWN && event->key.windowID == window) {
    if (event->key.key == SDLK_BACKSPACE) name_input(NETPLAY_NAME_BACKSPACE, nullptr);
    else if (!event->key.repeat && (event->key.key == SDLK_RETURN || event->key.key == SDLK_KP_ENTER)) name_input(NETPLAY_NAME_CONFIRM, nullptr);
    else if (!event->key.repeat && event->key.key == SDLK_ESCAPE) name_input(NETPLAY_NAME_CANCEL, nullptr);
    else if (!event->key.repeat && (event->key.mod & SDL_KMOD_CTRL) && event->key.key == SDLK_A) name_input(NETPLAY_NAME_CLEAR, nullptr);
    else if (!event->key.repeat && (event->key.mod & SDL_KMOD_CTRL) && event->key.key == SDLK_V) {
      char* text = SDL_GetClipboardText(); if (text) { name_input(NETPLAY_NAME_APPEND, text); SDL_free(text); }
    }
  }
  return true;
}
void room_name_keyboard() {
  const bool opened = ui && ui->room_name_open;
  if (opened && !text_window) {
    int count = 0; SDL_Window** windows = SDL_GetWindows(&count);
    if (windows && count) text_window = windows[0];
    SDL_free(windows);
    if (text_window) SDL_StartTextInput(text_window);
  } else if (!opened && text_window) {
    SDL_StopTextInput(text_window); text_window = nullptr;
  }
  bool capture = opened;
  if (!capture && keyboard_captured.load()) {
    int count = 0; const bool* keys = SDL_GetKeyboardState(&count);
    for (int i = 0; i < count; ++i) if (keys[i]) { capture = true; break; }
  }
  keyboard_captured.store(capture);
}
}
extern "C" AUSHIM_API void aushim_netplay_bind(void* state) {
  auto* s = static_cast<MeleeNetplayUi*>(state);
  ui = (s && s->size == sizeof(MeleeNetplayUi) && s->version == NETPLAY_UI_VERSION) ? s : nullptr;
}
extern "C" AUSHIM_API void aushim_room_name_bind(int (*input)(int,const char*)) {
  name_input = input;
  if (!watch_installed) { SDL_AddEventWatch(room_name_event, nullptr); watch_installed = true; }
}
extern "C" int aushim_room_name_text_ready() { return ui && ui->room_name_open && text_window; }
extern "C" int aushim_netplay_keyboard_captured() { return (ui && ui->room_name_open) || keyboard_captured.load() || aushim_profile_capturing() ? 1 : 0; }
extern "C" int aushim_netplay_captures_input() { return 0; }
extern "C" AUSHIM_API void aushim_netplay_menu() {
  room_name_keyboard();
  if (!ui || !ui->session_active) return;
  const ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 10, 10), ImGuiCond_Always, ImVec2(1, 0));
  ImGui::SetNextWindowBgAlpha(.55f);
  ImGui::Begin("Online status", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
               ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
  ImGui::TextColored(ImVec4(.68f, .91f, .43f, 1), "ONLINE   delay %d", ui->delay);
  if (ui->stalled_ms > 250) ImGui::TextColored(ImVec4(1, .8f, .3f, 1), "Waiting for the other player (%.1fs)", ui->stalled_ms / 1000.f);
  if (ui->desynced) ImGui::TextColored(ImVec4(1, .4f, .4f, 1), "Desync detected: this match no longer matches your opponent's");
  ImGui::End();
}
