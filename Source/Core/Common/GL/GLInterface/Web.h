// Copyright 2026 dolphin2web Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "Common/GL/GLContext.h"

// WebGL 2 (OpenGL ES 3.0) context for the web build.
//
// Rendering happens on Dolphin's emulation thread, which is a Web Worker. The context renders
// into an OffscreenCanvas owned by that worker; Swap() turns the finished frame into an
// ImageBitmap and transfers it (zero-copy) to the page, which shows it in a "bitmaprenderer"
// canvas. This works even though the worker never returns to its event loop.
class GLContextWeb final : public GLContext
{
public:
  ~GLContextWeb() override;

  bool IsHeadless() const override { return false; }

  bool MakeCurrent() override;
  bool ClearCurrent() override;

  void Update() override;

  void Swap() override;
  void SwapInterval(int interval) override;

  void* GetFuncAddress(const std::string& name) override;

protected:
  bool Initialize(const WindowSystemInfo& wsi, bool stereo, bool core) override;

private:
  int m_handle = 0;
};

namespace WebSurface
{
// Size of the visible canvas in device pixels, set from JavaScript.
void SetSize(u32 width, u32 height);
u32 GetWidth();
u32 GetHeight();
}  // namespace WebSurface
