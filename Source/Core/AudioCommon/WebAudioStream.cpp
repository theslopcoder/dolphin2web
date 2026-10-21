// Copyright 2026 dolphin2web Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "AudioCommon/WebAudioStream.h"

#include <algorithm>
#include <array>
#include <chrono>

#include <emscripten.h>

#include "Common/Thread.h"

namespace
{
// Layout shared with the AudioWorklet in dolphin2web.html. Indices count stereo frames and grow
// without wrapping (they are reduced modulo CAPACITY when used).
struct AudioRing
{
  static constexpr u32 CAPACITY = 8192;  // ~170 ms at 48 kHz, must be a power of two

  std::atomic<u32> write_index;
  std::atomic<u32> read_index;
  u32 capacity;
  u32 sample_rate;
  std::atomic<s32> volume;  // 0-100
  u32 padding[3];
  s16 samples[CAPACITY * 2];
};

alignas(64) AudioRing s_ring{0, 0, AudioRing::CAPACITY, 48000, 100, {}, {}};

// Keep about this many frames queued: enough to survive a slow frame on a Chromebook without
// adding much latency.
constexpr u32 TARGET_QUEUED_FRAMES = 3072;
constexpr u32 CHUNK_FRAMES = 256;
}  // namespace

extern "C" EMSCRIPTEN_KEEPALIVE AudioRing* d2w_audio_ring()
{
  return &s_ring;
}

WebAudioStream::~WebAudioStream()
{
  SetRunning(false);
}

bool WebAudioStream::Init()
{
  s_ring.sample_rate = m_mixer->GetSampleRate();
  return true;
}

bool WebAudioStream::SetRunning(bool running)
{
  if (running == m_run_thread.IsSet())
    return true;

  if (running)
  {
    m_run_thread.Set();
    m_thread = std::thread(&WebAudioStream::FeedThread, this);
  }
  else
  {
    m_run_thread.Clear();
    if (m_thread.joinable())
      m_thread.join();
  }
  return true;
}

void WebAudioStream::SetVolume(int volume)
{
  s_ring.volume.store(std::clamp(volume, 0, 100));
}

void WebAudioStream::FeedThread()
{
  Common::SetCurrentThreadName("Audio feeder");
  std::array<s16, CHUNK_FRAMES * 2> buffer;

  while (m_run_thread.IsSet())
  {
    const u32 write = s_ring.write_index.load(std::memory_order_relaxed);
    const u32 read = s_ring.read_index.load(std::memory_order_acquire);
    const u32 queued = write - read;
    if (queued + CHUNK_FRAMES > TARGET_QUEUED_FRAMES)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }

    m_mixer->Mix(buffer.data(), CHUNK_FRAMES);
    for (u32 i = 0; i < CHUNK_FRAMES; ++i)
    {
      const u32 slot = (write + i) & (AudioRing::CAPACITY - 1);
      s_ring.samples[slot * 2] = buffer[i * 2];
      s_ring.samples[slot * 2 + 1] = buffer[i * 2 + 1];
    }
    s_ring.write_index.store(write + CHUNK_FRAMES, std::memory_order_release);
  }
}
