//===--------- Passes/Golang/go_v1_16_6.h -----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_BOLT_GOLANG_V1_16_5_H
#define LLVM_TOOLS_LLVM_BOLT_GOLANG_V1_16_5_H

#include "go_base.h"

namespace llvm {
namespace bolt {

class Pclntab_v1_16_5 : public Pclntab {
#define PcHeaderFields                                                         \
  F(uint32_t, false, Magic)                                                    \
  F(uint8_t, false, Zero1)                                                     \
  F(uint8_t, false, Zero2)                                                     \
  F(uint8_t, false, MinLC)                                                     \
  F(uint8_t, false, PtrSize)                                                   \
  F(uint64_t, true, Nfuncs)                                                    \
  F(uint64_t, true, Nfiles)                                                    \
  F(uint64_t, true, FuncnameOffset)                                            \
  F(uint64_t, true, CuOffset)                                                  \
  F(uint64_t, true, FiletabOffset)                                             \
  F(uint64_t, true, PctabOffset)                                               \
  F(uint64_t, true, PclnOffset)

  struct PcHeader {
#define F(Type, IsPointerSize, Field) Type Field;
    PcHeaderFields
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
    PcHeaderFields
#undef F
  }

  void __writeHeader(BinaryContext &BC, uint8_t *Pclntab) const override {
#define F(Type, IsPointer, Field)                                              \
  {                                                                            \
    size_t FSize =                                                             \
        IsPointer ? BC.AsmInfo->getCodePointerSize() : sizeof(Type);           \
    writeEndianVal(BC, Header.Field, FSize, &Pclntab);                         \
  }
    PcHeaderFields
#undef F
  }

  void setNewHeaderOffsets() override {
    Header.FuncnameOffset = 0;
    Header.CuOffset = 0;
    Header.FiletabOffset = 0;
    Header.PctabOffset = 0;
    Header.PclnOffset = getPcHeaderSize();
  }

  bool checkMagic() const override { return Header.Magic == 0xfffffffa; }

public:
  ~Pclntab_v1_16_5() = default;

  static size_t getPcHeaderSize(unsigned Psize) {
    size_t FuncSize = 0;
#define F(Type, IsPointerSize, Field)                                          \
  if (IsPointerSize)                                                           \
    FuncSize += Psize;                                                         \
  else                                                                         \
    FuncSize += sizeof(Type);
    PcHeaderFields
#undef F

        return alignTo(FuncSize, Psize);
  }

#undef PcHeaderFields

  size_t getPcHeaderSize() const override {
    return getPcHeaderSize(Header.PtrSize);
  }

  void setFunctionsCount(uint64_t Count) override { Header.Nfuncs = Count; }

  uint8_t getQuantum() const override { return Header.MinLC; }

  uint8_t getPsize() const override { return Header.PtrSize; }

  uint64_t getFunctionsCount() const override { return Header.Nfuncs; }

  uint64_t getNameOffset() const override {
    return getPclntabHeaderOffset() + Header.FuncnameOffset;
  }

  uint64_t getFiletabOffset() const override {
    return getPclntabHeaderOffset() + Header.FiletabOffset;
  }

  uint64_t getPctabOffset() const override {
    return getPclntabHeaderOffset() + Header.PctabOffset;
  }

  uint64_t getPclntabOffset() const override {
    return getPclntabHeaderOffset() + Header.PclnOffset;
  }

  uint64_t getFunctabOffset() const override {
    return getPclntabHeaderOffset() + Header.PclnOffset;
  }
};

struct GoFunc_v1_16_5 : GoFunc {
  ~GoFunc_v1_16_5() = default;

  // runtime/symtab.go
  enum {
    _PCDATA_UnsafePoint = 0,
    _PCDATA_StackMapIndex = 1,
    _PCDATA_InlTreeIndex = 2,
    _PCDATA_MAX,
    _FUNCDATA_ArgsPointerMaps = 0,
    _FUNCDATA_LocalsPointerMaps = 1,
    _FUNCDATA_StackObjects = 2,
    _FUNCDATA_InlTree = 3,
    _FUNCDATA_OpenCodedDeferInfo = 4,
    _FUNCDATA_ArgInfo = 5, // NOTE gc 1.17
    _FUNCDATA_MAX,
    _ArgsSizeUnknown = -0x80000000
  };

  // runtime/symtab.go
  enum {
    _PCDATA_UnsafePointSafe = -1,
    _PCDATA_UnsafePointUnsafe = -2,
    _PCDATA_Restart1 = -3,
    _PCDATA_Restart2 = -4,
    _PCDATA_RestartAtEntry = -5
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
    funcID_cgocallback,
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
  F(int32_t, false, Args)                                                      \
  F(uint32_t, false, Deferreturn)                                              \
  F(uint32_t, false, Pcsp)                                                     \
  F(uint32_t, false, pcfile)                                                   \
  F(uint32_t, false, Pcln)                                                     \
  F(uint32_t, false, Npcdata)                                                  \
  F(uint32_t, false, CuOffset)                                                 \
  F(uint8_t, false, FuncID)                                                    \
  F(uint8_t, false, Flag)                                                      \
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
    __GoFunc.pcfile = 0;
    __GoFunc.Pcln = 0;
    __GoFunc.CuOffset = 0;
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
    for (int i = _PCDATA_MAX - 1; i >= 0; --i) {
      if (Pcdata[i]) {
        __GoFunc.Npcdata = i + 1;
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
    return _PCDATA_UnsafePoint;
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
    return PcdataMax[Index];
  }

  int getPcdataSafePointVal() const override { return _PCDATA_UnsafePointSafe; }

  int getPcdataUnsafePointVal() const override {
    return _PCDATA_UnsafePointUnsafe;
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

struct Module_v1_16_5 : Module {
  ~Module_v1_16_5() = default;

  union ModuleStruct {
    struct {
      uint64_t pcHeader;

      // Function names part
      GoArray funcnametab;

      // Compilation Unit indexes part
      GoArray cutab;

      // Source file names part
      GoArray filetab;

      // Functions pc-relative part
      GoArray pctab;

      // Function - ftab offset table part
      GoArray pclntable;

      // Functions table part
      GoArray ftab;

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
    // Set funcnametab size
    ModuleStruct.m.funcnametab.setCount(Size);

    // Set pctab size
    ModuleStruct.m.pctab.setCount(Size);

    // Set pclntable size
    ModuleStruct.m.pclntable.setCount(Size);
  }

  void setFtabSize(uint64_t Count) override {
    // Fix ftab size; the last entry reserved for maxpc
    ModuleStruct.m.ftab.setCount(Count + 1);
  }

  uint64_t getPcHeaderAddr() const override { return ModuleStruct.m.pcHeader; }

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
  F(pcHeader, PclntabSym, 0)                                                   \
  F(funcnametab.Address, PclntabSym, 0)                                        \
  F(pctab.Address, PclntabSym, 0)                                              \
  F(pclntable.Address, PclntabSym, 0)                                          \
  F(ftab.Address, PclntabSym, Pclntab_v1_16_5::getPcHeaderSize(Psize))         \
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
  F(funcnametab.Count[0])                                                      \
  F(funcnametab.Count[1])                                                      \
  F(pctab.Count[0])                                                            \
  F(pctab.Count[1])                                                            \
  F(pclntable.Count[0])                                                        \
  F(pclntable.Count[1])                                                        \
  F(ftab.Count[0])                                                             \
  F(ftab.Count[1])

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

} // namespace bolt
} // namespace llvm

#endif
