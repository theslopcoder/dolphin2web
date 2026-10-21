// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "Common/Assert.h"
#include "Common/MemArena.h"

namespace Common
{
#ifdef __EMSCRIPTEN__
// dolphin2web: wasm32 has no lazily-committed virtual memory, and the desktop implementation
// would eagerly commit hundreds of MiB (one bit per possible PPC instruction address). Use a
// two-level sparse bit set instead: 8 KiB pages are allocated only when a bit in them is set.
class LazyMemoryRegionBitSet final
{
public:
  LazyMemoryRegionBitSet(size_t bits);
  ~LazyMemoryRegionBitSet();
  LazyMemoryRegionBitSet(const LazyMemoryRegionBitSet&) = delete;
  LazyMemoryRegionBitSet& operator=(const LazyMemoryRegionBitSet&) = delete;

  bool IsValid() const { return m_valid; }

  void Clear();

  bool TestBit(size_t bit) const
  {
    const u32* page = m_pages[bit >> PAGE_BITS_SHIFT];
    if (!page)
      return false;
    const size_t local = bit & PAGE_BITS_MASK;
    return (page[local / 32] & (1u << (local % 32))) != 0;
  }

  void SetBit(size_t bit)
  {
    u32* page = GetPageForWrite(bit >> PAGE_BITS_SHIFT);
    const size_t local = bit & PAGE_BITS_MASK;
    page[local / 32] |= (1u << (local % 32));
  }

  void ClearBit(size_t bit)
  {
    u32* page = m_pages[bit >> PAGE_BITS_SHIFT];
    if (!page)
      return;
    const size_t local = bit & PAGE_BITS_MASK;
    page[local / 32] &= ~(1u << (local % 32));
  }

  void SetBits(size_t bits_start, size_t bits_end);

  void ClearBits(size_t bits_start, size_t bits_end);

  // There is no flat backing memory on the web build.
  const u32* GetRawMemory() const { return nullptr; }

private:
  static constexpr size_t PAGE_BITS_SHIFT = 16;  // 65536 bits = 8 KiB per page
  static constexpr size_t PAGE_BITS = size_t(1) << PAGE_BITS_SHIFT;
  static constexpr size_t PAGE_BITS_MASK = PAGE_BITS - 1;

  u32* GetPageForWrite(size_t page_index);

  u32** m_pages = nullptr;
  size_t m_num_pages = 0;
  size_t m_bits;
  bool m_valid;
};
#else
// A bit set backed by a LazyMemoryRegion.
class LazyMemoryRegionBitSet final
{
public:
  // Allocates backing memory and creates a bit set.
  // Allocating backing memory may fail; use IsValid to check for success.
  LazyMemoryRegionBitSet(size_t bits);

  bool IsValid() const { return m_valid; }

  void Clear();

  bool TestBit(size_t bit) const
  {
    CheckBitIndexReadable(bit);
    return (m_pointer[bit / 32] & (1 << (bit % 32))) != 0;
  }

  void SetBit(size_t bit)
  {
    CheckBitIndexWriteable(bit);
    m_region.EnsureMemoryPageWritable(bit / 8);
    m_pointer[bit / 32] |= (1 << (bit % 32));
  }

  void ClearBit(size_t bit)
  {
    CheckBitIndexWriteable(bit);
    m_region.EnsureMemoryPageWritable(bit / 8);
    m_pointer[bit / 32] &= ~(1 << (bit % 32));
  }

  void SetBits(size_t bits_start, size_t bits_end);

  void ClearBits(size_t bits_start, size_t bits_end);

  const u32* GetRawMemory() const { return m_pointer; }

private:
  LazyMemoryRegion m_region;
  u32* m_pointer;
  size_t m_bits;
  bool m_valid;

  bool Create(size_t bits);

  void CheckBitIndexReadable(size_t bit) const
  {
    DEBUG_ASSERT_MSG(COMMON, m_valid, "LazyMemoryRegionBitSet isn't valid");
    DEBUG_ASSERT_MSG(COMMON, bit < m_bits,
                     "Tried to access bit {} in a LazyMemoryRegionBitSet with only {} bits", bit,
                     m_bits);
  }

  void CheckBitIndexWriteable(size_t bit)
  {
    CheckBitIndexReadable(bit);
    m_region.EnsureMemoryPageWritable(bit / 8);
  }

  void CheckBitIndicesWriteable(size_t bits_start, size_t bits_end);
};

#endif  // __EMSCRIPTEN__
}  // namespace Common
