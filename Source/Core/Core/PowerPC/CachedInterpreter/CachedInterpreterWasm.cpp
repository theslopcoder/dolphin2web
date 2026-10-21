// Copyright 2026 dolphin2web Project
// SPDX-License-Identifier: GPL-2.0-or-later

// dolphin2web: PowerPC -> WebAssembly recompiler.
//
// Browsers do not let us write native machine code, but they do let us create new WebAssembly
// modules at runtime, which the browser then compiles to native code (Liftoff, then TurboFan for
// hot code). This file turns hot Cached Interpreter blocks into such modules.
//
// The Cached Interpreter already converts each PowerPC block into a list of "callbacks". We walk
// that list and emit equivalent WebAssembly:
//  * Common integer, load/store and branch instructions are emitted as native WebAssembly that
//    reads and writes PowerPCState directly (the guest GPRs are cached in wasm locals).
//  * Every other callback is emitted as a direct call_indirect to the very same C++ callback the
//    Cached Interpreter would have run, so behaviour is identical by construction.
// This removes the interpreter dispatch loop entirely for compiled blocks.

#ifdef __EMSCRIPTEN__

#include "Core/PowerPC/CachedInterpreter/CachedInterpreter.h"

#include <array>
#include <cstddef>
#include <vector>

#include <emscripten.h>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/PowerPC/CachedInterpreter/WasmEmitter.h"
#include "Core/PowerPC/ConditionRegister.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

// ----------------------------------------------------------------------------------------------
// JavaScript bridge. Runs on the CPU thread (a Web Worker), where synchronous compilation of
// modules of any size is allowed.

EM_JS_DEPS(d2w_jit, "$wasmTable");

// Compiles and instantiates a block module and stores its function at table[slot].
// Returns 1 on success, 0 on failure.
EM_JS(int, d2w_jit_install, (const u8* bytes, u32 length, u32 slot), {
  try {
    // WebAssembly.Module does not accept views of shared memory, so copy the bytes out first.
    var module = new WebAssembly.Module(HEAPU8.slice(bytes, bytes + length));
    var instance = new WebAssembly.Instance(module, {e : {t : wasmTable, m : wasmMemory}});
    wasmTable.set(slot, instance.exports.f);
    return 1;
  } catch (e) {
    err('dolphin2web JIT: failed to compile block: ' + e);
    return 0;
  }
});

// Grows the table by `count` entries and returns the index of the first new entry.
EM_JS(u32, d2w_jit_grow_table, (u32 count), { return wasmTable.grow(count, null); });

EM_JS(u32, d2w_jit_table_size, (), { return wasmTable.length; });

EM_JS(u32, d2w_jit_memory_pages, (), { return wasmMemory.buffer.byteLength >>> 16; });

EM_JS(void, d2w_jit_clear_slot, (u32 slot), { wasmTable.set(slot, null); });

// ----------------------------------------------------------------------------------------------
// C++ helpers called from generated code through function pointers (= table indices).

namespace
{
PowerPC::MMU* s_mmu = nullptr;
PowerPC::PowerPCState* s_ppc_state = nullptr;

u32 WasmRead8(u32 address)
{
  return s_mmu->Read<u8>(address);
}
u32 WasmRead16(u32 address)
{
  return s_mmu->Read<u16>(address);
}
u32 WasmRead32(u32 address)
{
  return s_mmu->Read<u32>(address);
}
void WasmWrite8(u32 value, u32 address)
{
  s_mmu->Write<u8>(value, address);
}
void WasmWrite16(u32 value, u32 address)
{
  s_mmu->Write<u16>(value, address);
}
void WasmWrite32(u32 value, u32 address)
{
  s_mmu->Write<u32>(value, address);
}

struct PerfMonOperands
{
  u32 downcount;
  u32 num_load_stores;
  u32 num_fp_inst;
};

void WasmUpdatePerfMon(const PerfMonOperands* operands)
{
  PowerPC::UpdatePerformanceMonitor(operands->downcount, operands->num_load_stores,
                                    operands->num_fp_inst, *s_ppc_state);
}

template <typename T>
u32 FuncIndex(T* function)
{
  return static_cast<u32>(reinterpret_cast<uintptr_t>(function));
}

u32 Addr(const void* p)
{
  return static_cast<u32>(reinterpret_cast<uintptr_t>(p));
}

// Raw instruction field decoding.
constexpr u32 OPCD(u32 i)
{
  return i >> 26;
}
constexpr u32 RD(u32 i)
{
  return (i >> 21) & 31;
}
constexpr u32 RA(u32 i)
{
  return (i >> 16) & 31;
}
constexpr u32 RB(u32 i)
{
  return (i >> 11) & 31;
}
constexpr u32 MB(u32 i)
{
  return (i >> 6) & 31;
}
constexpr u32 ME(u32 i)
{
  return (i >> 1) & 31;
}
constexpr u32 SUBOP10(u32 i)
{
  return (i >> 1) & 0x3FF;
}
constexpr bool Rc(u32 i)
{
  return (i & 1) != 0;
}
constexpr s32 SIMM(u32 i)
{
  return static_cast<s16>(i & 0xFFFF);
}
constexpr u32 UIMM(u32 i)
{
  return i & 0xFFFF;
}
constexpr u32 CRFD(u32 i)
{
  return (i >> 23) & 7;
}

constexpr u32 RotationMask(u32 mb, u32 me)
{
  const u32 begin = 0xFFFFFFFFu >> mb;
  const u32 end = me < 31 ? (0xFFFFFFFFu >> (me + 1)) : 0;
  const u32 mask = begin ^ end;
  return me < mb ? ~mask : mask;
}

using namespace WasmGen;

// Generates the body of one block function.
class BlockCompiler
{
public:
  explicit BlockCompiler(PowerPC::PowerPCState& ppc) : m_ppc(ppc)
  {
    for (u32 i = 0; i < 32; ++i)
      e.AddLocal(ValType::I32);  // locals 0-31: cached guest GPRs
    m_t0 = e.AddLocal(ValType::I32);
    m_t1 = e.AddLocal(ValType::I32);
    m_t2 = e.AddLocal(ValType::I32);
  }

