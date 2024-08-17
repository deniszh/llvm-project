//===--------- Passes/Golang/go_v1_14.h -------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_BOLT_GOLANG_V1_14_H
#define LLVM_TOOLS_LLVM_BOLT_GOLANG_V1_14_H

#include "go_base.h"

namespace llvm {
namespace bolt {

class Pclntab_v1_14_9 : public Pclntab {
// runtime/symtab.go moduledataverify1
#define PclntabFields                                                          \
  F(uint32_t, false, Magic)                                                    \
  F(uint8_t, false, Zero1)                                                     \
  F(uint8_t, false, Zero2)                                                     \
  F(uint8_t, false, Quantum)                                                   \
  F(uint8_t, false, Psize)                                                     \
  F(uint64_t, true, SymtabSize)

  struct PcHeader {
#define F(Type, IsPointerSize, Field) Type Field;
    PclntabFields
#undef F
  } Header;

  void __readHeader(BinaryContext &BC, DataExtractor &DE) override {
    uint64_t Offset = getPclntabHeaderOffset();
#define F(Type, IsPointer, Field)                                              \
  {                                                                            \
    assert(DE.isValidOffset(Offset) && "Invalid offset");                      \
    size_t FSize =                                                             \
        IsPointer ? BC.AsmInfo->getCodePointerSize() : sizeof(Type);           \
    Header.Field = readEndianVal(DE, &Offset, FSize);                          \
  }
    PclntabFields
#undef F
  }

  void __writeHeader(BinaryContext &BC, uint8_t *Pclntab) const override {
#define F(Type, IsPointer, Field)                                              \
  {                                                                            \
    size_t FSize =                                                             \
        IsPointer ? BC.AsmInfo->getCodePointerSize() : sizeof(Type);           \
    writeEndianVal(BC, Header.Field, FSize, &Pclntab);                         \
  }
    PclntabFields
#undef F
  }

  void setNewHeaderOffsets() override {}

  bool checkMagic() const override { return Header.Magic == 0xfffffffb; }

public:
  ~Pclntab_v1_14_9() = default;

  static size_t getPcHeaderSize(unsigned Psize) {
    size_t FuncSize = 0;
#define F(Type, IsPointerSize, Field)                                          \
  if (IsPointerSize)                                                           \
    FuncSize += Psize;                                                         \
  else                                                                         \
    FuncSize += sizeof(Type);
    PclntabFields
#undef F

        return alignTo(FuncSize, Psize);
  }

#undef PclntabFields

  size_t getPcHeaderSize() const override {
    return getPcHeaderSize(Header.Psize);
  }

  void setFunctionsCount(uint64_t Count) override { Header.SymtabSize = Count; }

  uint8_t getQuantum() const override { return Header.Quantum; }

  uint8_t getPsize() const override { return Header.Psize; }

  uint64_t getFunctionsCount() const override { return Header.SymtabSize; }

  uint64_t getNameOffset() const override { return getPclntabHeaderOffset(); }

  uint64_t getFiletabOffset() const override {
    return getPclntabHeaderOffset();
  }

  uint64_t getPctabOffset() const override { return getPclntabHeaderOffset(); }

  uint64_t getPclntabOffset() const override {
    return getPclntabHeaderOffset() + getPcHeaderSize();
  }

  uint64_t getFunctabOffset() const override {
    return getPclntabHeaderOffset();
  }
};

struct GoFunc_v1_14_9 : GoFunc {
  ~GoFunc_v1_14_9() = default;

  // runtime/symtab.go
  enum {
    _PCDATA_RegMapIndex = 0,
    _PCDATA_StackMapIndex = 1,
    _PCDATA_InlTreeIndex = 2,
    _PCDATA_MAX,
    _FUNCDATA_ArgsPointerMaps = 0,
    _FUNCDATA_LocalsPointerMaps = 1,
    _FUNCDATA_RegPointerMaps = 2,
    _FUNCDATA_StackObjects = 3,
    _FUNCDATA_InlTree = 4,
    _FUNCDATA_OpenCodedDeferInfo = 5,
    _FUNCDATA_MAX,
    _ArgsSizeUnknown = -0x80000000
  };

