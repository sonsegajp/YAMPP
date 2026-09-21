#pragma once
#include "aurora_shim.h"
#ifdef __cplusplus
extern "C" {
#endif
void aushim_controllers_init(void);
void aushim_controllers_update(void);
void aushim_controllers_open(int open);
AUSHIM_API int aushim_controllers_back(void);
int aushim_controllers_capturing(void);
int aushim_controller_tap_jump(unsigned port);
int aushim_controller_keyboard_enabled(unsigned port);
void aushim_controller_keyboard_fallback(unsigned port, AushimPadStatus* status);
/* Applies this port's trigger settings to a freshly read pad: dead zone,
 * where the L/R click engages, and whether partial presses light-shield.
 * Everything that hands a pad to the game goes through here, so what the
 * controller panel draws is what the match receives. */
void aushim_controller_triggers(unsigned port, AushimPadStatus* status);
#ifdef __cplusplus
}
#include <string>
struct ControllerMenuView {
  int port, row, first, count, keyboard, binding, live_test, connected, tap_jump;
  AushimPadStatus pad;
  std::string device, message;
  std::string labels[40], values[40];
};
const ControllerMenuView& controller_menu_view();
#endif