  WasmEmitter e;

  // ---- PowerPCState addresses
  u32 GprAddr(u32 r) const { return Addr(&m_ppc.gpr[r]); }
  u32 PcAddr() const { return Addr(&m_ppc.pc); }
  u32 NpcAddr() const { return Addr(&m_ppc.npc); }
  u32 DowncountAddr() const { return Addr(&m_ppc.downcount); }
  u32 ExceptionsAddr() const { return Addr(&m_ppc.Exceptions); }
  u32 CaAddr() const { return Addr(&m_ppc.xer_ca); }
  u32 SoOvAddr() const { return Addr(&m_ppc.xer_so_ov); }
  u32 SprAddr(u32 spr) const { return Addr(&m_ppc.spr[spr]); }
  u32 CrAddr(u32 field) const { return Addr(&m_ppc.cr.fields[field]); }

  // ---- Guest register cache
  void GetGpr(u32 r)
  {
    if (m_state[r] == State::Uncached)
    {
      e.LoadAbs32(GprAddr(r));
      e.LocalSet(r);
      m_state[r] = State::Clean;
    }
    e.LocalGet(r);
  }
  // Pops the value on top of the stack into guest register r.
  void SetGpr(u32 r)
  {
    e.LocalSet(r);
    m_state[r] = State::Dirty;
  }
  // Writes back every dirty register. If keep_state is false the cache state is updated too
  // (for straight-line code); exit paths inside a conditional pass true so the main path's
  // view of the cache is unaffected.
  void Flush(bool keep_state = false)
  {
    for (u32 r = 0; r < 32; ++r)
    {
      if (m_state[r] != State::Dirty)
        continue;
      e.I32Const(0);
      e.LocalGet(r);
      e.EndStoreAbs32(GprAddr(r));
      if (!keep_state)
        m_state[r] = State::Clean;
    }
  }
  // Called after anything that might have modified guest GPRs behind our back.
  void Invalidate() { m_state.fill(State::Uncached); }

  // ---- Common sequences
  void GetSO()
  {
    e.LoadAbs8U(SoOvAddr());
    e.I32Const(1);
    e.U8(Op::I32ShrU);
  }

  // cr0 = Interpreter::Helper_UpdateCR0(value in local `v`)
  void UpdateCR0(u32 v)
  {
    e.I32Const(0);
    e.LocalGet(v);
    e.U8(Op::I64ExtendI32S);
    e.LocalGet(v);
    e.U8(Op::I32Eqz);
    e.U8(Op::I64ExtendI32U);
    e.I64Const(63);
    e.U8(Op::I64Shl);
    e.U8(Op::I64Or);
    e.I64Const(static_cast<s64>(~(1ULL << PowerPC::CR_EMU_SO_BIT)));
    e.U8(Op::I64And);
    GetSO();
    e.U8(Op::I64ExtendI32U);
    e.I64Const(PowerPC::CR_EMU_SO_BIT);
    e.U8(Op::I64Shl);
    e.U8(Op::I64Or);
    e.EndStoreAbs64(CrAddr(0));
  }

