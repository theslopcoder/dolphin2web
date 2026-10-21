// Copyright 2026 dolphin2web Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/GL/GLInterface/Web.h"

#include <atomic>

#include <emscripten.h>
#include <emscripten/html5_webgl.h>

#include "Common/Logging/Log.h"

namespace
{
std::atomic<u32> s_surface_width{640};
std::atomic<u32> s_surface_height{528};
}  // namespace

namespace WebSurface
{
void SetSize(u32 width, u32 height)
{
  if (width == 0 || height == 0)
    return;
  s_surface_width.store(width);
  s_surface_height.store(height);
}
u32 GetWidth()
{
  return s_surface_width.load();
}
u32 GetHeight()
{
  return s_surface_height.load();
}
}  // namespace WebSurface

extern "C" EMSCRIPTEN_KEEPALIVE void d2w_set_surface_size(u32 width, u32 height)
{
  WebSurface::SetSize(width, height);
}

EM_JS_DEPS(d2w_gl, "$GL");

// Creates a WebGL 2 context on a new OffscreenCanvas owned by the calling worker.
EM_JS(int, d2w_gl_create, (int width, int height), {
  if (typeof OffscreenCanvas === 'undefined')
    return 0;
  var canvas = new OffscreenCanvas(width, height);
  var attributes = {
    alpha : false,
    depth : false,
    stencil : false,
    antialias : false,
    premultipliedAlpha : false,
    preserveDrawingBuffer : false,
    powerPreference : 'high-performance',
    desynchronized : true,
    failIfMajorPerformanceCaveat : false,
  };
  var ctx = canvas.getContext('webgl2', attributes);
  if (!ctx)
    return 0;
  Module.d2wCanvas = canvas;
  var handle = GL.registerContext(ctx, {majorVersion : 2, minorVersion : 0,
                                        enableExtensionsByDefault : true});
  GL.makeContextCurrent(handle);
  return handle;
});

EM_JS(void, d2w_gl_resize, (int width, int height), {
  var canvas = Module.d2wCanvas;
  if (canvas && (canvas.width != width || canvas.height != height))
  {
    canvas.width = width;
    canvas.height = height;
  }
});

// Hands the finished frame to the page. 9 == CMD_CALL_HANDLER in Emscripten's pthread protocol,
// which makes the main thread call Module.d2wPresent(bitmap).
EM_JS(void, d2w_gl_present, (), {
  var canvas = Module.d2wCanvas;
  if (!canvas)
    return;
  var bitmap = canvas.transferToImageBitmap();
  postMessage({cmd : 9, handler : 'd2wPresent', args : [bitmap]}, [bitmap]);
});

GLContextWeb::~GLContextWeb()
{
  if (m_handle)
    emscripten_webgl_destroy_context(m_handle);
}

bool GLContextWeb::Initialize(const WindowSystemInfo& wsi, bool stereo, bool core)
{
  m_backbuffer_width = WebSurface::GetWidth();
  m_backbuffer_height = WebSurface::GetHeight();
  m_handle = d2w_gl_create(static_cast<int>(m_backbuffer_width),
                           static_cast<int>(m_backbuffer_height));
  if (!m_handle)
  {
    ERROR_LOG_FMT(VIDEO, "Failed to create a WebGL 2 context");
    return false;
  }
  m_opengl_mode = Mode::OpenGLES;
  return true;
}

bool GLContextWeb::MakeCurrent()
{
  return emscripten_webgl_make_context_current(m_handle) == EMSCRIPTEN_RESULT_SUCCESS;
}

bool GLContextWeb::ClearCurrent()
{
  return emscripten_webgl_make_context_current(0) == EMSCRIPTEN_RESULT_SUCCESS;
}

void GLContextWeb::Update()
{
  const u32 width = WebSurface::GetWidth();
  const u32 height = WebSurface::GetHeight();
  if (width == m_backbuffer_width && height == m_backbuffer_height)
    return;
  d2w_gl_resize(static_cast<int>(width), static_cast<int>(height));
  m_backbuffer_width = width;
  m_backbuffer_height = height;
}

void GLContextWeb::Swap()
{
  d2w_gl_present();
}

void GLContextWeb::SwapInterval(int interval)
{
  // Presentation is paced by the page (requestAnimationFrame), not by the context.
}

void* GLContextWeb::GetFuncAddress(const std::string& name)
{
  return emscripten_webgl_get_proc_address(name.c_str());
}