  // runtime/symtab.go
  enum {
    funcID_normal = 0,
    funcID_runtime_main,
    funcID_goexit,
    funcID_jmpdefer,
    funcID_mcall,
    funcID_morestack,
    funcID_mstart,
    funcID_rt0_go,
    funcID_asmcgocall,
    funcID_sigpanic,
    funcID_runfinq,
    funcID_gcBgMarkWorker,
    funcID_systemstack_switch,
    funcID_systemstack,
    funcID_cgocallback_gofunc,
    funcID_gogo,
    funcID_externalthreadhandler,
    funcID_debugCallV1,
    funcID_gopanic,
    funcID_panicwrap,
    funcID_handleAsyncEvent,
    funcID_asyncPreempt,
    funcID_wrapper
  };

#define FuncFields                                                             \
  F(uint64_t, true, Entry)                                                     \
  F(int32_t, false, Name)                                                      \
  F(uint32_t, false, Args)                                                     \
  F(uint32_t, false, Deferreturn)                                              \
  F(int32_t, false, Pcsp)                                                      \
  F(int32_t, false, Pcfile)                                                    \
  F(int32_t, false, Pcln)                                                      \
  F(uint32_t, false, Npcdata)                                                  \
  F(uint8_t, false, FuncID)                                                    \
  F(uint8_t, false, Unused1)                                                   \
  F(uint8_t, false, Unused2)                                                   \
  F(uint8_t, false, Nfuncdata)

  struct _Func {
#define F(Type, IsPointer, Field) Type Field;
    FuncFields
#undef F
  } __GoFunc;

  uint32_t Pcdata[_PCDATA_MAX] = {};
  uint64_t Funcdata[_FUNCDATA_MAX] = {};

  int32_t PcdataMax[_PCDATA_MAX] = {};

  void __read(const BinaryContext &BC, DataExtractor &DE,
              BinarySection *Section, uint64_t *FuncOffset) override {
#define F(Type, IsPointer, Field)                                              \
  {                                                                            \
    assert(DE.isValidOffset(*FuncOffset) && "Invalid offset");                 \
    size_t FSize =                                                             \
        IsPointer ? BC.AsmInfo->getCodePointerSize() : sizeof(Type);           \
    if (IsPointer && Section)                                                  \
      RemoveRelaReloc(BC, Section, *FuncOffset);                               \
    __GoFunc.Field = readEndianVal(DE, FuncOffset, FSize);                     \
  }
    FuncFields
#undef F
  }

  void __write(BinaryFunction *BF, uint8_t **FuncPart, uint8_t *SectionData,
               BinarySection *OutputSection) const override {
    BinaryContext &BC = BF->getBinaryContext();
#define F(Type, IsPointer, Field)                                              \
  {                                                                            \
    if (IsPointer) {                                                           \
      uint64_t Delta = (uint64_t)(*FuncPart - SectionData);                    \
      AddRelaReloc(BC, BF->getSymbol(), OutputSection, Delta, 0);              \
      *FuncPart += BC.AsmInfo->getCodePointerSize();                           \
    } else {                                                                   \
      writeEndianVal(BC, __GoFunc.Field, sizeof(Type), FuncPart);              \
    }                                                                          \
  }
    FuncFields
#undef F
  }

  size_t getSize(BinaryContext &BC) const override {
    size_t FuncSize = 0;
#define F(Type, IsPointerSize, Field)                                          \
  if (IsPointerSize)                                                           \
    FuncSize += BC.AsmInfo->getCodePointerSize();                              \
  else                                                                         \
    FuncSize += sizeof(Type);
    FuncFields
#undef F
        return FuncSize;
  }

#undef FuncFields

  void disableMetadata() override {
    __GoFunc.Pcfile = 0;
    __GoFunc.Pcln = 0;
  }

  int32_t getNameOffset() const override { return __GoFunc.Name; }

  void setNameOffset(int32_t Offset) override { __GoFunc.Name = Offset; }

  uint32_t getDeferreturnOffset() const override {
    return __GoFunc.Deferreturn;
  }

  void setDeferreturnOffset(uint32_t Offset) override {
    __GoFunc.Deferreturn = Offset;
  }