  // cr[field] = s_crTable[(a<b)<<3 | (a>b)<<2 | (a==b)<<1 | SO], with a in m_t0 and b in m_t1.
  void Compare(u32 field, bool is_signed)
  {
    e.I32Const(0);
    e.LocalGet(m_t0);
    e.LocalGet(m_t1);
    e.U8(is_signed ? Op::I32LtS : Op::I32LtU);
    e.I32Const(3);
    e.U8(Op::I32Shl);
    e.LocalGet(m_t0);
    e.LocalGet(m_t1);
    e.U8(is_signed ? Op::I32GtS : Op::I32GtU);
    e.I32Const(2);
    e.U8(Op::I32Shl);
    e.U8(Op::I32Or);
    e.LocalGet(m_t0);
    e.LocalGet(m_t1);
    e.U8(Op::I32Eq);
    e.I32Const(1);
    e.U8(Op::I32Shl);
    e.U8(Op::I32Or);
    GetSO();
    e.U8(Op::I32Or);
    e.I32Const(3);
    e.U8(Op::I32Shl);
    e.MemOp(Op::I64Load, 3, Addr(PowerPC::ConditionRegister::s_crTable.data()));
    e.EndStoreAbs64(CrAddr(field));
  }

  // Pushes CR bit `bi` (0 or 1), following ConditionRegister::GetBit.
  void GetCRBit(u32 bi)
  {
    const u32 field = bi >> 2;
    switch (3 - (bi & 3))
    {
    case PowerPC::CR_LT_BIT:
      e.LoadAbs64(CrAddr(field));
      e.I64Const(PowerPC::CR_EMU_LT_BIT);
      e.U8(Op::I64ShrU);
      e.U8(Op::I32WrapI64);
      e.I32Const(1);
      e.U8(Op::I32And);
      break;
    case PowerPC::CR_GT_BIT:
      e.LoadAbs64(CrAddr(field));
      e.I64Const(0);
      e.U8(Op::I64GtS);
      break;
    case PowerPC::CR_EQ_BIT:
      e.LoadAbs32(CrAddr(field));  // low 32 bits (little endian)
      e.U8(Op::I32Eqz);
      break;
    case PowerPC::CR_SO_BIT:
      e.LoadAbs64(CrAddr(field));
      e.I64Const(PowerPC::CR_EMU_SO_BIT);
      e.U8(Op::I64ShrU);
      e.U8(Op::I32WrapI64);
      e.I32Const(1);
      e.U8(Op::I32And);
      break;
    }
  }

  void StoreConst32(u32 address, u32 value)
  {
    e.I32Const(0);
    e.I32ConstU(value);
    e.EndStoreAbs32(address);
  }

  // Calls a CachedInterpreter callback exactly as ExecuteOneBlock would.
  void GenericCallback(u32 callback, u32 payload, bool may_exit)
  {
    Flush();
    e.I32ConstU(Addr(&m_ppc));
    e.I32ConstU(payload);
    e.I32ConstU(callback);
    e.CallIndirect(TYPE_I32I32_I32);
    Invalidate();
    if (may_exit)
    {
      e.U8(Op::I32Eqz);
      e.If();
      e.Return();
      e.End();
    }
    else
    {
      e.U8(Op::Drop);
    }
  }

  // Effective address (rA|0) + simm into m_t0.
  void EA_D(u32 inst, bool update)
  {
    const u32 ra = RA(inst);
    if (ra != 0 || update)
    {
      GetGpr(ra);
      e.I32Const(SIMM(inst));
      e.U8(Op::I32Add);
    }
    else
    {
      e.I32Const(SIMM(inst));
    }
    e.LocalSet(m_t0);
  }

  // Effective address (rA|0) + rB into m_t0.
  void EA_X(u32 inst)
  {
    const u32 ra = RA(inst);
    if (ra != 0)
    {
      GetGpr(ra);
      GetGpr(RB(inst));
      e.U8(Op::I32Add);
    }
    else
    {
      GetGpr(RB(inst));
    }
    e.LocalSet(m_t0);
  }

  // Pushes (Exceptions & EXCEPTION_DSI) == 0.
  void NoDSI()
  {
    e.LoadAbs32(ExceptionsAddr());
    e.I32Const(EXCEPTION_DSI);
    e.U8(Op::I32And);
    e.U8(Op::I32Eqz);
  }

  // reg = no_dsi ? value_local : reg   (mirrors the interpreter's DSI guard)
  void SetGprIfNoDSI(u32 r, u32 value_local)
  {
    e.LocalGet(value_local);
    GetGpr(r);
    NoDSI();
    e.U8(Op::Select);
    SetGpr(r);
  }

  void Load(u32 inst, u32 helper, int size, bool sign_extend, bool update, bool indexed)
  {
    if (indexed)
      EA_X(inst);
    else
      EA_D(inst, update);
    e.LocalGet(m_t0);
    e.I32ConstU(helper);
    e.CallIndirect(TYPE_I32_I32);
    if (sign_extend && size == 16)
      e.U8(Op::I32Extend16S);
    e.LocalSet(m_t1);
    SetGprIfNoDSI(RD(inst), m_t1);
    if (update)
      SetGprIfNoDSI(RA(inst), m_t0);
  }

