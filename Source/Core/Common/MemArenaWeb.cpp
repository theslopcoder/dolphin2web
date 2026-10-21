// Copyright 2026 dolphin2web Project
// SPDX-License-Identifier: GPL-2.0-or-later

// WebAssembly has a single flat linear memory with no mmap/mprotect tricks, so there is no
// fastmem arena. Emulated memory is a plain allocation and "views" are pointers into it.

#include "Common/MemArena.h"

#include <cstdlib>
#include <cstring>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"

namespace Common
{
MemArena::MemArena() = default;
MemArena::~MemArena()
{
  ReleaseSHMSegment();
}

void MemArena::GrabSHMSegment(size_t size, std::string_view base_name)
{
  ReleaseSHMSegment();
  m_shm_base = static_cast<u8*>(std::aligned_alloc(64 * 1024, Common::AlignUp(size, 64 * 1024)));
  if (!m_shm_base)
  {
    ERROR_LOG_FMT(MEMMAP, "Failed to allocate {} bytes of emulated memory", size);
    return;
  }
  std::memset(m_shm_base, 0, size);
  m_shm_size = size;
}

void MemArena::ReleaseSHMSegment()
{
  std::free(m_shm_base);
  m_shm_base = nullptr;
  m_shm_size = 0;
}

void* MemArena::CreateView(s64 offset, size_t size)
{
  if (!m_shm_base || static_cast<size_t>(offset) + size > m_shm_size)
    return nullptr;
  return m_shm_base + offset;
}

void MemArena::ReleaseView(void* view, size_t size)
{
}

u8* MemArena::ReserveMemoryRegion(size_t memory_size)
{
  // No 4 GiB address-space tricks are possible inside a wasm32 linear memory.
  NOTICE_LOG_FMT(MEMMAP, "Fastmem is not available on the web build");
  return nullptr;
}

void MemArena::ReleaseMemoryRegion()
{
}

void* MemArena::MapInMemoryRegion(s64 offset, size_t size, void* base, bool writeable)
{
  return nullptr;
}

bool MemArena::ChangeMappingProtection(void* view, size_t size, bool writeable)
{
  return false;
}

void MemArena::UnmapFromMemoryRegion(void* view, size_t size)
{
}

size_t MemArena::GetPageSize() const
{
  return 64 * 1024;
}

LazyMemoryRegion::LazyMemoryRegion() = default;

LazyMemoryRegion::~LazyMemoryRegion()
{
  Release();
}

void* LazyMemoryRegion::Create(size_t size)
{
  ASSERT(!m_memory);
  if (size == 0)
    return nullptr;

  void* memory = std::calloc(1, size);
  if (!memory)
  {
    NOTICE_LOG_FMT(MEMMAP, "Memory allocation of {} bytes failed.", size);
    return nullptr;
  }
  m_memory = memory;
  m_size = size;
  return memory;
}

void LazyMemoryRegion::Clear()
{
  ASSERT(m_memory);
  std::memset(m_memory, 0, m_size);
}

void LazyMemoryRegion::Release()
{
  std::free(m_memory);
  m_memory = nullptr;
  m_size = 0;
}
}  // namespace Common
