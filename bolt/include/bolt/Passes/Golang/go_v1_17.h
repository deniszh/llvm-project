//===--------- Passes/Golang/go_v1_17.h -------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_BOLT_GOLANG_V1_17_H
#define LLVM_TOOLS_LLVM_BOLT_GOLANG_V1_17_H

#include "go_v1_16_5.h"

namespace llvm {
namespace bolt {

class Pclntab_v1_17_2 : public Pclntab_v1_16_5 {
public:
  ~Pclntab_v1_17_2() = default;
};

struct GoFunc_v1_17_2 : GoFunc_v1_16_5 {
  // runtime/symtab.go
  enum {
    funcID_normal = 0,
    funcID_abort,
    funcID_asmcgocall,
    funcID_asyncPreempt,
    funcID_cgocallback,
    funcID_debugCallV2,
    funcID_gcBgMarkWorker,
    funcID_goexit,
    funcID_gogo,
    funcID_gopanic,
    funcID_handleAsyncEvent,
    funcID_jmpdefer,
    funcID_mcall,
    funcID_morestack,
    funcID_mstart,
    funcID_panicwrap,
    funcID_rt0_go,
    funcID_runfinq,
    funcID_runtime_main,
    funcID_sigpanic,
    funcID_systemstack,
    funcID_systemstack_switch,
    funcID_wrapper
  };

  bool hasReservedID(std::string Name) const override {
    // NOTE Go 1.17 has a bug with function names containing '_' symbol.
    // https://go-review.googlesource.com/c/go/+/396797
    // The end of the name might contain .abi0 prefix
    const char Rt0Go[] = "runtime.rt0_go";
    const char StackSwitch[] = "runtime.systemstack_switch";

    return (__GoFunc.FuncID != funcID_normal &&
            __GoFunc.FuncID != funcID_wrapper) ||
           !strncmp(Name.c_str(), Rt0Go, sizeof(Rt0Go) - 1) ||
           !strncmp(Name.c_str(), StackSwitch, sizeof(StackSwitch) - 1);
  }

  ~GoFunc_v1_17_2() = default;
};

struct Module_v1_17_2 : Module_v1_16_5 {
  ~Module_v1_17_2() = default;
};

class Pclntab_v1_17_5 : public Pclntab_v1_17_2 {
public:
  ~Pclntab_v1_17_5() = default;
};

struct GoFunc_v1_17_5 : GoFunc_v1_17_2 {
  ~GoFunc_v1_17_5() = default;
};

struct Module_v1_17_5 : Module_v1_17_2 {
  ~Module_v1_17_5() = default;
};

class Pclntab_v1_17_8 : public Pclntab_v1_17_5 {
public:
  ~Pclntab_v1_17_8() = default;
};

struct GoFunc_v1_17_8 : GoFunc_v1_17_5 {
  ~GoFunc_v1_17_8() = default;
};

struct Module_v1_17_8 : Module_v1_17_5 {
  ~Module_v1_17_8() = default;
};

} // namespace bolt
} // namespace llvm

#endif