  void Store(u32 inst, u32 helper, bool update, bool indexed)
  {
    if (indexed)
      EA_X(inst);
    else
      EA_D(inst, update);
    GetGpr(RD(inst));
    e.LocalGet(m_t0);
    e.I32ConstU(helper);
    e.CallIndirect(TYPE_I32I32_VOID);
    if (update)
      SetGprIfNoDSI(RA(inst), m_t0);
  }

  // Writes m_t2 into rd and updates CR0 if requested.
  void Result(u32 rd, bool rc)
  {
    e.LocalGet(m_t2);
    SetGpr(rd);
    if (rc)
      UpdateCR0(m_t2);
  }

  void SetCA(u32 value_local)
  {
    e.I32Const(0);
    e.LocalGet(value_local);
    e.EndStoreAbs8(CaAddr());
  }

  // Emits the effect of Interpret<write_pc> for one instruction natively.
  // Returns false if the instruction is not supported natively.
  bool Instruction(u32 inst, u32 pc, bool write_pc, bool allow_branches)
  {
    const u32 op = OPCD(inst);
    const u32 rd = RD(inst), ra = RA(inst), rb = RB(inst);

    if (write_pc)
    {
      // Only branches end blocks among the natively supported instructions.
      if (!allow_branches || !(op == 16 || op == 18 || (op == 19 && (SUBOP10(inst) == 16 ||
                                                                     SUBOP10(inst) == 528))))
      {
        return false;
      }
      StoreConst32(PcAddr(), pc);
      StoreConst32(NpcAddr(), pc + 4);
    }

    switch (op)
    {
    case 7:  // mulli
      GetGpr(ra);
      e.I32Const(SIMM(inst));
      e.U8(Op::I32Mul);
      SetGpr(rd);
      return true;
    case 10:  // cmpli
    case 11:  // cmpi
      if ((inst >> 21) & 1)  // L bit: 64-bit compare, not valid on Gekko
        return false;
      GetGpr(ra);
      e.LocalSet(m_t0);
      if (op == 11)
        e.I32Const(SIMM(inst));
      else
        e.I32ConstU(UIMM(inst));
      e.LocalSet(m_t1);
      Compare(CRFD(inst), op == 11);
      return true;
    case 12:  // addic
    case 13:  // addic.
      GetGpr(ra);
      e.LocalTee(m_t0);
      e.I32Const(SIMM(inst));
      e.U8(Op::I32Add);
      e.LocalSet(m_t2);
      // carry = imm > ~a
      e.I32Const(SIMM(inst));
      e.LocalGet(m_t0);
      e.I32Const(-1);
      e.U8(Op::I32Xor);
      e.U8(Op::I32GtU);
      e.LocalSet(m_t1);
      Result(rd, false);
      SetCA(m_t1);
      if (op == 13)
        UpdateCR0(m_t2);
      return true;
    case 14:  // addi
    case 15:  // addis
    {
      const s32 imm = op == 14 ? SIMM(inst) : static_cast<s32>(UIMM(inst) << 16);
      if (ra)
      {
        GetGpr(ra);
        e.I32Const(imm);
        e.U8(Op::I32Add);
      }
      else
      {
        e.I32Const(imm);
      }
      SetGpr(rd);
      return true;
    }
    case 16:  // bcx
    {
      const u32 bo = (inst >> 21) & 31;
      const u32 bi = (inst >> 16) & 31;
      const bool lk = (inst & 1) != 0;
      const bool aa = (inst & 2) != 0;
      u32 target = static_cast<u32>(static_cast<s32>(static_cast<s16>(inst & 0xFFFC)));
      if (!aa)
        target += pc;
      if ((bo & 4) == 0)
      {
        // CTR--
        e.I32Const(0);
        e.LoadAbs32(SprAddr(SPR_CTR));
        e.I32Const(1);
        e.U8(Op::I32Sub);
        e.EndStoreAbs32(SprAddr(SPR_CTR));
      }
      BranchCondition(bo, bi, true);
      e.If();
      if (lk)
        StoreConst32(SprAddr(SPR_LR), pc + 4);
      StoreConst32(NpcAddr(), target);
      e.End();
      return true;
    }
    case 18:  // bx
    {
      u32 target = inst & 0x03FFFFFC;
      if (target & 0x02000000)
        target |= 0xFC000000;
      if ((inst & 2) == 0)
        target += pc;
      if (inst & 1)
        StoreConst32(SprAddr(SPR_LR), pc + 4);
      StoreConst32(NpcAddr(), target);
      return true;
    }
    case 19:
    {
      const u32 sub = SUBOP10(inst);
      if (sub != 16 && sub != 528)
        return false;
      const u32 bo = (inst >> 21) & 31;
      const u32 bi = (inst >> 16) & 31;
      const bool lk = (inst & 1) != 0;
      const bool to_lr = sub == 16;
      if (to_lr && (bo & 4) == 0)
      {
        e.I32Const(0);
        e.LoadAbs32(SprAddr(SPR_CTR));
        e.I32Const(1);
        e.U8(Op::I32Sub);
        e.EndStoreAbs32(SprAddr(SPR_CTR));
      }
      BranchCondition(bo, bi, to_lr);
      e.If();
      // destination must be read before LR is overwritten
      e.I32Const(0);
      e.LoadAbs32(SprAddr(to_lr ? SPR_LR : SPR_CTR));
      e.I32Const(~3);
      e.U8(Op::I32And);
      e.EndStoreAbs32(NpcAddr());
      if (lk)
        StoreConst32(SprAddr(SPR_LR), pc + 4);
      e.End();
      return true;
    }
    case 20:  // rlwimi
    {
      const u32 mask = RotationMask(MB(inst), ME(inst));
      GetGpr(ra);
      e.I32ConstU(~mask);
      e.U8(Op::I32And);
      GetGpr(rd);
      e.I32Const(static_cast<s32>(rb));
      e.U8(Op::I32Rotl);
      e.I32ConstU(mask);
      e.U8(Op::I32And);
      e.U8(Op::I32Or);
      e.LocalSet(m_t2);
      Result(ra, Rc(inst));
      return true;
    }
    case 21:  // rlwinm
    case 23:  // rlwnm
    {
      const u32 mask = RotationMask(MB(inst), ME(inst));
      GetGpr(rd);
      if (op == 21)
      {
        e.I32Const(static_cast<s32>(rb));
      }
      else
      {
        GetGpr(rb);
        e.I32Const(0x1F);
        e.U8(Op::I32And);
      }
      e.U8(Op::I32Rotl);
      e.I32ConstU(mask);
      e.U8(Op::I32And);
      e.LocalSet(m_t2);
      Result(ra, Rc(inst));
      return true;
    }
    case 24:  // ori
    case 25:  // oris
    case 26:  // xori
    case 27:  // xoris
    case 28:  // andi.
    case 29:  // andis.
    {
      const u32 imm = (op & 1) ? (UIMM(inst) << 16) : UIMM(inst);
      GetGpr(rd);
      e.I32ConstU(imm);
      e.U8(op <= 25 ? Op::I32Or : op <= 27 ? Op::I32Xor : Op::I32And);
      e.LocalSet(m_t2);
      Result(ra, op >= 28);
      return true;
    }
    case 31:
      return Instruction31(inst);
    case 32:  // lwz
    case 33:  // lwzu
      Load(inst, FuncIndex(WasmRead32), 32, false, op == 33, false);
      return true;
    case 34:  // lbz
    case 35:  // lbzu
      Load(inst, FuncIndex(WasmRead8), 8, false, op == 35, false);
      return true;
    case 36:  // stw
    case 37:  // stwu
      Store(inst, FuncIndex(WasmWrite32), op == 37, false);
      return true;
    case 38:  // stb
    case 39:  // stbu
      Store(inst, FuncIndex(WasmWrite8), op == 39, false);
      return true;
    case 40:  // lhz
    case 41:  // lhzu
      Load(inst, FuncIndex(WasmRead16), 16, false, op == 41, false);
      return true;
    case 42:  // lha
    case 43:  // lhau
      Load(inst, FuncIndex(WasmRead16), 16, true, op == 43, false);
      return true;
    case 44:  // sth
    case 45:  // sthu
      Store(inst, FuncIndex(WasmWrite16), op == 45, false);
      return true;
    default:
      return false;
    }
  }

