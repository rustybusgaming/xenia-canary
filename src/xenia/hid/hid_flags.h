/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_HID_FLAGS_H_
#define XENIA_HID_HID_FLAGS_H_

#include <string>

#include "xenia/base/cvar.h"

DECLARE_bool(guide_button);
DECLARE_string(controller_type);

namespace xe {
namespace hid {
// Changes the controller_type option, saving it to the config.
void SetControllerTypeCvar(const std::string& controller_type);
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_HID_FLAGS_H_
