/* SPDX-License-Identifier: MIT */
// power.hpp - restart / shut down the machine.
#pragma once
namespace aos::power {
[[noreturn]] void restart();
[[noreturn]] void shutdown();
}