  // Pushes the combined counter/condition result of a conditional branch.
  void BranchCondition(u32 bo, u32 bi, bool can_use_ctr)
  {
    const bool check_ctr = can_use_ctr && (bo & 4) == 0;
    const bool check_cond = (bo & 16) == 0;
    if (check_ctr)
    {
      // (CTR != 0) ^ BO[1]
      e.LoadAbs32(SprAddr(SPR_CTR));
      e.I32Const(0);
      e.U8(((bo >> 1) & 1) ? Op::I32Eq : Op::I32Ne);
    }
    if (check_cond)
    {
      GetCRBit(bi);
      if (((bo >> 3) & 1) == 0)
        e.U8(Op::I32Eqz);
    }
    if (check_ctr && check_cond)
      e.U8(Op::I32And);
    if (!check_ctr && !check_cond)
      e.I32Const(1);
  }

  bool Instruction31(u32 inst)
  {
    const u32 rd = RD(inst), ra = RA(inst), rb = RB(inst);
    const bool rc = Rc(inst);
    switch (SUBOP10(inst))
    {
    case 0:   // cmp
    case 32:  // cmpl
      if ((inst >> 21) & 1)
        return false;
      GetGpr(ra);
      e.LocalSet(m_t0);
      GetGpr(rb);
      e.LocalSet(m_t1);
      Compare(CRFD(inst), SUBOP10(inst) == 0);
      return true;
    case 266:  // add
      GetGpr(ra);
      GetGpr(rb);
      e.U8(Op::I32Add);
      e.LocalSet(m_t2);
      Result(rd, rc);
      return true;
    case 40:  // subf
      GetGpr(rb);
      GetGpr(ra);
      e.U8(Op::I32Sub);
      e.LocalSet(m_t2);
      Result(rd, rc);
      return true;
    case 104:  // neg
      e.I32Const(0);
      GetGpr(ra);
      e.U8(Op::I32Sub);
      e.LocalSet(m_t2);
      Result(rd, rc);
      return true;
    case 235:  // mullw
      GetGpr(ra);
      GetGpr(rb);
      e.U8(Op::I32Mul);
      e.LocalSet(m_t2);
      Result(rd, rc);
      return true;
    case 28:   // and
    case 60:   // andc
    case 124:  // nor
    case 284:  // eqv
    case 316:  // xor
    case 412:  // orc
    case 444:  // or
    case 476:  // nand
    {
      const u32 sub = SUBOP10(inst);
      GetGpr(rd);  // rS
      GetGpr(rb);
      if (sub == 60 || sub == 412)
      {
        e.I32Const(-1);
        e.U8(Op::I32Xor);
      }
      e.U8(sub == 28 || sub == 60 || sub == 476 ? Op::I32And :
           sub == 284 || sub == 316             ? Op::I32Xor :
                                                  Op::I32Or);
      if (sub == 124 || sub == 284 || sub == 476)
      {
        e.I32Const(-1);
        e.U8(Op::I32Xor);
      }
      e.LocalSet(m_t2);
      Result(ra, rc);
      return true;
    }
    case 24:   // slw
    case 536:  // srw
    {
      const bool left = SUBOP10(inst) == 24;
      // (amount & 0x20) ? 0 : rS shifted by (amount & 0x1f)
      GetGpr(rd);
      GetGpr(rb);
      e.U8(left ? Op::I32Shl : Op::I32ShrU);  // wasm masks the shift amount to 5 bits
      e.I32Const(0);
      GetGpr(rb);
      e.I32Const(0x20);
      e.U8(Op::I32And);
      e.U8(Op::I32Eqz);
      e.U8(Op::Select);
      e.LocalSet(m_t2);
      Result(ra, rc);
      return true;
    }
    case 824:  // srawi
    {
      const u32 amount = rb;
      GetGpr(rd);
      e.LocalTee(m_t0);
      e.I32Const(static_cast<s32>(amount));
      e.U8(Op::I32ShrS);
      e.LocalSet(m_t2);
      if (amount == 0)
      {
        e.I32Const(0);
      }
      else
      {
        // CA = rS < 0 && (rS << (32 - amount)) != 0
        e.LocalGet(m_t0);
        e.I32Const(0);
        e.U8(Op::I32LtS);
        e.LocalGet(m_t0);
        e.I32Const(static_cast<s32>(32 - amount));
        e.U8(Op::I32Shl);
        e.I32Const(0);
        e.U8(Op::I32Ne);
        e.U8(Op::I32And);
      }
      e.LocalSet(m_t1);
      Result(ra, false);
      SetCA(m_t1);
      if (rc)
        UpdateCR0(m_t2);
      return true;
    }
    case 954:  // extsb
    case 922:  // extsh
      GetGpr(rd);
      e.U8(SUBOP10(inst) == 954 ? Op::I32Extend8S : Op::I32Extend16S);
      e.LocalSet(m_t2);
      Result(ra, rc);
      return true;
    case 26:  // cntlzw
      GetGpr(rd);
      e.U8(Op::I32Clz);
      e.LocalSet(m_t2);
      Result(ra, rc);
      return true;
    case 339:  // mfspr
    case 467:  // mtspr
    {
      const u32 spr_field = (inst >> 11) & 0x3FF;
      const u32 index = ((spr_field & 0x1F) << 5) + ((spr_field >> 5) & 0x1F);
      if (index != SPR_LR && index != SPR_CTR)
        return false;
      if (SUBOP10(inst) == 339)
      {
        e.LoadAbs32(SprAddr(index));
        SetGpr(rd);
      }
      else
      {
        e.I32Const(0);
        GetGpr(rd);
        e.EndStoreAbs32(SprAddr(index));
      }
      return true;
    }
    case 23:  // lwzx
      Load(inst, FuncIndex(WasmRead32), 32, false, false, true);
      return true;
    case 87:  // lbzx
      Load(inst, FuncIndex(WasmRead8), 8, false, false, true);
      return true;
    case 279:  // lhzx
      Load(inst, FuncIndex(WasmRead16), 16, false, false, true);
      return true;
    case 343:  // lhax
      Load(inst, FuncIndex(WasmRead16), 16, true, false, true);
      return true;
    case 151:  // stwx
      Store(inst, FuncIndex(WasmWrite32), false, true);
      return true;
    case 215:  // stbx
      Store(inst, FuncIndex(WasmWrite8), false, true);
      return true;
    case 407:  // sthx
      Store(inst, FuncIndex(WasmWrite16), false, true);
      return true;
    default:
      return false;
    }
  }

