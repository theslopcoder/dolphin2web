// Copyright 2026 dolphin2web Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The official GameCube controller adapter needs raw USB access, which the web build lacks.
// Real controllers are still usable through the browser Gamepad API (see ControllerInterface/Web).

#include "InputCommon/GCAdapter.h"

#include "InputCommon/GCPadStatus.h"

namespace GCAdapter
{
void Init()
{
}

void ResetRumble()
{
}

void Shutdown()
{
}

void SetAdapterCallback(std::function<void(void)>)
{
}

GCPadStatus Input(int)
{
  GCPadStatus status{};
  status.isConnected = false;
  return status;
}

void Output(int, u8)
{
}

bool IsDetected(const char** error_message)
{
  if (error_message)
    *error_message = "USB adapters are not available in the browser";
  return false;
}

bool DeviceConnected(int)
{
  return false;
}

void ResetDeviceType(int)
{
}

double GetCurrentPollRate()
{
  return 0.0;
}
}  // namespace GCAdapter
