/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/hid_flags.h"

DEFINE_bool(guide_button, true, "Forward guide button presses to guest.",
            "HID");
DEFINE_string(
    controller_type, "auto",
    "Type of controller reported to games for SDL controllers. \"auto\" "
    "reports the type detected by SDL. Set it to \"gamepad\", \"guitar\", "
    "\"guitar_alternate\", \"guitar_bass\", \"drum_kit\", \"wheel\", "
    "\"arcade_stick\", \"flight_stick\", \"dance_pad\" or \"arcade_pad\" "
    "for controllers detected as another type, such as guitars connected "
    "through a dongle presenting them as a gamepad.",
    "HID");

namespace xe {
namespace hid {
void SetControllerTypeCvar(const std::string& controller_type) {
  OVERRIDE_string(controller_type, controller_type);
}
}  // namespace hid
}  // namespace xe