  void EndBlock(u32 payload, u32 downcount)
  {
    Flush();
    // pc = npc
    e.I32Const(0);
    e.LoadAbs32(NpcAddr());
    e.EndStoreAbs32(PcAddr());
    // downcount -= cycles
    e.I32Const(0);
    e.LoadAbs32(DowncountAddr());
    e.I32ConstU(downcount);
    e.U8(Op::I32Sub);
    e.EndStoreAbs32(DowncountAddr());
    // The performance monitor only does anything if MMCR0 or MMCR1 is non-zero.
    e.LoadAbs32(SprAddr(SPR_MMCR0));
    e.LoadAbs32(SprAddr(SPR_MMCR1));
    e.U8(Op::I32Or);
    e.If();
    e.I32ConstU(payload);
    e.I32ConstU(FuncIndex(WasmUpdatePerfMon));
    e.CallIndirect(TYPE_I32_VOID);
    e.End();
    e.Return();
  }

  u32 native_count = 0;
  u32 callback_count = 0;

private:
  enum class State : u8
  {
    Uncached,
    Clean,
    Dirty,
  };

  PowerPC::PowerPCState& m_ppc;
  std::array<State, 32> m_state{};
  u32 m_t0, m_t1, m_t2;
};
}  // namespace

// ----------------------------------------------------------------------------------------------

