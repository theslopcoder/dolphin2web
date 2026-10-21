// Copyright 2026 dolphin2web Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/ControllerInterface/Web/Web.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>

#include <emscripten.h>
#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"

namespace
{
// KeyboardEvent.code values exposed as keyboard inputs. The page asks for this list through
// d2w_input_key_name() so the two sides can never disagree on indices.
constexpr std::array<const char*, 76> KEY_CODES = {
    "KeyA",       "KeyB",        "KeyC",        "KeyD",         "KeyE",        "KeyF",
    "KeyG",       "KeyH",        "KeyI",        "KeyJ",         "KeyK",        "KeyL",
    "KeyM",       "KeyN",        "KeyO",        "KeyP",         "KeyQ",        "KeyR",
    "KeyS",       "KeyT",        "KeyU",        "KeyV",         "KeyW",        "KeyX",
    "KeyY",       "KeyZ",        "Digit0",      "Digit1",       "Digit2",      "Digit3",
    "Digit4",     "Digit5",      "Digit6",      "Digit7",       "Digit8",      "Digit9",
    "ArrowUp",    "ArrowDown",   "ArrowLeft",   "ArrowRight",   "Enter",       "Space",
    "ShiftLeft",  "ShiftRight",  "ControlLeft", "ControlRight", "AltLeft",     "AltRight",
    "Tab",        "Backspace",   "Escape",      "Minus",        "Equal",       "BracketLeft",
    "BracketRight", "Semicolon", "Quote",       "Comma",        "Period",      "Slash",
    "Backslash",  "Backquote",   "Numpad0",     "Numpad1",      "Numpad2",     "Numpad3",
    "Numpad4",    "Numpad5",     "Numpad6",     "Numpad7",      "Numpad8",     "Numpad9",
    "F1",         "F2",          "F3",          "F4",
};

constexpr u32 MAX_GAMEPADS = 4;
constexpr u32 GAMEPAD_BUTTONS = 17;  // "standard" Gamepad API mapping
constexpr u32 GAMEPAD_AXES = 4;

struct InputState
{
  u8 keys[KEY_CODES.size()];
  u8 gamepad_connected[MAX_GAMEPADS];
  float gamepad_buttons[MAX_GAMEPADS][GAMEPAD_BUTTONS];
  float gamepad_axes[MAX_GAMEPADS][GAMEPAD_AXES];
};

InputState s_state{};
}  // namespace

extern "C" EMSCRIPTEN_KEEPALIVE InputState* d2w_input_state()
{
  return &s_state;
}

extern "C" EMSCRIPTEN_KEEPALIVE u32 d2w_input_key_count()
{
  return static_cast<u32>(KEY_CODES.size());
}

extern "C" EMSCRIPTEN_KEEPALIVE const char* d2w_input_key_name(u32 index)
{
  return index < KEY_CODES.size() ? KEY_CODES[index] : "";
}

// Offsets so the page does not have to hardcode the struct layout.
extern "C" EMSCRIPTEN_KEEPALIVE u32 d2w_input_layout(u32 field)
{
  switch (field)
  {
  case 0:
    return offsetof(InputState, gamepad_connected);
  case 1:
    return offsetof(InputState, gamepad_buttons);
  case 2:
    return offsetof(InputState, gamepad_axes);
  case 3:
    return GAMEPAD_BUTTONS;
  case 4:
    return GAMEPAD_AXES;
  case 5:
    return MAX_GAMEPADS;
  default:
    return 0;
  }
}

namespace ciface::Web
{
namespace
{
class Keyboard final : public Core::Device
{
  class Key final : public Input
  {
  public:
    explicit Key(u32 index) : m_index(index) {}
    std::string GetName() const override { return KEY_CODES[m_index]; }
    ControlState GetState() const override { return s_state.keys[m_index] ? 1.0 : 0.0; }

  private:
    const u32 m_index;
  };

public:
  Keyboard()
  {
    for (u32 i = 0; i < KEY_CODES.size(); ++i)
      AddInput(new Key(i));
  }
  std::string GetName() const override { return "Keyboard"; }
  std::string GetSource() const override { return "Web"; }
  int GetSortPriority() const override { return 5; }
};

class Gamepad final : public Core::Device
{
  class Button final : public Input
  {
  public:
    Button(u32 pad, u32 index) : m_pad(pad), m_index(index) {}
    std::string GetName() const override { return fmt::format("Button {}", m_index); }
    ControlState GetState() const override { return s_state.gamepad_buttons[m_pad][m_index]; }

  private:
    const u32 m_pad, m_index;
  };

  class Axis final : public Input
  {
  public:
    Axis(u32 pad, u32 index, bool positive) : m_pad(pad), m_index(index), m_positive(positive) {}
    std::string GetName() const override
    {
      return fmt::format("Axis {}{}", m_index, m_positive ? '+' : '-');
    }
    ControlState GetState() const override
    {
      const ControlState v = s_state.gamepad_axes[m_pad][m_index];
      return m_positive ? std::max(0.0, v) : std::max(0.0, -v);
    }

  private:
    const u32 m_pad, m_index;
    const bool m_positive;
  };

public:
  explicit Gamepad(u32 pad) : m_pad(pad)
  {
    for (u32 i = 0; i < GAMEPAD_BUTTONS; ++i)
      AddInput(new Button(pad, i));
    for (u32 i = 0; i < GAMEPAD_AXES; ++i)
    {
      AddInput(new Axis(pad, i, false));
      AddInput(new Axis(pad, i, true));
    }
  }
  std::string GetName() const override { return "Gamepad"; }
  std::string GetSource() const override { return "Web"; }
  std::optional<int> GetPreferredId() const override { return static_cast<int>(m_pad); }

private:
  const u32 m_pad;
};

class InputBackend final : public ciface::InputBackend
{
public:
  using ciface::InputBackend::InputBackend;

  void PopulateDevices() override
  {
    GetControllerInterface().AddDevice(std::make_shared<Keyboard>());
    // Gamepads are always present so mappings stay valid; unplugged pads simply read as idle.
    for (u32 i = 0; i < MAX_GAMEPADS; ++i)
      GetControllerInterface().AddDevice(std::make_shared<Gamepad>(i));
  }
};
}  // namespace

std::unique_ptr<ciface::InputBackend> CreateInputBackend(ControllerInterface* controller_interface)
{
  return std::make_unique<InputBackend>(controller_interface);
}
}  // namespace ciface::Web
