//===--------- Passes/Golang/go_base.h --------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_BOLT_GOLANG_BASE_H
#define LLVM_TOOLS_LLVM_BOLT_GOLANG_BASE_H

#include "go_common.h"

namespace llvm {
namespace bolt {

// runtime/symtab.go
struct Functab {
  uint64_t Address; // Pointer size
  uint64_t Offset;  // Pointer size
};

// runtime/symtab.go
struct InlinedCall {
  int16_t Parent;
  uint8_t FuncID;
  uint8_t Unused;
  uint32_t File;
  uint32_t Line;
  uint32_t Func;
  uint32_t ParentPc;
};

class GoArray {
public:
  uint64_t Address;
  uint64_t Count[2];

  uint64_t getAddress() const { return Address; }

  void setAddress(uint64_t Addr) { Address = Addr; }

  uint64_t getCount() const { return Count[0]; }

  void setCount(uint64_t C) {
    Count[0] = C;
    Count[1] = C;
  }
};

// moduledata struct
// runtime/symtab.go
// NOTE: Every field size is target-machines pointer size
// NOTE: For some reason array[] fields count is repeated in struct
struct Module {
  virtual ~Module() = 0;

  BinaryData *getModuleBD(BinaryContext &BC) {
    BinaryData *Module = BC.getFirstBinaryDataByName("local.moduledata");
    if (!Module)
      Module = BC.getFirstBinaryDataByName("runtime.firstmoduledata");

    return Module;
  }

  int read(const BinaryContext &BC);
  virtual uint64_t getFieldOffset(BinaryContext &BC, uint64_t *Addr) const = 0;
  virtual int patch(BinaryContext &BC) = 0;
  virtual uint64_t *getModule() = 0;
  virtual size_t getModuleSize() const = 0;
  virtual void setPclntabSize(uint64_t Size) = 0;
  virtual void setFtabSize(uint64_t Count) = 0;
  virtual uint64_t getPcHeaderAddr() const = 0;
  virtual const GoArray &getFtab() const = 0;
  virtual uint64_t getFindfunctab() const = 0;
  virtual uint64_t getTypes() const = 0;
  virtual uint64_t getEtypes() const = 0;
  virtual const GoArray &getTextsectmap() const = 0;
  virtual const GoArray &getTypelinks() const = 0;
};

std::unique_ptr<struct Module> createGoModule();

// runtime/symtab.go
class Pclntab {
  uint64_t PclntabHeaderOffset = 0;

  virtual void __readHeader(BinaryContext &BC, DataExtractor &DE) = 0;
  virtual void __writeHeader(BinaryContext &BC, uint8_t *Pclntab) const = 0;
  virtual bool checkMagic() const = 0;
  virtual void setNewHeaderOffsets() = 0;

protected:
  void setPclntabHeaderOffset(uint64_t Off) { PclntabHeaderOffset = Off; }

  uint64_t getPclntabHeaderOffset() const { return PclntabHeaderOffset; }

public:
  virtual ~Pclntab() = 0;
  int readHeader(BinaryContext &BC, const uint64_t PclntabHeaderAddr);
  int writeHeader(BinaryContext &BC, uint8_t *Pclntab);
  virtual size_t getPcHeaderSize() const = 0;
  virtual void setFunctionsCount(uint64_t Count) = 0;
  virtual uint8_t getQuantum() const = 0;
  virtual uint8_t getPsize() const = 0;
  virtual uint64_t getFunctionsCount() const = 0;
  virtual uint64_t getNameOffset() const = 0;
  virtual uint64_t getFiletabOffset() const = 0;
  virtual uint64_t getPctabOffset() const = 0;
  virtual uint64_t getPclntabOffset() const = 0;
  virtual uint64_t getFunctabOffset() const = 0;
};

std::unique_ptr<class Pclntab> createGoPclntab();

// runtime/runtime2.go
struct GoFunc {
  virtual ~GoFunc() = 0;

  uint64_t PcdataOffset = 0;
  uint64_t FuncdataOffset = 0;

  virtual void __read(const BinaryContext &BC, DataExtractor &DE,
                      BinarySection *Section, uint64_t *FuncOffset) = 0;

  virtual void __write(BinaryFunction *BF, uint8_t **FuncPart, uint8_t *Section,
                       BinarySection *OutputSection) const = 0;

  virtual size_t getSize(BinaryContext &BC) const = 0;

