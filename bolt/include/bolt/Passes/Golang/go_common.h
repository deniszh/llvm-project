//===--------- Passes/Golang/go_common.h ------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_BOLT_GOLANG_COMMON_H
#define LLVM_TOOLS_LLVM_BOLT_GOLANG_COMMON_H

namespace llvm {
namespace bolt {

BinaryFunction *getBF(BinaryContext &BC, std::vector<BinaryFunction *> &BFs,
                      const char *Name);

BinaryFunction *getFirstBF(BinaryContext &BC,
                           std::vector<BinaryFunction *> &BFs);

BinaryFunction *getLastBF(BinaryContext &BC,
                          std::vector<BinaryFunction *> &BFs);

void writeEndianVal(BinaryContext &BC, uint64_t Val, uint16_t Size,
                    uint8_t **Res);

void writeEndianPointer(BinaryContext &BC, uint64_t Val, uint8_t **Res);

inline uint64_t readEndianVal(DataExtractor &DE, uint64_t *Offset,
                              uint16_t Size) {
  assert(DE.isValidOffset(*Offset) && "Invalid offset");
  switch (Size) {
  case 8:
    return DE.getU64(Offset);
  case 4:
    return DE.getU32(Offset);
  case 2:
    return DE.getU16(Offset);
  case 1:
    return DE.getU8(Offset);
  default:
    __builtin_unreachable();
  }
}

inline uint32_t readVarint(const uint8_t *Data, uint64_t *Offset) {
  uint32_t res = 0, shift = 0;
  uint8_t val;

  while (1) {
    val = Data[(*Offset)++];
    res |= ((uint32_t)(val & 0x7F)) << (shift & 31);
    if ((val & 0x80) == 0)
      break;

    shift += 7;
  }

  return res;
}

inline int32_t readVarintPair(const uint8_t *Data, uint64_t *Offset,
                              int32_t &ValSum, uint64_t &OffsetSum,
                              const uint8_t Quantum) {
  uint32_t Val = readVarint(Data, Offset);
  ValSum += (-(Val & 1) ^ (Val >> 1));
  OffsetSum += readVarint(Data, Offset) * Quantum;
  return (int32_t)Val;
}

inline int32_t readVarintPair(DataExtractor &DE, uint64_t *Offset,
                              int32_t &ValSum, uint64_t &OffsetSum,
                              const uint8_t Quantum) {
  assert(DE.isValidOffset(*Offset));
  const uint8_t *Data = (const uint8_t *)DE.getData().data();
  return readVarintPair(Data, Offset, ValSum, OffsetSum, Quantum);
}

inline void AddRelaReloc(BinaryContext &BC, MCSymbol *Symbol,
                         BinarySection *Section, uint64_t Offset,
                         uint64_t Addend) {
  Section->addRelocation(Offset, Symbol,
                         Relocation::getAbs(BC.AsmInfo->getCodePointerSize()),
                         Addend);

  if (!BC.HasFixedLoadAddress)
    Section->addDynamicRelocation(Offset, Symbol, Relocation::getRelative(),
                                  Addend);
}

inline void RemoveRelaReloc(const BinaryContext &BC, BinarySection *Section,
                            uint64_t Offset) {
  Section->removeRelocationAt(Offset);
  Section->removeDynamicRelocationAt(Offset);
}

inline std::string getVarintName(uint32_t Index, bool IsNext = false) {
  const char *const Varint = "VARINT";
  const char *const VarintNext = "VARINT_NEXT";

  std::string Name = IsNext ? VarintNext : Varint;
  Name += std::to_string(Index);
  return Name;
}

inline void addVarintAnnotation(BinaryContext &BC, MCInst &II, uint32_t Index,
                                int32_t Value, bool IsNext,
                                unsigned AllocId = 0) {
  BC.MIB->addAnnotation(II, getVarintName(Index, IsNext), Value, AllocId);
}

inline bool hasVarintAnnotation(BinaryContext &BC, MCInst &II, uint32_t Index,
                                bool IsNext = false) {
  return BC.MIB->hasAnnotation(II, getVarintName(Index, IsNext));
}

inline int32_t getVarintAnnotation(BinaryContext &BC, MCInst &II,
                                   uint32_t Index, bool IsNext = false) {
  return BC.MIB->getAnnotationAs<int32_t>(II, getVarintName(Index, IsNext));
}

inline std::string getFuncdataName(uint32_t Findex, uint32_t Size) {
  return "FUNCDATA" + std::to_string(Findex) + std::to_string(Size);
}

inline std::string getFuncdataSizeName(uint32_t Findex) {
  return "FUNCDATA_SIZE" + std::to_string(Findex);
}

inline void addFuncdataAnnotation(BinaryContext &BC, MCInst &II,
                                  uint32_t Findex, int32_t Value,
                                  unsigned AllocId = 0) {
  auto &Size = BC.MIB->getOrCreateAnnotationAs<uint32_t>(
      II, getFuncdataSizeName(Findex), AllocId);
  BC.MIB->addAnnotation(II, getFuncdataName(Findex, Size++), Value, AllocId);
}

inline bool hasFuncdataAnnotation(BinaryContext &BC, MCInst &II,
                                  uint32_t Findex) {
  return BC.MIB->hasAnnotation(II, getFuncdataSizeName(Findex));
}

inline uint32_t getFuncdataSizeAnnotation(BinaryContext &BC, MCInst &II,
                                          uint32_t Findex) {
  return BC.MIB->getAnnotationAs<uint32_t>(II, getFuncdataSizeName(Findex));
}

inline int32_t getFuncdataAnnotation(BinaryContext &BC, MCInst &II,
                                     uint32_t Findex, uint32_t Index) {
  return BC.MIB->getAnnotationAs<int32_t>(II, getFuncdataName(Findex, Index));
}

} // namespace bolt
} // namespace llvm

#endif
