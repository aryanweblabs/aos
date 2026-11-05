/* SPDX-License-Identifier: MIT */
// backlight.hpp - hardware LCD backlight (Intel integrated graphics PWM). Not available => the Control Panel dims in software.
#pragma once
namespace aos::backlight {
bool init();
bool available();
void set(int percent);     // clamped to 5..100 so the screen can never go black
int  get();
}
