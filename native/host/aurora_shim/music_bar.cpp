// Now-playing bar: a slim strip in the top left that slides in when a stage
// track starts, holds, then slides back out. Melee ships no art for this, so it
// is drawn here; the track name comes from the game's own HPS table, or from a
// custom file (native/host/gxrt/music.c).
#include "aurora_shim.h"
#include <imgui.h>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>

namespace {
std::mutex lock;
std::string track;
bool custom = false;
using Clock = std::chrono::steady_clock;
Clock::time_point shown_at;
const double kSlide = 0.40;   // seconds sliding in and out
const double kHold = 5.5;     // seconds fully out
}

extern "C" AUSHIM_API void aushim_music_bar(const char* text, int is_custom) {
  std::lock_guard<std::mutex> guard(lock);
  if (!text || !*text) { track.clear(); return; }
  track = text;
  custom = is_custom != 0;
  // Track changes arrive from the guest worker; ImGui belongs to the render thread.
  shown_at = Clock::now();
}

extern "C" AUSHIM_API void aushim_music_draw() {
  std::string name;
  bool is_custom;
  double since;
  {
    std::lock_guard<std::mutex> guard(lock);
    if (track.empty()) return;
    name = track;
    is_custom = custom;
    since = std::chrono::duration<double>(Clock::now() - shown_at).count();
  }
  const double total = kSlide + kHold + kSlide;
  if (since < 0.0 || since > total) return;
  float progress = since < kSlide ? (float)(since / kSlide)
                 : since < kSlide + kHold ? 1.f
                 : (float)((total - since) / kSlide);
  progress = std::clamp(progress, 0.f, 1.f);
  const float eased = progress * progress * (3.f - 2.f * progress);

  const ImGuiIO& io = ImGui::GetIO();
  const float scale = std::max(1.f, io.DisplaySize.y / 720.f);
  const float height = 34.f * scale;
  const float pad = 14.f * scale;
  const float note = 16.f * scale;
  const float font = ImGui::GetFontSize() * 1.15f * scale;
  const float text_width = ImGui::GetFont()->CalcTextSizeA(font, FLT_MAX, 0.f, name.c_str()).x;
  const float width = pad + note + 8.f * scale + text_width + pad * 1.6f;

  // Slides in from off the left edge and back out the same way.
  const float left = -width * (1.f - eased);
  const float top = 28.f * scale;
  const float bottom = top + height;
  const float alpha = eased;

  ImDrawList* draw = ImGui::GetForegroundDrawList();
  const ImU32 plate = IM_COL32(12, 14, 20, (int)(210 * alpha));
  const ImU32 ink = IM_COL32(255, 255, 255, (int)(255 * alpha));
  const ImU32 accent = is_custom ? IM_COL32(150, 220, 90, (int)(255 * alpha))
                                 : IM_COL32(215, 225, 245, (int)(255 * alpha));

  // Rounded only on the trailing edge, so it reads as sliding out of the side.
  draw->AddRectFilled(ImVec2(left, top), ImVec2(left + width, bottom), plate, height * .5f,
                      ImDrawFlags_RoundCornersRight);

  // A small music note, drawn rather than typed: the default font has no glyph.
  const float nx = left + pad, ny = (top + bottom) * .5f;
  const float stem = note * .62f;
  draw->AddLine(ImVec2(nx + stem, ny - note * .42f), ImVec2(nx + stem, ny + note * .20f), accent, 1.8f * scale);
  draw->AddCircleFilled(ImVec2(nx + stem * .55f, ny + note * .24f), note * .26f, accent, 12);
  draw->AddLine(ImVec2(nx + stem, ny - note * .42f), ImVec2(nx + stem + note * .34f, ny - note * .26f), accent, 1.8f * scale);

  draw->AddText(nullptr, font, ImVec2(nx + note + 8.f * scale, ny - font * .54f), ink, name.c_str());
}
