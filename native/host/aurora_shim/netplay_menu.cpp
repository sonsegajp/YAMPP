// In-match netplay status overlay. The lobby, room browser and rules are drawn
// by the game itself as original Melee menus (native/host/gxrt/hooks.c); the
// only thing left here is a corner readout the original art has no room for.
//
// It answers the two questions a player actually has when something feels
// wrong: what the link is doing, and whether the match is about to end. A
// held match says so and counts down, because a frozen screen with no
// explanation is what makes an interruption feel like a crash; a desync says
// the games have diverged and that play continues, because the match is
// still playable and stopping it is the players' call.
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
  const ImVec4 good(.68f, .91f, .43f, 1), warn(1, .8f, .3f, 1), bad(1, .4f, .4f, 1), dim(.72f, .77f, .85f, 1);
  ImGui::TextColored(good, "ONLINE   delay %d%s", ui->delay, ui->rules.delay ? "" : " auto");
  // Which way the inputs are actually travelling. "peer to peer" means the
  // server is no longer in the path at all, which is the difference the
  // player feels; the two relayed states differ only in how a lost packet is
  // repaired, and both are worth distinguishing when a connection is poor.
  const char* route = ui->direct_peers ? "peer to peer"
                    : ui->udp_active ? "via server"
                    : "via server (stream)";
  if (ui->ping_ms >= 0)
    ImGui::TextColored(dim, "%d ms   %s", ui->ping_ms, route);
  else
    ImGui::TextColored(dim, "measuring link   %s", route);
  if (ui->interrupted) {
    // A countdown, not a spinner: the player can see whether to wait.
    const float left = (ui->interrupt_limit_ms - ui->interrupt_ms) / 1000.f;
    ImGui::TextColored(warn, "%s", ui->interrupt_text[0] ? ui->interrupt_text : "Waiting for opponent");
    ImGui::TextColored(warn, "Holding the match - %.0fs left", left < 0 ? 0.f : left);
  } else if (ui->stalled_ms > 250) {
    ImGui::TextColored(warn, "Waiting for the other player (%.1fs)", ui->stalled_ms / 1000.f);
  }
  if (ui->desynced)
    ImGui::TextColored(bad, "Desync: the two games have diverged. Play continues.");
  if (ui->clock_skips || ui->frame_advantage)
    ImGui::TextColored(dim, "clock %+d   held %d", ui->frame_advantage, ui->clock_skips);
  ImGui::End();
}
