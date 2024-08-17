//===--------- Passes/Golang.h ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Golang binaries support passes.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_BOLT_PASSES_GOLANG_H
#define LLVM_TOOLS_LLVM_BOLT_PASSES_GOLANG_H

#include "BinaryPasses.h"
#include "bolt/Utils/CommandLineOpts.h"

#include "Golang/go_v1_14.h"
#include "Golang/go_v1_16_5.h"
#include "Golang/go_v1_17.h"

namespace llvm {
namespace bolt {

class GolangPass : public BinaryFunctionPass {
protected:
  uint64_t RuntimeText = 0;
  std::unique_ptr<struct Module> FirstModule;
  std::unique_ptr<class Pclntab> Pclntab;

public:
  /// Golang version strings
  const char *GolangStringVer[opts::GV_LAST] = {
      "none",     "auto",     "go1.14.9", "go1.14.12",
      "go1.16.5", "go1.17.2", "go1.17.5", "go1.17.8",
  };

  explicit GolangPass(BinaryContext &BC) : BinaryFunctionPass(false) {
    if (checkGoVersion(BC) < 0) {
      errs() << "BOLT-ERROR: Failed to check golang version!\n";
      exit(1);
    }

    if (getSymbols(BC) < 0) {
      errs() << "BOLT-ERROR: Failed to get golang-specific symbols!\n";
      exit(1);
    }

    FirstModule = createGoModule();
    if (!FirstModule || FirstModule->read(BC) < 0) {
      errs() << "BOLT-ERROR: Failed to read firstmodule!\n";
      exit(1);
    }

    Pclntab = createGoPclntab();
    if (!Pclntab || Pclntab->readHeader(BC, getPcHeaderAddr()) < 0) {
      errs() << "BOLT-ERROR: Failed to read pclntab!\n";
      exit(1);
    }

    // NOTE last entry is etext
    if (Pclntab->getFunctionsCount() != FirstModule->getFtab().getCount() - 1) {
      errs() << "BOLT-ERROR: Wrong symtab size!\n";
      exit(1);
    }
  }

  static const char *getFirstBFName(void) {
    const char *const Name = "runtime.text";
    return Name;
  }

  static const char *getLastBFName(void) {
    const char *const Name = "runtime.etext";
    return Name;
  }

  static uint32_t getUndAarch64(void) { return 0xbea71700; }

  uint64_t getPcHeaderAddr() const { return FirstModule->getPcHeaderAddr(); }

  uint8_t getPsize() const { return Pclntab->getPsize(); }

  const class Pclntab *getPclntab() const {
    return const_cast<class Pclntab *>(Pclntab.get());
  }

  const char *getName() const override { return "golang"; }

  /// Pass entry point
  void runOnFunctions(BinaryContext &BC) override;
  int checkGoVersion(BinaryContext &BC);
  int getSymbols(BinaryContext &BC);
  int textsectmapPass(BinaryContext &BC);
  int typePass(BinaryContext &BC, uint64_t TypeAddr);
  int typelinksPass(BinaryContext &BC);
  int pcspPass(BinaryFunction *BF, uint8_t **SectionData, const uint32_t Index,
               uint8_t Quantum, bool ForcePreserve);
  uint32_t deferreturnPass(BinaryContext &BC, BinaryFunction *BF);
  int getNextMCinstVal(FunctionLayout::block_iterator BBIt, uint64_t I,
                       const uint32_t Index, int32_t &Val,
                       uint64_t *NextOffset);
  int writeVarintPass(BinaryFunction *BF, uint8_t **DataFuncOffset,
                      const uint32_t Index, const uint8_t Quantum);
  int unsafePointPass(BinaryFunction *BF, GoFunc *GoFunc);
  int pclntabPass(BinaryContext &BC);
  int findFuncTabPass(BinaryContext &BC);
};

class GolangPostPass : public GolangPass {
public:
  explicit GolangPostPass(BinaryContext &BC) : GolangPass(BC) {}

  /// Pass entry point
  void runOnFunctions(BinaryContext &BC) override;
  void skipPleaseUseCallersFramesPass(BinaryContext &BC);
  void instrumentExitCall(BinaryContext &BC);
  uint32_t pcdataPass(BinaryFunction *BF, GoFunc *GoFunc, const uint32_t Index,
                      const unsigned AllocId);
  int pclntabPass(BinaryContext &BC);
};

class GolangPrePass : public GolangPass {
public:
  explicit GolangPrePass(BinaryContext &BC) : GolangPass(BC) {}

  /// Pass entry point
  void runOnFunctions(BinaryContext &BC) override;
  void goPassInit(BinaryContext &BC);
  void nopPass(BinaryContext &BC);
  int pclntabPass(BinaryContext &BC);
  void deferreturnPass(BinaryFunction &BF, const uint64_t DeferOffset,
                       const unsigned AllocId, const MCCodeEmitter *Emitter);
};

} // namespace bolt
} // namespace llvm

#endif
