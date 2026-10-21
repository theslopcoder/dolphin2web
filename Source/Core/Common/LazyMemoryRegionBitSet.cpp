// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/LazyMemoryRegionBitSet.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "Common/Align.h"
#include "Common/MsgHandler.h"

namespace Common
{
#ifdef __EMSCRIPTEN__
LazyMemoryRegionBitSet::LazyMemoryRegionBitSet(size_t bits) : m_bits(bits)
{
  m_num_pages = (bits + PAGE_BITS - 1) >> PAGE_BITS_SHIFT;
  m_pages = static_cast<u32**>(std::calloc(m_num_pages, sizeof(u32*)));
  m_valid = m_pages != nullptr;
  if (!m_valid)
    PanicAlertFmt("LazyMemoryRegionBitSet failed to create backing region");
}

LazyMemoryRegionBitSet::~LazyMemoryRegionBitSet()
{
  Clear();
  std::free(m_pages);
}

void LazyMemoryRegionBitSet::Clear()
{
  for (size_t i = 0; i < m_num_pages; ++i)
  {
    std::free(m_pages[i]);
    m_pages[i] = nullptr;
  }
}

u32* LazyMemoryRegionBitSet::GetPageForWrite(size_t page_index)
{
  u32*& page = m_pages[page_index];
  if (!page)
    page = static_cast<u32*>(std::calloc(PAGE_BITS / 32, sizeof(u32)));
  return page;
}

void LazyMemoryRegionBitSet::SetBits(size_t bits_start, size_t bits_end)
{
  // Ranges are short (a JIT block or a cache line), so a simple loop is fine.
  for (size_t bit = bits_start; bit < bits_end; ++bit)
    SetBit(bit);
}

void LazyMemoryRegionBitSet::ClearBits(size_t bits_start, size_t bits_end)
{
  // Invalidation ranges can be huge (whole-RAM DMA), so skip pages that were never written.
  size_t bit = bits_start;
  while (bit < bits_end)
  {
    const size_t page_end = std::min(bits_end, (bit | PAGE_BITS_MASK) + 1);
    if (m_pages[bit >> PAGE_BITS_SHIFT])
    {
      for (; bit < page_end; ++bit)
        ClearBit(bit);
    }
    bit = page_end;
  }
}
#else
LazyMemoryRegionBitSet::LazyMemoryRegionBitSet(size_t bits) : m_bits(bits), m_valid(Create(bits))
{
}

bool LazyMemoryRegionBitSet::Create(size_t bits)
{
  m_pointer = reinterpret_cast<u32*>(m_region.Create(Common::AlignUp(bits, 32) / 8));
  if (!m_pointer)
  {
    PanicAlertFmt("LazyMemoryRegionBitSet failed to create backing region");
    return false;
  }

  return true;
}

void LazyMemoryRegionBitSet::Clear()
{
  m_region.Clear();
}

void LazyMemoryRegionBitSet::CheckBitIndicesWriteable(size_t bits_start, size_t bits_end)
{
  CheckBitIndexReadable(bits_end - 1);
  const size_t start_byte = Common::AlignDown(bits_start, 32) / 8;
  const size_t end_byte = Common::AlignUp(bits_end, 32) / 8;
  m_region.EnsureMemoryPagesWritable(start_byte, end_byte - start_byte);
}

void LazyMemoryRegionBitSet::SetBits(size_t bits_start, size_t bits_end)
{
  if (bits_start >= bits_end)
    return;

  CheckBitIndicesWriteable(bits_start, bits_end);

  const u32 extra_first_word = 0xFFFFFFFF << bits_start % 32;
  const u32 extra_last_word = ~(0xFFFFFFFF << bits_end % 32);

  if (bits_start / 32 == bits_end / 32)
  {
    m_pointer[bits_start / 32] |= extra_first_word & extra_last_word;
    return;
  }

  if (bits_start % 32 != 0)
    m_pointer[bits_start / 32] |= extra_first_word;

  const size_t start_index = Common::DivideRoundingUp(bits_start, 32);
  const size_t end_index = bits_end / 32;
  std::memset(m_pointer + start_index, 0xFF, (end_index - start_index) * 4);

  if (bits_end % 32 != 0)
    m_pointer[bits_end / 32] |= extra_last_word;
}

void LazyMemoryRegionBitSet::ClearBits(size_t bits_start, size_t bits_end)
{
  if (bits_start >= bits_end)
    return;

  CheckBitIndicesWriteable(bits_start, bits_end);

  const u32 extra_first_word = ~(0xFFFFFFFF << bits_start % 32);
  const u32 extra_last_word = 0xFFFFFFFF << bits_end % 32;

  if (bits_start / 32 == bits_end / 32)
  {
    m_pointer[bits_start / 32] &= extra_first_word | extra_last_word;
    return;
  }

  if (bits_start % 32 != 0)
    m_pointer[bits_start / 32] &= extra_first_word;

  const size_t start_index = Common::DivideRoundingUp(bits_start, 32);
  const size_t end_index = bits_end / 32;
  std::memset(m_pointer + start_index, 0x00, (end_index - start_index) * sizeof(u32));

  if (bits_end % 32 != 0)
    m_pointer[bits_end / 32] &= extra_last_word;
}

#endif  // __EMSCRIPTEN__
}  // namespace Common
