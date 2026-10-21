// Copyright 2026 dolphin2web Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// A tiny WebAssembly binary writer used by the dolphin2web PowerPC -> WebAssembly JIT.
// It only knows the handful of opcodes the JIT needs, and builds a module of the form:
//
//   (import "e" "t" (table funcref))          ;; the host module's indirect function table
//   (import "e" "m" (memory shared))          ;; the host module's linear memory
//   (func (export "f") ...)                   ;; one compiled PowerPC block
//
// Because the generated code shares the table and memory with Dolphin itself, it can read and
// write PowerPCState at fixed addresses and call back into C++ through plain function pointers.

#include <cstring>
#include <vector>

#include "Common/CommonTypes.h"

namespace WasmGen
{
enum class ValType : u8
{
  I32 = 0x7F,
  I64 = 0x7E,
  F32 = 0x7D,
  F64 = 0x7C,
};

// Function type indices used by every generated module (see WasmEmitter::BuildModule).
enum TypeIndex : u32
{
  TYPE_VOID_VOID = 0,     // () -> ()           compiled block
  TYPE_I32I32_I32 = 1,    // (i32, i32) -> i32  CachedInterpreter callback
  TYPE_I32_I32 = 2,       // (i32) -> i32       memory read helper
  TYPE_I32I32_VOID = 3,   // (i32, i32) -> ()   memory write helper
  TYPE_VOID_I32 = 4,      // () -> i32
  TYPE_I32_VOID = 5,      // (i32) -> ()
  NUM_TYPES
};

namespace Op
{
constexpr u8 Unreachable = 0x00;
constexpr u8 Block = 0x02;
constexpr u8 Loop = 0x03;
constexpr u8 If = 0x04;
constexpr u8 Else = 0x05;
constexpr u8 End = 0x0B;
constexpr u8 Br = 0x0C;
constexpr u8 BrIf = 0x0D;
constexpr u8 Return = 0x0F;
constexpr u8 CallIndirect = 0x11;
constexpr u8 Drop = 0x1A;
constexpr u8 Select = 0x1B;
constexpr u8 LocalGet = 0x20;
constexpr u8 LocalSet = 0x21;
constexpr u8 LocalTee = 0x22;
constexpr u8 I32Load = 0x28;
constexpr u8 I64Load = 0x29;
constexpr u8 I32Load8U = 0x2D;
constexpr u8 I32Load16U = 0x2F;
constexpr u8 I32Store = 0x36;
constexpr u8 I64Store = 0x37;
constexpr u8 I32Store8 = 0x3A;
constexpr u8 I32Store16 = 0x3B;
constexpr u8 I32Const = 0x41;
constexpr u8 I64Const = 0x42;
constexpr u8 I32Eqz = 0x45;
constexpr u8 I32Eq = 0x46;
constexpr u8 I32Ne = 0x47;
constexpr u8 I32LtS = 0x48;
constexpr u8 I32LtU = 0x49;
constexpr u8 I32GtS = 0x4A;
constexpr u8 I32GtU = 0x4B;
constexpr u8 I32LeU = 0x4D;
constexpr u8 I32GeU = 0x4F;
constexpr u8 I64Eqz = 0x50;
constexpr u8 I64GtS = 0x55;
constexpr u8 I32Clz = 0x67;
constexpr u8 I32Add = 0x6A;
constexpr u8 I32Sub = 0x6B;
constexpr u8 I32Mul = 0x6C;
constexpr u8 I32And = 0x71;
constexpr u8 I32Or = 0x72;
constexpr u8 I32Xor = 0x73;
constexpr u8 I32Shl = 0x74;
constexpr u8 I32ShrS = 0x75;
constexpr u8 I32ShrU = 0x76;
constexpr u8 I32Rotl = 0x77;
constexpr u8 I64Or = 0x84;
constexpr u8 I64And = 0x83;
constexpr u8 I64Shl = 0x86;
constexpr u8 I64ShrU = 0x88;
constexpr u8 I32WrapI64 = 0xA7;
constexpr u8 I64ExtendI32S = 0xAC;
constexpr u8 I64ExtendI32U = 0xAD;
constexpr u8 I32Extend8S = 0xC0;
constexpr u8 I32Extend16S = 0xC1;
constexpr u8 BlockTypeVoid = 0x40;
}  // namespace Op

class WasmEmitter
{
public:
  void Reset()
  {
    m_body.clear();
    m_locals.clear();
  }

  // Declares a new local of the given type and returns its index.
  u32 AddLocal(ValType type)
  {
    m_locals.push_back(type);
    return static_cast<u32>(m_locals.size() - 1);
  }

  void U8(u8 v) { m_body.push_back(v); }
  void U32(u32 v) { WriteULEB(m_body, v); }

  void I32Const(s32 v)
  {
    U8(Op::I32Const);
    WriteSLEB(m_body, v);
  }
  void I32ConstU(u32 v) { I32Const(static_cast<s32>(v)); }
  void I64Const(s64 v)
  {
    U8(Op::I64Const);
    WriteSLEB(m_body, v);
  }

  void LocalGet(u32 i)
  {
    U8(Op::LocalGet);
    U32(i);
  }
  void LocalSet(u32 i)
  {
    U8(Op::LocalSet);
    U32(i);
  }
  void LocalTee(u32 i)
  {
    U8(Op::LocalTee);
    U32(i);
  }

  // Memory access with the effective address on the stack plus a constant offset.
  void MemOp(u8 op, u32 align_log2, u32 offset)
  {
    U8(op);
    U32(align_log2);
    U32(offset);
  }