  uint32_t getPcspOffset() const override { return __GoFunc.Pcsp; }

  void setPcspOffset(uint32_t Offset) override { __GoFunc.Pcsp = Offset; }

  uint32_t getNpcdata() const override { return __GoFunc.Npcdata; }

  void fixNpcdata() override {
    for (int I = _PCDATA_MAX - 1; I >= 0; --I) {
      if (Pcdata[I]) {
        __GoFunc.Npcdata = I + 1;
        return;
      }
    }

    __GoFunc.Npcdata = 0;
  }

  bool hasReservedID(std::string Name) const override {
    return __GoFunc.FuncID != funcID_normal &&
           __GoFunc.FuncID != funcID_wrapper;
  }

  uint8_t getNfuncdata() const override { return __GoFunc.Nfuncdata; }

  unsigned getPcdataUnsafePointIndex() const override {
    return _PCDATA_RegMapIndex;
  }

  unsigned getPcdataStackMapIndex() const override {
    return _PCDATA_StackMapIndex;
  }

  unsigned getPcdataInlTreeIndex() const override {
    return _PCDATA_InlTreeIndex;
  }

  unsigned getPcdataMaxIndex() const override { return _PCDATA_MAX; }

  size_t getPcdataSize() const override { return sizeof(Pcdata); }

  uint32_t getPcdata(unsigned Index) const override {
    assert(Index < _PCDATA_MAX && "Invalid index");
    return Pcdata[Index];
  }

  void setPcdata(unsigned Index, uint32_t Value) override {
    assert(Index < _PCDATA_MAX && "Invalid index");
    Pcdata[Index] = Value;
  }

  void setPcdataMaxVal(unsigned Index, int32_t Value) override {
    assert(Index < _PCDATA_MAX && "Invalid index");
    PcdataMax[Index] = Value;
  }

  int32_t getPcdataMax(unsigned Index) const override {
    assert(Index < _PCDATA_MAX && "Invalid index");
    return PcdataMax[Index];
  }

  int getPcdataSafePointVal() const override {
    const int Val = -1;
    return Val;
  }

  int getPcdataUnsafePointVal() const override {
    const int Val = -2;
    return Val;
  }

  unsigned getFuncdataInlTreeIndex() const override {
    return _FUNCDATA_InlTree;
  }

  uint64_t getFuncdata(unsigned Index) const override {
    assert(Index < _FUNCDATA_MAX && "Invalid index");
    return Funcdata[Index];
  }

  void setFuncdata(unsigned Index, uint64_t Value) override {
    assert(Index < _FUNCDATA_MAX && "Invalid index");
    Funcdata[Index] = Value;
  }
};

struct Module_v1_14_9 : Module {
  ~Module_v1_14_9() = default;

  union ModuleStruct {
    struct {
      GoArray pclntable;

      GoArray ftab;

      GoArray filetab;

      uint64_t findfunctab;
      uint64_t minpc, maxpc;
      uint64_t text, etext;
      uint64_t noptrdata, enoptrdata;
      uint64_t data, edata;
      uint64_t bss, ebss;
      uint64_t noptrbss, enoptrbss;
      uint64_t end, gcdata, gcbss;
      uint64_t types, etypes;

      GoArray textsectmap;

      GoArray typelinks;

      GoArray itablinks;

      // Other fields are zeroed/unused in exec
    } m;

    uint64_t a[sizeof(m) / sizeof(uint64_t)];
  } ModuleStruct;

  uint64_t getFieldOffset(BinaryContext &BC, uint64_t *Addr) const override {
    unsigned Psize = BC.AsmInfo->getCodePointerSize();
    return (Addr - ModuleStruct.a) * Psize;
  }

  uint64_t *getModule() override { return ModuleStruct.a; }

  size_t getModuleSize() const override { return sizeof(ModuleStruct.m); }

  void setPclntabSize(uint64_t Size) override {
    // Set pclntable size
    ModuleStruct.m.pclntable.setCount(Size);
  }

  void setFtabSize(uint64_t Count) override {
    // Fix ftab size; the last entry reserved for maxpc
    ModuleStruct.m.ftab.setCount(Count + 1);
  }