struct CachedInterpreter::WasmEntryOperands
{
  CachedInterpreter& jit;
  u32 hits;
  s32 slot;  // table index of the compiled function, or a negative WasmSlotState
  const u8* end;  // end of this block's callback stream
};

static_assert(sizeof(CachedInterpreter::EndBlockOperands<false>) >= sizeof(PerfMonOperands));

s32 CachedInterpreter::WasmEntry(PowerPC::PowerPCState& ppc_state,
                                 const WasmEntryOperands& operands)
{
  if (operands.slot >= 0) [[likely]]
  {
    reinterpret_cast<void (*)()>(static_cast<uintptr_t>(operands.slot))();
    return 0;
  }

  auto& mutable_operands = const_cast<WasmEntryOperands&>(operands);
  if (operands.slot == WASM_SLOT_NOT_YET && ++mutable_operands.hits >= WASM_HOTNESS_THRESHOLD)
  {
    const u8* begin = reinterpret_cast<const u8*>(&operands) + sizeof(operands);
    if (operands.jit.WasmCompileBlock(mutable_operands, begin, operands.end))
    {
      reinterpret_cast<void (*)()>(static_cast<uintptr_t>(operands.slot))();
      return 0;
    }
    mutable_operands.slot = WASM_SLOT_NEVER;
  }

  // Not compiled (yet): fall through to the regular callback stream.
  return sizeof(AnyCallback) + sizeof(operands);
}

s32 CachedInterpreter::WasmEntry(std::ostream& stream, const WasmEntryOperands& operands)
{
  stream << "WasmEntry(slot=" << operands.slot << ", hits=" << operands.hits << ")\n";
  return sizeof(AnyCallback) + sizeof(operands);
}

CachedInterpreter::WasmEntryOperands* CachedInterpreter::WriteWasmEntry()
{
  u8* const at = GetWritableCodePtr();
  Write(WasmEntry, {*this, 0, m_wasm_enabled ? WASM_SLOT_NOT_YET : WASM_SLOT_NEVER, nullptr});
  if (HasWriteFailed())
    return nullptr;
  return reinterpret_cast<WasmEntryOperands*>(at + sizeof(AnyCallback));
}

void CachedInterpreter::FinishWasmEntry(WasmEntryOperands* entry, const u8* end)
{
  if (entry)
    entry->end = end;
}