  // Loads/stores at an absolute address (PowerPCState fields live at fixed addresses).
  void LoadAbs32(u32 address)
  {
    I32Const(0);
    MemOp(Op::I32Load, 2, address);
  }
  void LoadAbs64(u32 address)
  {
    I32Const(0);
    MemOp(Op::I64Load, 3, address);
  }
  void LoadAbs8U(u32 address)
  {
    I32Const(0);
    MemOp(Op::I32Load8U, 0, address);
  }
  // Value must already be on the stack *after* the address; use the Begin/End pairs instead.
  void BeginStoreAbs() { I32Const(0); }
  void EndStoreAbs32(u32 address) { MemOp(Op::I32Store, 2, address); }
  void EndStoreAbs64(u32 address) { MemOp(Op::I64Store, 3, address); }
  void EndStoreAbs8(u32 address) { MemOp(Op::I32Store8, 0, address); }

  void CallIndirect(u32 type_index)
  {
    U8(Op::CallIndirect);
    U32(type_index);
    U32(0);  // table index
  }

  void If()
  {
    U8(Op::If);
    U8(Op::BlockTypeVoid);
  }
  void Else() { U8(Op::Else); }
  void End() { U8(Op::End); }
  void Return() { U8(Op::Return); }

  size_t BodySize() const { return m_body.size(); }

  // Builds a complete module. table_min is the current size of the host table and memory_pages
  // the current size of the host memory, both of which only ever grow.
  std::vector<u8> BuildModule(u32 table_min, u32 memory_pages) const
  {
    std::vector<u8> out = {0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};

    // Type section
    {
      std::vector<u8> s;
      WriteULEB(s, NUM_TYPES);
      auto func_type = [&s](std::initializer_list<ValType> params,
                            std::initializer_list<ValType> results) {
        s.push_back(0x60);
        WriteULEB(s, static_cast<u32>(params.size()));
        for (ValType t : params)
          s.push_back(static_cast<u8>(t));
        WriteULEB(s, static_cast<u32>(results.size()));
        for (ValType t : results)
          s.push_back(static_cast<u8>(t));
      };
      func_type({}, {});
      func_type({ValType::I32, ValType::I32}, {ValType::I32});
      func_type({ValType::I32}, {ValType::I32});
      func_type({ValType::I32, ValType::I32}, {});
      func_type({}, {ValType::I32});
      func_type({ValType::I32}, {});
      Section(out, 1, s);
    }

    // Import section
    {
      std::vector<u8> s;
      WriteULEB(s, 2);
      Name(s, "e");
      Name(s, "t");
      s.push_back(0x01);  // table
      s.push_back(0x70);  // funcref
      s.push_back(0x00);  // limits: min only
      WriteULEB(s, table_min);
      Name(s, "e");
      Name(s, "m");
      s.push_back(0x02);  // memory
      s.push_back(0x03);  // limits: min + max, shared
      WriteULEB(s, memory_pages);
      WriteULEB(s, 65536);
      Section(out, 2, s);
    }

    // Function section
    {
      std::vector<u8> s;
      WriteULEB(s, 1);
      WriteULEB(s, TYPE_VOID_VOID);
      Section(out, 3, s);
    }

    // Export section
    {
      std::vector<u8> s;
      WriteULEB(s, 1);
      Name(s, "f");
      s.push_back(0x00);  // func
      WriteULEB(s, 0);
      Section(out, 7, s);
    }

    // Code section
    {
      std::vector<u8> func;
      // Local declarations, run-length encoded by type.
      std::vector<std::pair<u32, ValType>> groups;
      for (ValType t : m_locals)
      {
        if (!groups.empty() && groups.back().second == t)
          groups.back().first++;
        else
          groups.emplace_back(1, t);
      }
      WriteULEB(func, static_cast<u32>(groups.size()));
      for (const auto& [count, type] : groups)
      {
        WriteULEB(func, count);
        func.push_back(static_cast<u8>(type));
      }
      func.insert(func.end(), m_body.begin(), m_body.end());
      func.push_back(Op::End);

      std::vector<u8> s;
      WriteULEB(s, 1);
      WriteULEB(s, static_cast<u32>(func.size()));
      s.insert(s.end(), func.begin(), func.end());
      Section(out, 10, s);
    }

    return out;
  }

private:
  static void WriteULEB(std::vector<u8>& out, u64 v)
  {
    do
    {
      u8 byte = v & 0x7F;
      v >>= 7;
      if (v != 0)
        byte |= 0x80;
      out.push_back(byte);
    } while (v != 0);
  }

  static void WriteSLEB(std::vector<u8>& out, s64 v)
  {
    while (true)
    {
      const u8 byte = v & 0x7F;
      v >>= 7;
      if ((v == 0 && (byte & 0x40) == 0) || (v == -1 && (byte & 0x40) != 0))
      {
        out.push_back(byte);
        return;
      }
      out.push_back(byte | 0x80);
    }
  }

  static void Name(std::vector<u8>& out, const char* name)
  {
    const size_t len = std::strlen(name);
    WriteULEB(out, len);
    out.insert(out.end(), name, name + len);
  }

  static void Section(std::vector<u8>& out, u8 id, const std::vector<u8>& payload)
  {
    out.push_back(id);
    WriteULEB(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
  }

  std::vector<u8> m_body;
  std::vector<ValType> m_locals;
};
}  // namespace WasmGen