  int read(const BinaryContext &BC, DataExtractor &DE, BinarySection *Section,
           uint64_t *FuncOffset) {
    __read(BC, DE, Section, FuncOffset);

    // Read pcdata
    PcdataOffset = *FuncOffset;
    for (uint32_t I = 0; I < getNpcdata(); ++I)
      setPcdata(I, (uint32_t)readEndianVal(DE, FuncOffset, sizeof(uint32_t)));

    // Read funcdata
    *FuncOffset = alignTo(*FuncOffset, BC.AsmInfo->getCodePointerSize());
    FuncdataOffset = *FuncOffset;
    for (uint32_t I = 0; I < getNfuncdata(); ++I)
      setFuncdata(
          I, readEndianVal(DE, FuncOffset, BC.AsmInfo->getCodePointerSize()));

    return 0;
  }

  int read(const BinaryFunction &BF) {
    if (!BF.isGolang())
      return -1;

    const BinaryContext &BC = BF.getBinaryContext();
    std::unique_ptr<struct Module> FirstModule = createGoModule();
    FirstModule->read(BC);

    const uint64_t PclntabAddr = FirstModule->getPcHeaderAddr();
    if (!PclntabAddr) {
      errs() << "BOLT-ERROR: Pclntab address is zero!\n";
      return -1;
    }

    const BinaryData *PclntabSym = BC.getBinaryDataAtAddress(PclntabAddr);
    if (!PclntabSym) {
      errs() << "BOLT-ERROR: Failed to get pclntab symbol!\n";
      return -1;
    }

    const BinarySection *Section = &PclntabSym->getSection();
    DataExtractor DE =
        DataExtractor(Section->getContents(), BC.AsmInfo->isLittleEndian(),
                      BC.AsmInfo->getCodePointerSize());
    uint64_t FuncOffset = BF.getGolangFunctabOffset();
    return read(BC, DE, nullptr, &FuncOffset);
  }

  int write(BinaryFunction *BF, uint8_t **FuncPart, uint8_t *SectionData,
            BinarySection *OutputSection) {
    BinaryContext &BC = BF->getBinaryContext();
    __write(BF, FuncPart, SectionData, OutputSection);

    // Write pcdata
    for (uint32_t I = 0; I < getNpcdata(); ++I)
      writeEndianVal(BC, getPcdata(I), sizeof(uint32_t), FuncPart);

    // Write funcdata
    *FuncPart = SectionData + alignTo(*FuncPart - SectionData,
                                      BC.AsmInfo->getCodePointerSize());
    for (uint32_t I = 0; I < getNfuncdata(); ++I) {
      uint64_t Val = getFuncdata(I);
      if (Val) {
        uint64_t Delta = (uint64_t)(*FuncPart - SectionData);
        AddRelaReloc(BC, nullptr, OutputSection, Delta, Val);
        *FuncPart += BC.AsmInfo->getCodePointerSize();
      } else {
        writeEndianPointer(BC, 0, FuncPart);
      }
    }
    return 0;
  }

  virtual void disableMetadata() = 0;
  virtual int32_t getNameOffset() const = 0;
  virtual void setNameOffset(int32_t Offset) = 0;
  virtual uint32_t getDeferreturnOffset() const = 0;
  virtual void setDeferreturnOffset(uint32_t Offset) = 0;
  virtual uint32_t getPcspOffset() const = 0;
  virtual void setPcspOffset(uint32_t Offset) = 0;
  virtual uint32_t getNpcdata() const = 0;
  virtual void fixNpcdata() = 0;
  virtual bool hasReservedID(std::string Name) const = 0;
  virtual uint8_t getNfuncdata() const = 0;

  // runtime/symtab.go
  virtual unsigned getPcdataUnsafePointIndex() const = 0;
  virtual unsigned getPcdataStackMapIndex() const = 0;
  virtual unsigned getPcdataInlTreeIndex() const = 0;
  virtual unsigned getPcdataMaxIndex() const = 0;
  virtual size_t getPcdataSize() const = 0;
  virtual uint32_t getPcdata(unsigned Index) const = 0;
  virtual void setPcdata(unsigned Index, uint32_t Value) = 0;
  virtual void setPcdataMaxVal(unsigned Index, int32_t Value) = 0;
  virtual int32_t getPcdataMax(unsigned Index) const = 0;
  virtual int getPcdataSafePointVal() const = 0;
  virtual int getPcdataUnsafePointVal() const = 0;
  virtual unsigned getFuncdataInlTreeIndex() const = 0;
  virtual uint64_t getFuncdata(unsigned Index) const = 0;
  virtual void setFuncdata(unsigned Index, uint64_t Value) = 0;

  uint32_t getPcdataOffset() const { return PcdataOffset; }

  uint32_t getFuncdataOffset() const { return FuncdataOffset; }

  // Fake index used to store pcsp values
  unsigned getPcspIndex() const { return getPcdataMaxIndex() + 1; }
};

std::unique_ptr<struct GoFunc> createGoFunc();

} // namespace bolt
} // namespace llvm

#endif