bool CachedInterpreter::WasmCompileBlock(WasmEntryOperands& entry, const u8* begin,
                                         const u8* end)
{
  if (!end)
    return false;

  s_mmu = &m_system.GetMMU();
  s_ppc_state = &m_ppc_state;

  const bool allow_branches = !m_system.GetPowerPC().GetBranchWatch().GetRecordingActive();

  BlockCompiler c(m_ppc_state);
  for (const u8* p = begin; p < end;)
  {
    const auto callback = *reinterpret_cast<const AnyCallback*>(p);
    const u8* payload = p + sizeof(AnyCallback);
    const u32 callback_index = FuncIndex(callback);
    const u32 payload_address = Addr(payload);

    if (callback == AnyCallbackCast(Interpret<false>) ||
        callback == AnyCallbackCast(Interpret<true>))
    {
      const bool write_pc = callback == AnyCallbackCast(Interpret<true>);
      const auto& ops = *reinterpret_cast<const InterpretOperands*>(payload);
      if (c.Instruction(ops.inst.hex, ops.current_pc, write_pc, allow_branches))
      {
        ++c.native_count;
      }
      else
      {
        c.GenericCallback(callback_index, payload_address, false);
        ++c.callback_count;
      }
      p = payload + sizeof(InterpretOperands);
    }
    else if (callback == AnyCallbackCast(EndBlock<false>))
    {
      const auto& ops = *reinterpret_cast<const EndBlockOperands<false>*>(payload);
      c.EndBlock(payload_address, ops.downcount);
      p = payload + sizeof(EndBlockOperands<false>);
    }
    else if (callback == AnyCallbackCast(EndBlock<true>))
    {
      c.GenericCallback(callback_index, payload_address, true);
      p = payload + sizeof(EndBlockOperands<true>);
    }
    else if (callback == AnyCallbackCast(InterpretAndCheckExceptions<false>) ||
             callback == AnyCallbackCast(InterpretAndCheckExceptions<true>))
    {
      c.GenericCallback(callback_index, payload_address, true);
      ++c.callback_count;
      p = payload + sizeof(InterpretAndCheckExceptionsOperands);
    }
    else if (callback == AnyCallbackCast(HLEFunction))
    {
      c.GenericCallback(callback_index, payload_address, false);
      p = payload + sizeof(HLEFunctionOperands);
    }
    else if (callback == AnyCallbackCast(WriteBrokenBlockNPC))
    {
      c.GenericCallback(callback_index, payload_address, false);
      p = payload + sizeof(WriteBrokenBlockNPCOperands);
    }
    else if (callback == AnyCallbackCast(CheckFPU) || callback == AnyCallbackCast(CheckBreakpoint))
    {
      c.GenericCallback(callback_index, payload_address, true);
      p = payload + sizeof(CheckHaltOperands);
    }
    else if (callback == AnyCallbackCast(CheckIdle))
    {
      c.GenericCallback(callback_index, payload_address, false);
      p = payload + sizeof(CheckIdleOperands);
    }
    else if (callback == AnyCallbackCast(StartProfiledBlock))
    {
      c.GenericCallback(callback_index, payload_address, false);
      p = payload + sizeof(StartProfiledBlockOperands);
    }
    else
    {
      // Unknown callback (should not happen): stay on the Cached Interpreter path.
      WARN_LOG_FMT(DYNA_REC, "Wasm JIT: unknown callback in block, not compiling");
      return false;
    }
  }
  // Every stream ends in EndBlock (which returns), but be safe.
  c.Flush();

  if (m_wasm_free_slots.empty())
  {
    const u32 first = d2w_jit_grow_table(WASM_TABLE_GROW_STEP);
    for (u32 i = 0; i < WASM_TABLE_GROW_STEP; ++i)
      m_wasm_free_slots.push_back(first + WASM_TABLE_GROW_STEP - 1 - i);
  }
  const u32 slot = m_wasm_free_slots.back();

  const std::vector<u8> module = c.e.BuildModule(d2w_jit_table_size(), d2w_jit_memory_pages());
  if (!d2w_jit_install(module.data(), static_cast<u32>(module.size()), slot))
  {
    m_wasm_enabled = m_wasm_failures++ < 64;  // give up on the JIT if it keeps failing
    return false;
  }
  m_wasm_free_slots.pop_back();
  m_wasm_used_slots.insert(slot);
  entry.slot = static_cast<s32>(slot);
  m_wasm_compiled_blocks++;
  m_wasm_native_instructions += c.native_count;
  m_wasm_fallback_instructions += c.callback_count;
  return true;
}

void CachedInterpreter::WasmOnBlockDestroyed(const JitBlock& block)
{
  if (!block.normalEntry)
    return;
  const auto callback = *reinterpret_cast<const AnyCallback*>(block.normalEntry);
  if (callback != AnyCallbackCast(WasmEntry))
    return;
  auto& entry =
      *reinterpret_cast<WasmEntryOperands*>(block.normalEntry + sizeof(AnyCallback));
  if (entry.slot >= 0)
  {
    const u32 slot = static_cast<u32>(entry.slot);
    d2w_jit_clear_slot(slot);
    m_wasm_used_slots.erase(slot);
    m_wasm_free_slots.push_back(slot);
  }
  entry.slot = WASM_SLOT_NEVER;
}

void CachedInterpreter::WasmClearAll()
{
  for (const u32 slot : m_wasm_used_slots)
  {
    d2w_jit_clear_slot(slot);
    m_wasm_free_slots.push_back(slot);
  }
  m_wasm_used_slots.clear();
}

void CachedInterpreter::WasmLogStats() const
{
  NOTICE_LOG_FMT(DYNA_REC, "Wasm JIT: {} blocks compiled, {} native / {} fallback instructions",
                 m_wasm_compiled_blocks, m_wasm_native_instructions,
                 m_wasm_fallback_instructions);
}

#endif  // __EMSCRIPTEN__