  uint64_t getPcHeaderAddr() const override {
    return ModuleStruct.m.pclntable.getAddress();
  }

  const GoArray &getFtab() const override { return ModuleStruct.m.ftab; }

  uint64_t getFindfunctab() const override {
    return ModuleStruct.m.findfunctab;
  }

  uint64_t getTypes() const override { return ModuleStruct.m.types; }

  uint64_t getEtypes() const override { return ModuleStruct.m.etypes; }

  const GoArray &getTextsectmap() const override {
    return ModuleStruct.m.textsectmap;
  }

  const GoArray &getTypelinks() const override {
    return ModuleStruct.m.typelinks;
  }

  int patch(BinaryContext &BC) override {
    BinaryData *Module = getModuleBD(BC);
    if (!Module) {
      errs() << "BOLT-ERROR: Failed to get firstmoduledata symbol!\n";
      return -1;
    }

    BinarySection *Section = &Module->getSection();
    std::vector<BinaryFunction *> BFs = BC.getSortedFunctions();
    unsigned Psize = BC.AsmInfo->getCodePointerSize();

#define getOffset(Field)                                                       \
  Module->getOffset() + getFieldOffset(BC, &ModuleStruct.m.Field);

#define getValue(Field) (ModuleStruct.m.Field)

    // Fix firsmoduledata pointers
    BinaryData *PclntabSym = BC.getBinaryDataAtAddress(getPcHeaderAddr());
    assert(PclntabSym && "PclntabSym absent");
    BinaryData *FindfunctabSym = BC.getBinaryDataAtAddress(getFindfunctab());
    assert(FindfunctabSym && "FindfunctabSym absent");
    BinaryFunction *FirstBF = getFirstBF(BC, BFs);
    assert(FirstBF && "Text BF absent");
    BinaryFunction *LastBF = getLastBF(BC, BFs);
    assert(LastBF && "Text BF absent");

#define FirstmoduleFields                                                      \
  F(pclntable.Address, PclntabSym, 0)                                          \
  F(ftab.Address, PclntabSym, Pclntab_v1_14_9::getPcHeaderSize(Psize))         \
  F(findfunctab, FindfunctabSym, 0)                                            \
  F(minpc, FirstBF, 0)                                                         \
  F(text, FirstBF, 0)                                                          \
  F(maxpc, LastBF, 0)                                                          \
  F(etext, LastBF, 0)

#define F(Field, Symbol, Addend)                                               \
  {                                                                            \
    uint64_t FieldOffset = getOffset(Field);                                   \
    RemoveRelaReloc(BC, Section, FieldOffset);                                 \
    AddRelaReloc(BC, Symbol->getSymbol(), Section, FieldOffset, Addend);       \
  }
    FirstmoduleFields
#undef F
#undef FirstmoduleFields

        // Fix firstmoduledata static fields
        MCSymbol *ZeroSym = BC.registerNameAtAddress("Zero", 0, 0, 0);
#define FirstmoduleFields                                                      \
  F(pclntable.Count[0])                                                        \
  F(pclntable.Count[1])                                                        \
  F(ftab.Count[0])                                                             \
  F(ftab.Count[1])                                                             \
  F(filetab.Count[0])                                                          \
  F(filetab.Count[1])

#define F(Field)                                                               \
  {                                                                            \
    uint64_t FieldOffset = getOffset(Field);                                   \
    uint64_t FieldVal = getValue(Field);                                       \
    Section->addRelocation(FieldOffset, ZeroSym, Relocation::getAbs(Psize),    \
                           FieldVal);                                          \
  }
    FirstmoduleFields
#undef F
#undef FirstmoduleFields

#undef getValue
#undef getOffset
        return 0;
  }
};

class Pclntab_v1_14_12 : public Pclntab_v1_14_9 {
public:
  ~Pclntab_v1_14_12() = default;
};

struct GoFunc_v1_14_12 : GoFunc_v1_14_9 {
  ~GoFunc_v1_14_12() = default;
};

struct Module_v1_14_12 : Module_v1_14_9 {
  ~Module_v1_14_12() = default;
};

} // namespace bolt
} // namespace llvm

#endif
