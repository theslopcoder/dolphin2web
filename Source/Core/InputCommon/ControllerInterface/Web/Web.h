// Copyright 2026 dolphin2web Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include "InputCommon/ControllerInterface/InputBackend.h"

class ControllerInterface;

// Keyboard and Gamepad API input for the web build. The page writes key and gamepad state
// straight into a struct in shared wasm memory (see d2w_input_state); devices read it on demand.
namespace ciface::Web
{
std::unique_ptr<ciface::InputBackend> CreateInputBackend(ControllerInterface* controller_interface);
}
