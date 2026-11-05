/* SPDX-License-Identifier: MIT */
// hda.hpp - Intel High Definition Audio driver (PCI class 04:03): codec discovery, output path, volume/mute, tones.
#pragma once
#include <stdint.h>
namespace aos::audio {
bool init();                          // find controller + codec + output path; false if no usable audio hardware
bool available();
void set_volume(int percent);         // 0..100
void set_mute(bool on);
int  volume();
bool muted();
void tone(unsigned freq_hz, unsigned ms);   // non-blocking (max ~250 ms); ends by itself, see poll()
void poll();                          // call from the main loop
int  pin_count();                     // number of output pins (speaker / headphone / line-out) that were wired up
}
