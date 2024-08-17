//===--------- Passes/Golang-preprocess.h----------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#include "bolt/Core/ParallelUtilities.h"
#include "bolt/Passes/Golang.h"

#define DEBUG_TYPE "bolt-golang-preprocess"

using namespace llvm;
using namespace bolt;

namespace opts {
extern cl::opt<bool> GolangPcspPreserve;
} // end namespace opts

namespace llvm {
namespace bolt {

static void inlTreePass(BinaryFunction *Function, GoFunc *GoFunc,
                        const unsigned AllocId, const MCCodeEmitter *Emitter) {
  BinaryContext &BC = Function->getBinaryContext();
  const unsigned PcdataIndex = GoFunc->getPcdataInlTreeIndex();
  int32_t MaxVal = GoFunc->getPcdataMax(PcdataIndex);
  if (MaxVal < 0)
    return;

  const unsigned Index = GoFunc->getFuncdataInlTreeIndex();
  uint64_t FuncdataAddr = GoFunc->getFuncdata(Index);
  if (!FuncdataAddr)
    return;

  struct InlinedCall InlinedCall;
  ErrorOr<BinarySection &> FuncdataSection =
      BC.getSectionForAddress(FuncdataAddr);
  if (!FuncdataSection)
    return;

  DataExtractor DE = DataExtractor(FuncdataSection->getContents(),
                                   BC.AsmInfo->isLittleEndian(),
                                   BC.AsmInfo->getCodePointerSize());

  for (uint32_t I = 0; I < (uint32_t)MaxVal + 1; ++I) {
    uint64_t Offset = (uint64_t)(FuncdataAddr - FuncdataSection->getAddress());
    Offset += I * sizeof(InlinedCall);
    assert(DE.isValidOffset(Offset) && "Invalid offset");
    InlinedCall.Parent = (int16_t)DE.getU16(&Offset);
    InlinedCall.FuncID = DE.getU8(&Offset);
    InlinedCall.Unused = DE.getU8(&Offset);
    InlinedCall.File = (int32_t)DE.getU32(&Offset);
    InlinedCall.Line = (int32_t)DE.getU32(&Offset);
    InlinedCall.Func = (int32_t)DE.getU32(&Offset);
    InlinedCall.ParentPc = (int32_t)DE.getU32(&Offset);

    for (BinaryBasicBlock *BB : Function->getLayout().blocks()) {
      if (InlinedCall.ParentPc >= BB->getEndOffset())
        continue;

      uint32_t Offset = BB->getOffset();
      for (MCInst &II : *BB) {
        if (BC.MIB->hasAnnotation(II, "Offset")) {
          constexpr size_t InvalidOffset = std::numeric_limits<uint32_t>::max();
          Offset = BC.MIB->getAnnotationWithDefault<uint32_t>(II, "Offset",
                                                              InvalidOffset);
          assert(Offset != InvalidOffset && "Invalid offset");
        }

        if (Offset < InlinedCall.ParentPc) {
          Offset += BC.computeInstructionSize(II, Emitter);
          continue;
        }

        assert(Offset == InlinedCall.ParentPc && "Offset overflow");

        // NOTE Annotatations must not be created with in concurent threads
        static std::atomic_flag Lock = ATOMIC_FLAG_INIT;
        while (Lock.test_and_set(std::memory_order_acquire))
          ;
        addFuncdataAnnotation(BC, II, Index, I, AllocId);
        Lock.clear(std::memory_order_release);
        // To be able to restore right inline unwinding we will lock the
        // instruction
        bool &Locked = BC.MIB->getOrCreateAnnotationAs<bool>(II, "Locked");
        Locked = true;
        break;
      }

      break;
    }
  }
}

static uint32_t readVarintPass(BinaryFunction *Function, GoFunc *GoFunc,
                               DataExtractor &DE, uint64_t *MapOffset,
                               const uint32_t Index, const uint8_t Quantum,
                               const unsigned AllocId,
                               const MCCodeEmitter *Emitter) {
  BinaryContext &BC = Function->getBinaryContext();
  int32_t ValSum = -1, MaxVal = -1;
  uint64_t OldOffsetSum, OffsetSum = 0;
  bool IsFirst = true;
  MCInst *PrevII = nullptr;

  do {
    OldOffsetSum = OffsetSum;
    int32_t Val = readVarintPair(DE, MapOffset, ValSum, OffsetSum, Quantum);
    if (!Val && !IsFirst) {
      if (Index == GoFunc->getPcdataStackMapIndex())
        addVarintAnnotation(BC, *PrevII, Index, ValSum, /*IsNext*/ true,
                            AllocId);
      break;
    }

    if (ValSum > MaxVal)
      MaxVal = ValSum;

    for (BinaryBasicBlock *BB : Function->getLayout().blocks()) {
      if (OldOffsetSum >= BB->getEndOffset())
        continue;

      uint32_t Offset = BB->getOffset();
      if (Offset > OffsetSum)
        break;

      for (MCInst &II : *BB) {
        if (BC.MIB->hasAnnotation(II, "Offset")) {
          constexpr size_t InvalidOffset = std::numeric_limits<uint32_t>::max();
          Offset = BC.MIB->getAnnotationWithDefault<uint32_t>(II, "Offset",
                                                              InvalidOffset);
          assert(Offset != InvalidOffset && "Invalid offset");
        }

        if (Offset < OldOffsetSum) {
          Offset += BC.computeInstructionSize(II, Emitter);
          continue;
        } else if (Offset == OffsetSum) {
          break;
        }

        addVarintAnnotation(BC, II, Index, ValSum, /*IsNext*/ false, AllocId);
        if (Index == GoFunc->getPcdataStackMapIndex() && PrevII)
          addVarintAnnotation(BC, *PrevII, Index, ValSum, /*IsNext*/ true,
                              AllocId);

        PrevII = &II;
        assert(Offset < OffsetSum && "Offset overflow");
        Offset += BC.computeInstructionSize(II, Emitter);
      }
    }

    IsFirst = false;
  } while (1);

  return MaxVal;
}

void GolangPrePass::deferreturnPass(BinaryFunction &BF,
                                    const uint64_t DeferOffset,
                                    const unsigned AllocId,
                                    const MCCodeEmitter *Emitter) {
  BinaryContext &BC = BF.getBinaryContext();
  uint64_t Offset = 0;
  for (BinaryBasicBlock *BB : BF.getLayout().blocks()) {
    for (MCInst &II : *BB) {
      if (BC.MIB->hasAnnotation(II, "Offset")) {
        constexpr auto InvalidOffset = std::numeric_limits<uint32_t>::max();
        Offset = BC.MIB->getAnnotationWithDefault<uint32_t>(II, "Offset",
                                                            InvalidOffset);
        assert(Offset != InvalidOffset);
      }

      if (Offset < DeferOffset) {
        Offset += BC.computeInstructionSize(II, Emitter);
        continue;
      }

      if (Offset != DeferOffset)
        break;

      assert(BC.MIB->isCall(II));
      BC.MIB->addAnnotation(II, "IsDefer", true, AllocId);
      return;
    }
  }

  outs() << "Deferreturn call was not found for " << BF << "\n";
  exit(1);
}

int GolangPrePass::pclntabPass(BinaryContext &BC) {
  const uint64_t PclntabAddr = getPcHeaderAddr();
  if (!PclntabAddr) {
    errs() << "BOLT-ERROR: Pclntab address is zero!\n";
    return -1;
  }

  BinaryData *PclntabSym = BC.getBinaryDataAtAddress(PclntabAddr);
  if (!PclntabSym) {
    errs() << "BOLT-ERROR: Failed to get pclntab symbol!\n";
    return -1;
  }

  BinarySection *Section = &PclntabSym->getSection();
  const class Pclntab *Pclntab = getPclntab();
  uint64_t Offset = Pclntab->getPclntabOffset();
  DataExtractor DE = DataExtractor(Section->getContents(),
                                   BC.AsmInfo->isLittleEndian(), getPsize());
  for (uint64_t F = 0; F < Pclntab->getFunctionsCount(); ++F) {
    assert(DE.isValidOffset(Offset) && "Invalid offset");
    struct Functab Functab;
    RemoveRelaReloc(BC, Section, Offset);
    Functab.Address = DE.getAddress(&Offset);
    Functab.Offset = DE.getAddress(&Offset);

    BinaryFunction *Function = BC.getBinaryFunctionAtAddress(Functab.Address);
    if (!Function) {
      outs() << "Failed to find function by address "
             << Twine::utohexstr(Functab.Address) << "\n";
      return -1;
    }

    Function->setGolangFunctabOffset(Pclntab->getFunctabOffset() +
                                     Functab.Offset);
  }

  // Remove maxpc relocation (last pclntab entry)
  RemoveRelaReloc(BC, Section, Offset);

  ParallelUtilities::WorkFuncWithAllocTy WorkFun =
      [&](BinaryFunction &Function, MCPlusBuilder::AllocatorIdTy AllocId) {
        if (Function.getLayout().block_begin() ==
            Function.getLayout().block_end())
          return;

        BinaryContext::IndependentCodeEmitter Emitter;
        if (!opts::NoThreads) {
          Emitter =
              Function.getBinaryContext().createIndependentMCCodeEmitter();
        }

        uint64_t FuncOffset = Function.getGolangFunctabOffset();
        std::unique_ptr<struct GoFunc> GoFunc = createGoFunc();
        GoFunc->read(BC, DE, Section, &FuncOffset);
        if (GoFunc->hasReservedID(Function.getDemangledName())) {
          // The functions with reserved ID are special functions
          // mostly written on asm that are dangerous to change
          Function.setSimple(false);
        }

        // Special functions that we must not change
        const std::unordered_set<std::string> SimpleFuncs = {
            "runtime.gcWriteBarrier", "runtime.duffzero", "runtime.duffcopy"};

        if (SimpleFuncs.find(Function.getDemangledName()) != SimpleFuncs.end())
          Function.setSimple(false);

        auto GetPcdata = [&](const uint32_t Index) -> bool {
          int32_t Max = -1;
          uint32_t MapOffsetVal = GoFunc->getPcdata(Index);
          if (MapOffsetVal) {
            uint64_t MapOffset = Pclntab->getPctabOffset() + MapOffsetVal;
            Max = readVarintPass(&Function, GoFunc.get(), DE, &MapOffset, Index,
                                 Pclntab->getQuantum(), AllocId,
                                 Emitter.MCE.get());
          }

          GoFunc->setPcdataMaxVal(Index, Max);
          return !!MapOffsetVal;
        };

        if (!GetPcdata(GoFunc->getPcdataUnsafePointIndex())) {
          // The function has no PCDATA_UnsafePoint info, so we will mark every
          // instruction as a safe one.
          const int SafePoint = GoFunc->getPcdataSafePointVal();
          const uint32_t Index = GoFunc->getPcdataUnsafePointIndex();
          for (BinaryBasicBlock *BB : Function.getLayout().blocks())
            for (auto II = BB->begin(); II != BB->end(); ++II)
              addVarintAnnotation(BC, *II, Index, SafePoint, /*IsNext*/ false,
                                  AllocId);
        }

        GetPcdata(GoFunc->getPcdataStackMapIndex());
        GetPcdata(GoFunc->getPcdataInlTreeIndex());

        uint64_t DeferOffset = GoFunc->getDeferreturnOffset();
        if (DeferOffset)
          deferreturnPass(Function, DeferOffset, AllocId, Emitter.MCE.get());

        // If the function does not have stack map index varint
        // it was probably written in asm
        if (GoFunc->getPcdataMax(GoFunc->getPcdataStackMapIndex()) == -1)
          Function.setIsAsm(true);

        // ASM Functions might use the system stack and we won't be able to
        // locate that the stack was switched.
        // TODO For functions with deferreturn calls we preserve the table since
        // the BB is unreachable we are unable calculate stack offset currently.
        if (GoFunc->getPcspOffset() &&
            (Function.isAsm() || DeferOffset || opts::GolangPcspPreserve)) {
          uint64_t Offset = Pclntab->getPctabOffset() + GoFunc->getPcspOffset();
          readVarintPass(&Function, GoFunc.get(), DE, &Offset,
                         GoFunc->getPcspIndex(), Pclntab->getQuantum(), AllocId,
                         Emitter.MCE.get());
        }

        {
          // Remove funcdata relocations
          uint32_t Foffset = GoFunc->getFuncdataOffset();
          for (int I = 0; I < GoFunc->getNfuncdata(); ++I) {
            RemoveRelaReloc(BC, Section, Foffset);
            Foffset += getPsize();
          }
        }

        inlTreePass(&Function, GoFunc.get(), AllocId, Emitter.MCE.get());
      };

  ParallelUtilities::PredicateTy skipFunc =
      [&](const BinaryFunction &Function) { return !Function.isGolang(); };

  ParallelUtilities::runOnEachFunctionWithUniqueAllocId(
      BC, ParallelUtilities::SchedulingPolicy::SP_INST_QUADRATIC, WorkFun,
      skipFunc, "pcdataGoPreProcess", /*ForceSequential*/ true);

  return 0;
}

void GolangPrePass::goPassInit(BinaryContext &BC) {
  // NOTE Currently we don't support PCSP table restoration for
  // AARCH64 since we have many ways to increment/decrement stack
  // values and often stack value is changed through other
  // registers so we will need to track all registers in order
  // to properly find stack movement values
  if (BC.isAArch64() && !opts::GolangPcspPreserve) {
    LLVM_DEBUG(
        dbgs() << "BOLT-INFO: Enabling GolangPcspPreserve for AARCH64!\n");
    opts::GolangPcspPreserve = true;
  }

  BC.MIB->getOrCreateAnnotationIndex("IsDefer");

  // Initialize annotation index for multi-thread access
  std::unique_ptr<struct GoFunc> GoFunc = createGoFunc();
  auto initAnnotation = [&](const unsigned Index) {
    BC.MIB->getOrCreateAnnotationIndex(getVarintName(Index));
    BC.MIB->getOrCreateAnnotationIndex(getVarintName(Index, /*IsNext*/ true));
  };

  initAnnotation(GoFunc->getPcdataUnsafePointIndex());
  initAnnotation(GoFunc->getPcdataStackMapIndex());
  initAnnotation(GoFunc->getPcdataInlTreeIndex());
  initAnnotation(GoFunc->getPcspIndex());
}

void GolangPrePass::nopPass(BinaryContext &BC) {
  // The golang might gemerate unreachable jumps e.g.
  // https://go-review.googlesource.com/c/go/+/380894/
  // Removing the nops at branch destination might affect PCSP table generation
  // for the code below the nop. Remove NOP instruction annotation at the
  // beginning of the basic block in order to preserve BB layout for such cases.
  // Shorten multi-byte NOP before annotation remove.

  ParallelUtilities::WorkFuncWithAllocTy WorkFun =
      [&](BinaryFunction &Function, MCPlusBuilder::AllocatorIdTy AllocId) {
        for (BinaryBasicBlock *BB : Function.getLayout().blocks()) {
          MCInst &Inst = BB->front();
          if (!BC.MIB->isNoop(Inst))
            continue;

          BC.MIB->shortenInstruction(Inst, *BC.STI);
          BC.MIB->removeAnnotation(Inst, "NOP");
          BC.MIB->removeAnnotation(Inst, "Size");
        }
      };

  ParallelUtilities::PredicateTy skipFunc =
      [&](const BinaryFunction &Function) { return !Function.isGolang(); };

  ParallelUtilities::runOnEachFunctionWithUniqueAllocId(
      BC, ParallelUtilities::SchedulingPolicy::SP_INST_QUADRATIC, WorkFun,
      skipFunc, "nopGoPreProcess", /*ForceSequential*/ true);
}

void GolangPrePass::runOnFunctions(BinaryContext &BC) {
  int Ret;

  goPassInit(BC);

  Ret = pclntabPass(BC);
  if (Ret < 0) {
    errs() << "BOLT-ERROR: Golang preprocess pclntab pass failed!\n";
    exit(1);
  }

  nopPass(BC);
}

} // end namespace bolt
} // end namespace llvm
