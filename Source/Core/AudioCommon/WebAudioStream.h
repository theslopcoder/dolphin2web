// Copyright 2026 dolphin2web Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <thread>

#include "AudioCommon/SoundStream.h"
#include "Common/Flag.h"

// Web Audio output for the web build.
//
// A feeder thread pulls samples from the Mixer into a single-producer/single-consumer ring
// buffer in (shared) wasm memory. An AudioWorklet on the page reads the ring directly from the
// SharedArrayBuffer, so no messages or copies cross threads per audio block.
class WebAudioStream final : public SoundStream
{
public:
  ~WebAudioStream() override;

  bool Init() override;
  bool SetRunning(bool running) override;
  void SetVolume(int volume) override;

  static bool IsValid() { return true; }

private:
  void FeedThread();

  std::thread m_thread;
  Common::Flag m_run_thread;
};
