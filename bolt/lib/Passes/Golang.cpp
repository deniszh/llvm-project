//===--- Golang.cpp -------------------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#include "bolt/Passes/Golang.h"
#include "bolt/Core/ParallelUtilities.h"
#include "llvm/Support/EndianStream.h"

#define DEBUG_TYPE "bolt-golang"

using namespace llvm;
using namespace bolt;

namespace opts {
extern cl::OptionCategory BoltOptCategory;

extern cl::opt<bool> Instrument;
extern cl::opt<bool> NoHugePages;
extern cl::opt<unsigned> AlignFunctions;

cl::opt<bool>
    GolangPcspPreserve("golang-preserve-pcsp",
                       cl::desc("Save pcsp table instead of reconstructing it"),
                       cl::init(false), cl::ZeroOrMore, cl::Hidden,
                       cl::cat(BoltOptCategory));

} // end namespace opts

namespace llvm {
namespace bolt {

#define KINDMASK ((1 << 5) - 1) // runtime/typekind.go
#define UNCOMMON_FLAG (1 << 0)  // runtime/type.go

// reflect/type.go ; runtime/typekind.go
enum Kind {
  Invalid = 0,
  Bool,
  Int,
  Int8,
  Int16,
  Int32,
  Int64,
  Uint,
  Uint8,
  Uint16,
  Uint32,
  Uint64,
  Uintptr,
  Float32,
  Float64,
  Complex64,
  Complex128,
  Array,
  Chan,
  Func,
  Interface,
  Map,
  Ptr,
  Slice,
  String,
  Struct,
  UnsafePointer,
  LastKind
};

static std::map<const BinaryBasicBlock *, uint64_t> BBSizes;

static int updateBBSizes(BinaryContext &BC) {
  ParallelUtilities::WorkFuncTy WorkFun = [&](BinaryFunction &BF) {
    for (BinaryBasicBlock *BB : BF.getLayout().blocks())
      BBSizes[BB] = BB->estimateSize();
  };

  ParallelUtilities::runOnEachFunction(
      BC, ParallelUtilities::SchedulingPolicy::SP_TRIVIAL, WorkFun, nullptr,
      "UpdateEstimatedSizes",
      /*ForceSequential*/ true);
  return 0;
}

static uint64_t getBFCodeSize(BinaryFunction *BF) {
  uint64_t Size = 0;
  for (BinaryBasicBlock *BB : BF->getLayout().blocks())
    Size += BBSizes[BB];

  return Size;
}

static uint64_t getBFSize(BinaryFunction *BF) {
  uint64_t Size = getBFCodeSize(BF);
  Size += BF->estimateConstantIslandSize();
  return Size;
}

static uint64_t getBFInstrOffset(const BinaryBasicBlock *BB,
                                 const MCInst *Instr) {
  uint64_t Offset = 0;
  BinaryFunction *BF = BB->getFunction();
  for (BinaryBasicBlock *BasicBlock : BF->getLayout().blocks()) {
    if (BB == BasicBlock)
      break;

    Offset += BBSizes[BasicBlock];
  }

  BinaryContext &BC = BF->getBinaryContext();
  for (const MCInst &II : *BB) {
    if (Instr == &II)
      return Offset;

    Offset += BC.computeInstructionSize(II);
  }

  llvm_unreachable("Wrong BB or Instr");
  exit(1);
}

inline static uint64_t getNewTextStart(BinaryContext &BC) {
  // NOTE The new text address is allocated after all passes were finished,
  // for now use the first free address stored in BC.LayoutStartAddress
  return alignTo(BC.LayoutStartAddress, BC.PageAlign);
}

BinaryFunction *getBF(BinaryContext &BC, std::vector<BinaryFunction *> &BFs,
                      const char *Name) {
  for (auto BFit = BFs.rbegin(); BFit != BFs.rend(); ++BFit) {
    BinaryFunction *BF = *BFit;
    if (BF->hasRestoredNameRegex(Name))
      return BF;
  }

  return nullptr;
}

BinaryFunction *getFirstBF(BinaryContext &BC,
                           std::vector<BinaryFunction *> &BFs) {
  return getBF(BC, BFs, GolangPass::getFirstBFName());
}

BinaryFunction *getLastBF(BinaryContext &BC,
                          std::vector<BinaryFunction *> &BFs) {
  return getBF(BC, BFs, GolangPass::getLastBFName());
}

uint64_t readEndianVal(DataExtractor &DE, uint64_t *Offset, uint16_t Size);

uint32_t readVarint(DataExtractor &DE, uint64_t *Offset);

int32_t readVarintPair(DataExtractor &DE, uint64_t *MapOffset, int32_t &ValSum,
                       uint32_t &OffsetSum);

static void writeVarint(uint8_t *Data, uint64_t *Offset, uint32_t Val) {
  while (Val >= 0x80) {
    Data[(*Offset)++] = (uint8_t)(Val | 0x80);
    Val >>= 7;
  }

  Data[(*Offset)++] = (uint8_t)Val;
}

static void writeVarint(uint8_t **Data, uint32_t Val) {
  uint64_t Offset = 0;
  writeVarint(*Data, &Offset, Val);
  *Data += Offset;
}

static void writeVarintPair(int32_t Val, int32_t &PrevVal, uint64_t Offset,
                            uint64_t &CurrentOffset, bool &IsFirst,
                            uint8_t **DataFuncOffset, const uint8_t Quantum) {
  int32_t V = Val - PrevVal;
  V = (V < 0) ? (((-V - 1) << 1) | 1) : V << 1;
  assert((V != 0 || IsFirst) && "The value detla could not be zero");
  PrevVal = Val;
  writeVarint(DataFuncOffset, (uint32_t)V);
  assert((Offset - CurrentOffset) % Quantum == 0 &&
         "Offset it not multiple of quantum");
  uint32_t CurrentDelta = (Offset - CurrentOffset) / Quantum;
  assert((CurrentDelta || IsFirst) && "The offset delta could not be zero");
  writeVarint(DataFuncOffset, CurrentDelta);
  CurrentOffset = Offset;
  IsFirst = false;
}

template <typename T> static T writeEndian(BinaryContext &BC, T Val) {
  T Ret;
  SmallVector<char, sizeof(T)> Tmp;
  raw_svector_ostream OS(Tmp);
  enum support::endianness Endian = support::big;
  if (BC.AsmInfo->isLittleEndian())
    Endian = support::little;

  struct support::endian::Writer Writer(OS, Endian);
  Writer.write<T>(Val);
  memcpy(&Ret, OS.str().data(), sizeof(T));
  return Ret;
}

void writeEndianVal(BinaryContext &BC, uint64_t Val, uint16_t Size,
                    uint8_t **Res) {
  switch (Size) {
  case 8: {
    uint64_t Endian = writeEndian<uint64_t>(BC, Val);
    **(uint64_t **)Res = Endian;
    break;
  }

  case 4: {
    uint32_t Endian = writeEndian<uint32_t>(BC, (uint32_t)Val);
    **(uint32_t **)Res = Endian;
    break;
  }

  case 2: {
    uint16_t Endian = writeEndian<uint16_t>(BC, (uint16_t)Val);
    **(uint16_t **)Res = Endian;
    break;
  }

  case 1: {
    **Res = (uint8_t)Val;
    break;
  }

  default:
    llvm_unreachable("Wrong type size");
    exit(1);
  }

  *Res += Size;
}

inline void writeEndianPointer(BinaryContext &BC, uint64_t Val, uint8_t **Res) {
  return writeEndianVal(BC, Val, BC.AsmInfo->getCodePointerSize(), Res);
}

std::string getVarintName(uint32_t Index, bool IsNext);

void addVarintAnnotation(BinaryContext &BC, MCInst &II, uint32_t Index,
                         int32_t Value, bool IsNext, unsigned AllocId);

bool hasVarintAnnotation(BinaryContext &BC, MCInst &II, uint32_t Index,
                         bool IsNext);

int32_t getVarintAnnotation(BinaryContext &BC, MCInst &II, uint32_t Index,
                            bool IsNext);

std::string getFuncdataName(uint32_t Findex, uint32_t Size);

std::string getFuncdataSizeName(uint32_t Findex);

void addFuncdataAnnotation(BinaryContext &BC, MCInst &II, uint32_t Findex,
                           int32_t Value, unsigned AllocId);

bool hasFuncdataAnnotation(BinaryContext &BC, MCInst &II, uint32_t Findex);

uint32_t getFuncdataSizeAnnotation(BinaryContext &BC, MCInst &II,
                                   uint32_t Findex);

int32_t getFuncdataAnnotation(BinaryContext &BC, MCInst &II, uint32_t Findex,
                              uint32_t Index);

void RemoveRelaReloc(BinaryContext &BC, BinarySection *Section,
                     uint64_t Offset);

void AddRelaReloc(BinaryContext &BC, MCSymbol *Symbol, BinarySection *Section,
                  uint64_t Offset, uint64_t Addend = 0);

static std::vector<BinaryFunction *>
getSortedGolangFunctions(BinaryContext &BC) {
  std::vector<BinaryFunction *> BFs = BC.getSortedFunctions();
  BFs.erase(std::remove_if(BFs.begin(), BFs.end(),
                           [](BinaryFunction *BF) {
                             return !BF->isGolang() || BF->isFolded();
                           }),
            BFs.end());
  return BFs;
}

Pclntab::~Pclntab() {}

int Pclntab::readHeader(BinaryContext &BC, const uint64_t PclntabHeaderAddr) {
  BinaryData *PclntabSym = BC.getBinaryDataAtAddress(PclntabHeaderAddr);
  if (!PclntabSym) {
    errs() << "BOLT-ERROR: failed to get pclntab symbol!\n";
    return -1;
  }

  BinarySection *Section = &PclntabSym->getSection();
  uint64_t Offset = PclntabHeaderAddr - Section->getAddress();
  setPclntabHeaderOffset(Offset);
  DataExtractor DE =
      DataExtractor(Section->getContents(), BC.AsmInfo->isLittleEndian(),
                    BC.AsmInfo->getCodePointerSize());

  __readHeader(BC, DE);

  if (!checkMagic()) {
    errs() << "BOLT-ERROR: pclntab bad magic!\n";
    return -1;
  }

  if (getPsize() != BC.AsmInfo->getCodePointerSize()) {
    outs() << "BOLT-ERROR: pclntab bad pointer size!\n";
    return -1;
  }

  return 0;
}

int Pclntab::writeHeader(BinaryContext &BC, uint8_t *Pclntab) {
  setNewHeaderOffsets();
  __writeHeader(BC, Pclntab);
  return 0;
}

Module::~Module() {}

int Module::read(const BinaryContext &BC) {
  // NOTE The local.moduledata are used in plugins.
  // The firstmoduledata symbol still could be found there
  // but it will point in BSS section */
  const BinaryData *Module = BC.getFirstBinaryDataByName("local.moduledata");
  if (!Module)
    Module = BC.getFirstBinaryDataByName("runtime.firstmoduledata");

  if (!Module) {
    errs() << "BOLT-ERROR: failed to get firstmoduledata symbol!\n";
    return -1;
  }

  const BinarySection *Section = &Module->getSection();
  DataExtractor DE =
      DataExtractor(Section->getContents(), BC.AsmInfo->isLittleEndian(),
                    BC.AsmInfo->getCodePointerSize());

  uint64_t Offset = Module->getAddress() - Section->getAddress();
  uint64_t *ModuleArr = getModule();
  for (size_t I = 0; I < getModuleSize() / sizeof(uint64_t); ++I) {
    assert(DE.isValidOffset(Offset) && "Invalid offset");
    ModuleArr[I] = DE.getAddress(&Offset);
  }

  return 0;
}

GoFunc::~GoFunc() {}

std::unique_ptr<struct Module> createGoModule() {
  if (opts::GolangPass == opts::GV_1_17_8)
    return std::make_unique<Module_v1_17_8>();
  else if (opts::GolangPass == opts::GV_1_17_5)
    return std::make_unique<Module_v1_17_5>();
  else if (opts::GolangPass == opts::GV_1_17_2)
    return std::make_unique<Module_v1_17_2>();
  else if (opts::GolangPass == opts::GV_1_16_5)
    return std::make_unique<Module_v1_16_5>();
  else if (opts::GolangPass == opts::GV_1_14_12)
    return std::make_unique<Module_v1_14_12>();
  else if (opts::GolangPass == opts::GV_1_14_9)
    return std::make_unique<Module_v1_14_9>();

  llvm_unreachable("Wrong golang version");
  exit(1);
}

std::unique_ptr<class Pclntab> createGoPclntab() {
  if (opts::GolangPass == opts::GV_1_17_8)
    return std::make_unique<Pclntab_v1_17_8>();
  else if (opts::GolangPass == opts::GV_1_17_5)
    return std::make_unique<Pclntab_v1_17_5>();
  else if (opts::GolangPass == opts::GV_1_17_2)
    return std::make_unique<Pclntab_v1_17_2>();
  else if (opts::GolangPass == opts::GV_1_16_5)
    return std::make_unique<Pclntab_v1_16_5>();
  else if (opts::GolangPass == opts::GV_1_14_12)
    return std::make_unique<Pclntab_v1_14_12>();
  else if (opts::GolangPass == opts::GV_1_14_9)
    return std::make_unique<Pclntab_v1_14_9>();

  llvm_unreachable("Wrong golang version");
  exit(1);
}

std::unique_ptr<struct GoFunc> createGoFunc() {
  if (opts::GolangPass == opts::GV_1_17_8)
    return std::make_unique<GoFunc_v1_17_8>();
  else if (opts::GolangPass == opts::GV_1_17_5)
    return std::make_unique<GoFunc_v1_17_5>();
  else if (opts::GolangPass == opts::GV_1_17_2)
    return std::make_unique<GoFunc_v1_17_2>();
  else if (opts::GolangPass == opts::GV_1_16_5)
    return std::make_unique<GoFunc_v1_16_5>();
  else if (opts::GolangPass == opts::GV_1_14_12)
    return std::make_unique<GoFunc_v1_14_12>();
  else if (opts::GolangPass == opts::GV_1_14_9)
    return std::make_unique<GoFunc_v1_14_9>();

  llvm_unreachable("Wrong golang version");
  exit(1);
}

struct StackVal {
  uint32_t Size;
  int32_t OldVal;
  int32_t Val;
};

using InstBias = std::map<uint32_t, struct StackVal>;

static uint32_t stackCounter(BinaryFunction *BF, BinaryBasicBlock *BB,
                             InstBias &Map, uint32_t SpVal) {
  BinaryContext &BC = BF->getBinaryContext();
  unsigned pSize = BC.AsmInfo->getCodePointerSize();
  auto addVal = [&](InstBias &Map, MCInst *II, int32_t OldVal, int32_t NewVal) {
    struct StackVal StackVal;
    uint32_t Offset = getBFInstrOffset(BB, II);
    StackVal.Size = BC.computeInstructionSize(*II);
    StackVal.OldVal = OldVal;
    StackVal.Val = NewVal;
    Map[Offset] = StackVal;
  };

  for (MCInst &II : *BB) {
    int Ret = 0;
    if (&II == &(*BB->begin()) || &II == &(*BB->rbegin()))
      addVal(Map, &II, SpVal, SpVal);

    // Ignore instrumentation stack usage
    if (opts::Instrument && BC.MIB->hasAnnotation(II, "IsInstrumentation"))
      continue;

    if ((Ret = BC.MIB->getStackAdjustment(II))) {
      // NOTE Seems to be the only exception is runtime.rt0_go function
      if (std::abs(Ret) % pSize == 0) {
        addVal(Map, &II, SpVal, SpVal + Ret);
        SpVal += Ret;
      }
    }
  }

  return SpVal;
}

int GolangPass::typePass(BinaryContext &BC, uint64_t TypeAddr) {
  static std::unordered_set<uint64_t> VisitedTypes;
  uint64_t Offset;
  uint64_t SectionAddr;

  if (VisitedTypes.find(TypeAddr) != VisitedTypes.end())
    return 0;

  VisitedTypes.insert(TypeAddr);
  ErrorOr<BinarySection &> Section = BC.getSectionForAddress(TypeAddr);
  if (!Section) {
    errs() << "BOLT-ERROR: failed to get section for type 0x"
           << Twine::utohexstr(TypeAddr) << "\n";
    return -1;
  }

  SectionAddr = Section->getAddress();
  DataExtractor DE = DataExtractor(Section->getContents(),
                                   BC.AsmInfo->isLittleEndian(), getPsize());
  Offset = TypeAddr - SectionAddr;
  assert(DE.isValidOffset(Offset) && "Invalid offset");

  // runtime/type.go
  struct Type {
    uint64_t Size;    // Pointer size
    uint64_t Ptrdata; // Pointer size
    uint32_t Hash;
    uint8_t Tflag;
    uint8_t Align;
    uint8_t Fieldalign;
    uint8_t Kind;
    uint64_t CompareFunc; // Pointer size
    uint64_t Gcdata;      // Pointer size
    int32_t NameOff;
    int32_t PtrToThis;
  } Type;

  Type.Size = DE.getAddress(&Offset);
  Type.Ptrdata = DE.getAddress(&Offset);
  Type.Hash = DE.getU32(&Offset);
  Type.Tflag = DE.getU8(&Offset);
  Type.Align = DE.getU8(&Offset);
  Type.Fieldalign = DE.getU8(&Offset);
  Type.Kind = DE.getU8(&Offset);
  Type.CompareFunc = DE.getAddress(&Offset);
  Type.Gcdata = DE.getAddress(&Offset);
  Type.NameOff = (int32_t)DE.getU32(&Offset);
  Type.PtrToThis = (int32_t)DE.getU32(&Offset);

  if (!(Type.Tflag & UNCOMMON_FLAG))
    return 0;

  uint8_t Kind = Type.Kind & KINDMASK;
  assert(Kind < LastKind && "Wrong kind type");
  assert(DE.isValidOffset(Offset) && "Wrong offset");

  // The furter structures are in runtime/type.go file
  if ((Kind == Ptr) || (Kind == Slice)) {
    // struct ptrtype {
    //   //typ  _type;
    //   struct Type *elem;
    // };
    //
    // struct slicetype {
    //   //typ  _type;
    //   elem *_type;
    // };

    uint64_t Address = DE.getAddress(&Offset);
    typePass(BC, Address);
  } else if (Kind == Struct) {
    // struct structtype {
    //   //struct Type typ;
    //   pkgPath name // bytes *byte;
    //   fields  []structfield;
    // };

    struct {
      uint64_t Bytes;      // Pointer size
      uint64_t Type;       // Pointer size
      uint64_t OffsetAnon; // Pointer size
    } Structfield;

    Offset += getPsize(); // Skip Name
    uint64_t StructfieldAddress = DE.getAddress(&Offset);
    uint64_t Size = DE.getAddress(&Offset);
    Offset += getPsize(); // Skip second size

    assert(Section->containsAddress(StructfieldAddress) &&
           "Wrong StructfieldAddress");
    uint64_t StructfieldOffset = StructfieldAddress - Section->getAddress();
    while (Size--) {
      Structfield.Bytes = DE.getAddress(&StructfieldOffset);
      Structfield.Type = DE.getAddress(&StructfieldOffset);
      Structfield.OffsetAnon = DE.getAddress(&StructfieldOffset);
      typePass(BC, Structfield.Type);
    }
  } else if (Kind == Interface) {
    // struct interfacetype {
    //  //struct Type typ;
    //  pkgPath name // bytes *byte;
    //  mhdr    []Imethod;
    // };

    struct {
      int32_t Name;
      int32_t Ityp;
    } Imethod;

    Offset += getPsize(); // Skip Name
    uint64_t MhdrAddress = DE.getAddress(&Offset);
    uint64_t Size = DE.getAddress(&Offset);
    Offset += getPsize(); // Skip second size

    assert(Section->containsAddress(MhdrAddress) && "Wrong MhdrAddress");
    uint64_t MhdrOffset = MhdrAddress - Section->getAddress();
    while (Size--) {
      Imethod.Name = (int32_t)DE.getU32(&MhdrOffset);
      Imethod.Ityp = (int32_t)DE.getU32(&MhdrOffset);
      typePass(BC, FirstModule->getTypes() + Imethod.Ityp);
    }
  } else if (Kind == Func) {
    // struct functype {
    //  //typ      _type;
    //  inCount  uint16;
    //  outCount uint16;
    // };

    Offset += 2 * sizeof(uint16_t);
    // NOTE in this case we must align offset
    Offset = alignTo(Offset, getPsize());
  } else if (Kind == Array) {
    // struct arraytype {
    //   //typ   _type;
    //   elem  *_type;
    //   slice *_type;
    //   len   uintptr;
    // };

    uint64_t Addr;
    Addr = DE.getAddress(&Offset);
    typePass(BC, Addr);
    Addr = DE.getAddress(&Offset);
    typePass(BC, Addr);
    Offset += getPsize();
  } else if (Kind == Chan) {
    // struct chantype {
    //  //typ  _type;
    //  elem *_type;
    //  dir  uintptr;
    // };

    uint64_t Addr = DE.getAddress(&Offset);
    typePass(BC, Addr);
    Offset += getPsize();
  } else if (Kind == Map) {
    // Large structure, seems to be no align needed though
    // The first 3 fields are type struct
    uint64_t Addr;
    Addr = DE.getAddress(&Offset);
    typePass(BC, Addr);
    Addr = DE.getAddress(&Offset);
    typePass(BC, Addr);
    Addr = DE.getAddress(&Offset);
    typePass(BC, Addr);
    Offset += 2 * getPsize();
  }

  assert(Offset == alignTo(Offset, getPsize()) && "Wrong alignment");
  assert(DE.isValidOffset(Offset) && "Invalid Offset");

  uint64_t UncommonOffset = Offset;

  // runtime/type.go
  struct {
    int32_t Pkgpath;
    uint16_t Mcount;
    uint16_t Xcount;
    uint32_t Moff;
    uint32_t Unused2;
  } Uncommontype;

  Uncommontype.Pkgpath = (int32_t)DE.getU32(&Offset);
  Uncommontype.Mcount = DE.getU16(&Offset);
  Uncommontype.Xcount = DE.getU16(&Offset);
  Uncommontype.Moff = DE.getU32(&Offset);
  Uncommontype.Unused2 = DE.getU32(&Offset);

  assert(UncommonOffset + Uncommontype.Moff >= Offset && "Wrong Moff");
  Offset = UncommonOffset + Uncommontype.Moff;
  while (Uncommontype.Mcount--) {
    assert(DE.isValidOffset(Offset) && "Invalid offset");
    // runtime/type.go
    struct {
      int32_t NameOff;
      int32_t TypeOff;
      int32_t Ifn;
      int32_t Tfn;
    } Method;

    Method.NameOff = (int32_t)DE.getU32(&Offset);
    Method.TypeOff = (int32_t)DE.getU32(&Offset);
    uint32_t IfnOffset = Offset;
    Method.Ifn = (int32_t)DE.getU32(&Offset);
    uint32_t TfnOffset = Offset;
    Method.Tfn = (int32_t)DE.getU32(&Offset);

    uint64_t StartAddr = getNewTextStart(BC);
    uint64_t RType = Relocation::getAbs(sizeof(uint32_t));
    auto setFn = [&](int32_t Value, uint32_t Offset) {
      if (Value == -1)
        return;

      BinaryFunction *Fn = BC.getBinaryFunctionAtAddress(RuntimeText + Value);
      if (!Fn) {
        errs() << "BOLT-ERROR: failed to get Ifn or Tfn!\n";
        exit(1);
      }

      BC.addRelocation(SectionAddr + Offset, Fn->getSymbol(), RType,
                       -StartAddr);
    };

    setFn(Method.Ifn, IfnOffset);
    setFn(Method.Tfn, TfnOffset);
  }

  return 0;
}

int GolangPass::typelinksPass(BinaryContext &BC) {
  int Ret;
  uint64_t Types = FirstModule->getTypes();
  if (!Types) {
    errs() << "BOLT-ERROR: types address is zero!\n";
    return -1;
  }

  uint64_t Etypes = FirstModule->getEtypes();
  assert(Types < Etypes && "Wrong Etypes");
  const GoArray &TypeLinks = FirstModule->getTypelinks();
  uint64_t TypelinkAddr = TypeLinks.getAddress();
  uint64_t TypelinkCount = TypeLinks.getCount();
  if (!TypelinkAddr) {
    errs() << "BOLT-ERROR: typelink address is zero!\n";
    return -1;
  }

  ErrorOr<BinarySection &> Section = BC.getSectionForAddress(TypelinkAddr);
  if (!Section) {
    errs() << "BOLT-ERROR: failed to get typelink section!\n";
    return -1;
  }

  DataExtractor DE = DataExtractor(Section->getContents(),
                                   BC.AsmInfo->isLittleEndian(), getPsize());

  uint64_t Offset = TypelinkAddr - Section->getAddress();
  while (TypelinkCount--) {
    assert(DE.isValidOffset(Offset) && "Invalid offset");
    uint64_t Type = Types + DE.getU32(&Offset);
    assert(Type < Etypes && "Wrong type offset");
    Ret = typePass(BC, Type);
    if (Ret < 0)
      return Ret;
  }

  return 0;
}

int GolangPass::textsectmapPass(BinaryContext &BC) {
  uint64_t TextSectAddr = FirstModule->getTextsectmap().getAddress();
  if (!TextSectAddr) {
    // Plugins does't have this structure
    return 0;
  }

  ErrorOr<BinarySection &> Section = BC.getSectionForAddress(TextSectAddr);
  if (!Section) {
    errs() << "BOLT-ERROR: failed to get textsectmaps section!\n";
    return -1;
  }

  BinaryData *EtextSymbol = BC.getFirstBinaryDataByName(getLastBFName());
  if (!EtextSymbol) {
    errs() << "BOLT-ERROR: failed to get etext symbol!\n";
    return -1;
  }

  // We will need to fix length field (text size) of textsect structure
  //
  // //runtime/symtab.go
  // struct textsect {
  //   uint64_t vaddr;    // Pointer size
  //   uint64_t length;   // Pointer size
  //   uint64_t baseaddr; // Pointer size
  // };

  uint32_t Offset = TextSectAddr + getPsize(); // length field
  uint64_t RType = Relocation::getAbs(getPsize());
  uint64_t Addend = -getNewTextStart(BC);
  BC.addRelocation(Offset, EtextSymbol->getSymbol(), RType, Addend);
  return 0;
}

int GolangPass::pcspPass(BinaryFunction *BF, uint8_t **SectionData,
                         const uint32_t Index, uint8_t Quantum,
                         bool ForcePreserve) {
  struct BBVal {
    BinaryBasicBlock *BB;
    uint32_t Val;
    bool IsUncond;
  } __attribute__((packed)) BBVal = {};

  std::queue<struct BBVal> Queue;
  std::unordered_map<BinaryBasicBlock *, bool> BBList;
  InstBias InstBias;

  if (BF->isAsm() || opts::GolangPcspPreserve || ForcePreserve)
    return writeVarintPass(BF, SectionData, Index, Quantum);

  if (BF->getLayout().block_begin() == BF->getLayout().block_end())
    return 0;

  BBVal.BB = *BF->getLayout().block_begin();
  Queue.push(BBVal);
  while (!Queue.empty()) {
    BinaryBasicBlock *BB = Queue.front().BB;
    uint32_t SpVal = Queue.front().Val;
    bool IsUncond = Queue.front().IsUncond;
    Queue.pop();

    // We are interested to find condition branching to BB
    // since uncondition one might be fallthrough
    auto Search = BBList.find(BB);
    if (Search != BBList.end() &&
        (/*not uncond*/ BBList[BB] == false || IsUncond))
      continue;

    BBList[BB] = IsUncond;
    BBVal.Val = stackCounter(BF, BB, InstBias, SpVal);
    for (BinaryBasicBlock *BBS : BB->successors()) {
      BBVal.BB = BBS;
      // We use getIndex() here to ensure that originally BBS was right
      // after BB, so if BB has only one successor no jmp instruction was in BB,
      // so potentially the BB could have noreturn call
      BBVal.IsUncond = IsUncond || !!(BB->succ_size() == 1 &&
                                      BBS->getIndex() == BB->getIndex() + 1);
      Queue.push(BBVal);
    }
  }

  uint64_t Offset, CurrentOffset = 0;
  int32_t PrevVal = -1, CurrentVal = 0;
  bool IsFirst = true;
  if (InstBias.empty())
    goto out;

  for (auto &I : InstBias) {
    if (I.second.Val == CurrentVal)
      continue;

    Offset = I.first;
    // If the condition was not met it means that the CurrentVal
    // belongs to the last instruction of the prev BB
    if (CurrentVal == I.second.OldVal)
      Offset += I.second.Size;

    writeVarintPair(CurrentVal, PrevVal, Offset, CurrentOffset, IsFirst,
                    SectionData, Quantum);
    CurrentVal = I.second.Val;
  }

  // Add Last Value
  Offset = InstBias.rbegin()->first + InstBias.rbegin()->second.Size;
  if (CurrentOffset < Offset) {
    writeVarintPair(CurrentVal, PrevVal, Offset, CurrentOffset, IsFirst,
                    SectionData, Quantum);
  }

out:;
  **SectionData = 0;
  (*SectionData)++;
  return 0;
}

uint32_t GolangPass::deferreturnPass(BinaryContext &BC,
                                     BinaryFunction *Function) {
  for (const BinaryBasicBlock *BB : Function->getLayout().rblocks()) {
    for (auto II = BB->begin(); II != BB->end(); ++II) {
      if (!BC.MIB->hasAnnotation(*II, "IsDefer"))
        continue;

      return getBFInstrOffset(BB, &(*II));
    }
  }

  errs() << "BOLT-ERROR: deferreturn call was not found for " << *Function
         << "\n";
  exit(1);
}

int GolangPass::getNextMCinstVal(FunctionLayout::block_iterator BBIt,
                                 uint64_t I, const uint32_t Index, int32_t &Val,
                                 uint64_t *NextOffset) {
  BinaryFunction *BF = (*BBIt)->getFunction();
  BinaryContext &BC = BF->getBinaryContext();
  // We're interating in value for the next instruction
  auto II = std::next((*BBIt)->begin(), I + 1);
  do {
    if (II == (*BBIt)->end()) {
      BBIt = std::next(BBIt);
      if (BBIt == BF->getLayout().block_end()) {
        // Last Instruction
        return -1;
      }

      II = (*BBIt)->begin();
    }

    while (II != (*BBIt)->end() && !hasVarintAnnotation(BC, *II, Index)) {
      if (NextOffset)
        *NextOffset += BC.computeInstructionSize(*II);
      II = std::next(II);
    }

  } while (II == (*BBIt)->end());

  Val = getVarintAnnotation(BC, *II, Index);
  return 0;
}

int GolangPass::writeVarintPass(BinaryFunction *BF, uint8_t **DataFuncOffset,
                                const uint32_t Index, const uint8_t Quantum) {
  int Ret;
  uint64_t CurrentOffset = 0, Offset = 0;
  int32_t PrevVal = -1, Val;
  bool IsFirst = true;
  size_t Size = getBFCodeSize(BF);
  BinaryContext &BC = BF->getBinaryContext();
  for (auto BBIt = BF->getLayout().block_begin();
       BBIt != BF->getLayout().block_end(); ++BBIt) {
    BinaryBasicBlock *BB = *BBIt;
    for (uint64_t I = 0; I < BB->size(); ++I) {
      MCInst &Inst = BB->getInstructionAtIndex(I);
      Offset += BC.computeInstructionSize(Inst);
      if (Offset < CurrentOffset)
        continue;

      if (!hasVarintAnnotation(BC, Inst, Index)) {
        if (Offset == Size && IsFirst)
          return -1;

        continue;
      }

      Val = getVarintAnnotation(BC, Inst, Index);

      int32_t NextInstVal;
      uint64_t NextOffset = Offset;
      Ret = getNextMCinstVal(BBIt, I, Index, NextInstVal, &NextOffset);
      if (Ret < 0) {
        Offset = NextOffset;
        goto bf_done;
      }

      if (Val != NextInstVal)
        writeVarintPair(Val, PrevVal, NextOffset, CurrentOffset, IsFirst,
                        DataFuncOffset, Quantum);
    }
  }

bf_done:;
  // Create entry for the last instruction
  writeVarintPair(Val, PrevVal, Offset, CurrentOffset, IsFirst, DataFuncOffset,
                  Quantum);
  **DataFuncOffset = 0;
  (*DataFuncOffset)++;
  return 0;
}

static void inlTreePass(BinaryFunction *BF, struct GoFunc *GoFunc,
                        const char *OldPclntabNames, uint8_t **DataFuncOffset,
                        const uint8_t *const SectionData) {
  BinaryContext &BC = BF->getBinaryContext();
  const unsigned InlIndex = GoFunc->getFuncdataInlTreeIndex();
  const uint64_t Address = GoFunc->getFuncdata(InlIndex);
  if (!Address)
    return;

  static std::unordered_map<uint64_t, uint32_t> InlHash; // String hash, offset
  ErrorOr<BinarySection &> FuncdataSection = BC.getSectionForAddress(Address);
  if (!FuncdataSection) {
    errs() << "BOLT-ERROR: failed to get section for inline 0x"
           << Twine::utohexstr(Address) << "\n";
  }

  DataExtractor DE = DataExtractor(FuncdataSection->getContents(),
                                   BC.AsmInfo->isLittleEndian(),
                                   BC.AsmInfo->getCodePointerSize());

  std::unordered_map<uint32_t, uint32_t> ParentOffset; // Val:newOffset
  uint32_t MaxInlCount = 0;
  for (BinaryBasicBlock *BB : BF->getLayout().blocks()) {
    for (MCInst &II : *BB) {
      if (!hasFuncdataAnnotation(BC, II, InlIndex))
        continue;

      uint32_t Size = getFuncdataSizeAnnotation(BC, II, InlIndex);
      MaxInlCount += Size;
      for (uint32_t I = 0; I < Size; ++I) {
        int32_t Index = getFuncdataAnnotation(BC, II, InlIndex, I);
        ParentOffset[Index] = getBFInstrOffset(BB, &II);
      }
    }
  }

  for (uint32_t I = 0; I < MaxInlCount; ++I) {
    uint64_t Offset = Address - FuncdataSection->getAddress();
    Offset += I * sizeof(InlinedCall);

    struct InlinedCall InlinedCall;
    uint64_t ReadOffset = Offset;
    assert(DE.isValidOffset(ReadOffset) && "Invalid offset");
    InlinedCall.Parent = (int16_t)DE.getU16(&ReadOffset);
    InlinedCall.FuncID = DE.getU8(&ReadOffset);
    InlinedCall.Unused = DE.getU8(&ReadOffset);
    InlinedCall.File = DE.getU32(&ReadOffset);
    InlinedCall.Line = DE.getU32(&ReadOffset);
    InlinedCall.Func = DE.getU32(&ReadOffset);
    InlinedCall.ParentPc = DE.getU32(&ReadOffset);

    // Copy inline function name if it was not copied already
    const char *Name = OldPclntabNames + InlinedCall.Func;
    std::hash<std::string> hasher;
    auto Hash = hasher(std::string(Name));
    if (InlHash.find(Hash) == InlHash.end()) {
      InlHash[Hash] = *DataFuncOffset - SectionData;
      size_t NameLen = strlen(Name) + 1;
      memcpy(*DataFuncOffset, Name, NameLen);
      *DataFuncOffset += NameLen;
    }

    // Use addend as relocation value
    MCSymbol *ZeroSym = BC.registerNameAtAddress("Zero", 0, 0, 0);

    // Zero out file and line fields
    FuncdataSection->addRelocation(Offset + offsetof(struct InlinedCall, File),
                                   ZeroSym,
                                   Relocation::getAbs(sizeof(uint32_t)));
    FuncdataSection->addRelocation(Offset + offsetof(struct InlinedCall, Line),
                                   ZeroSym,
                                   Relocation::getAbs(sizeof(uint32_t)));

    // Create relocation for inline function name
    FuncdataSection->addRelocation(
        Offset + offsetof(struct InlinedCall, Func), ZeroSym,
        Relocation::getAbs(sizeof(uint32_t)), InlHash[Hash]);

    // Create relocation for parentPc offset
    FuncdataSection->addRelocation(
        Offset + offsetof(struct InlinedCall, ParentPc), ZeroSym,
        Relocation::getAbs(sizeof(uint32_t)), ParentOffset[I]);
  }
}

int GolangPass::unsafePointPass(BinaryFunction *BF, GoFunc *GoFunc) {
  BinaryContext &BC = BF->getBinaryContext();
  const uint32_t UnsafePointIndex = GoFunc->getPcdataUnsafePointIndex();
  const int UnsafeVal = GoFunc->getPcdataUnsafePointVal();
  for (BinaryBasicBlock *BB : BF->getLayout().blocks()) {
    for (MCInst &Inst : *BB) {
      bool HasMap = hasVarintAnnotation(BC, Inst, UnsafePointIndex);
      if (HasMap)
        continue;

      // The regular branches are the only excpetions for inserted instructions
      // to be not unsafe point
      if ((BC.MIB->isBranch(Inst)) && !BC.MIB->isIndirectBranch(Inst))
        continue;

      addVarintAnnotation(BC, Inst, UnsafePointIndex, UnsafeVal,
                          /*IsNext*/ false);
    }
  }

  return 0;
}

int GolangPass::pclntabPass(BinaryContext &BC) {
  // yota9: The mmap is used since at this point we are unable to tell the final section size, we are allocating 8 times more of original space, which is more than enough. Since mmap allocates pages lazily we don't have extra memory consumption. However we probalbe should use ordinary new here
  int Ret;
  const uint64_t PclntabAddr = getPcHeaderAddr();
  if (!PclntabAddr) {
    errs() << "BOLT-ERROR: pclntab address is zero!\n";
    return -1;
  }

  BinaryData *PclntabSym = BC.getBinaryDataAtAddress(PclntabAddr);
  if (!PclntabSym) {
    errs() << "BOLT-ERROR: failed to get pclntab symbol!\n";
    return -1;
  }

  BinarySection *Section = &PclntabSym->getSection();
  const unsigned SectionFlags = BinarySection::getFlags(/*IsReadOnly=*/false,
                                                        /*IsText=*/false,
                                                        /*IsAllocatable=*/true);
  uint64_t SectionSize = 0;
  BinarySection *OutputSection =
      &BC.registerOrUpdateSection(".pclntab", ELF::SHT_PROGBITS, SectionFlags,
                                  nullptr, ~0ULL, sizeof(uint64_t));

  // NOTE Currently we don't know how much data we will have in pclntab section.
  // We would allocate 8 times more then original size.
  const uint64_t SectionMaxSize =
      alignTo(PclntabSym->getSize(), BC.RegularPageSize) * 8;
  uint8_t *const SectionData = new uint8_t[SectionMaxSize];
  if (!SectionData) {
    errs() << "BOLT-ERROR: failed to allocate new .pclntab section\n";
    return -1;
  }

  DataExtractor DE = DataExtractor(Section->getContents(),
                                   BC.AsmInfo->isLittleEndian(), getPsize());

  static std::vector<BinaryFunction *> BFs = getSortedGolangFunctions(BC);
  const uint8_t PclnEntrySize = getPsize() * 2;
  const size_t BFCount = BFs.size();
  // Reserve one entry for maxpc, written before FuncPart starts
  const uint64_t FuncOffset = BFCount * PclnEntrySize + PclnEntrySize;
  uint8_t *OffsetPart = SectionData + Pclntab->getPcHeaderSize();
  uint8_t *FuncPart = OffsetPart + FuncOffset;

  // NOTE IsFirstName variable is a hack used due to the bug in go1.16:
  // https://go-review.googlesource.com/c/go/+/334789
  bool IsFirstName = true;
  for (BinaryFunction *BF : BFs) {
    assert((uint64_t)(FuncPart - SectionData) < SectionMaxSize &&
           "Overflow error");

    {
      // Add Functab Offsets
      uint64_t Delta = (uint64_t)(OffsetPart - SectionData);
      AddRelaReloc(BC, BF->getSymbol(), OutputSection, Delta);
      OffsetPart += getPsize();
      writeEndianPointer(BC, FuncPart - SectionData, &OffsetPart);
    }

    uint64_t OldTabOffset = BF->getGolangFunctabOffset();
    std::unique_ptr<struct GoFunc> GoFunc = createGoFunc();

    // Read func structure
    GoFunc->read(BC, DE, nullptr, &OldTabOffset);

    // Get target structure size
    size_t FuncSize = GoFunc->getSize(BC);
    uint8_t *DataFuncOffset = FuncPart + FuncSize;

    // We don't interested in metadata tables anymore
    GoFunc->disableMetadata();

    // Save space for npcdata
    DataFuncOffset += GoFunc->getPcdataSize();

    // Save aligned space for nfuncdata
    DataFuncOffset =
        SectionData + alignTo(DataFuncOffset - SectionData, getPsize());
    DataFuncOffset += GoFunc->getNfuncdata() * getPsize();

    // Save name
    const char *OldPclntabNames =
        (char *)Section->getData() + Pclntab->getNameOffset();
    if (GoFunc->getNameOffset() || IsFirstName) {
      IsFirstName = false;
      const char *Name = OldPclntabNames + GoFunc->getNameOffset();
      size_t NameLen = strlen(Name) + 1;
      memcpy(DataFuncOffset, Name, NameLen);
      GoFunc->setNameOffset(DataFuncOffset - SectionData);
      DataFuncOffset += NameLen;
    }

    // Mark inserted instructions as unsafe points
    unsafePointPass(BF, GoFunc.get());

    // Fix pcdata
    auto setPcdata = [&](const uint32_t Index) {
      GoFunc->setPcdata(Index, DataFuncOffset - SectionData);
      if (writeVarintPass(BF, &DataFuncOffset, Index, Pclntab->getQuantum()) <
          0) {
        GoFunc->setPcdata(Index, 0);
        return;
      }
    };

    setPcdata(GoFunc->getPcdataUnsafePointIndex());
    setPcdata(GoFunc->getPcdataStackMapIndex());
    setPcdata(GoFunc->getPcdataInlTreeIndex());

    // Fix npcdata
    GoFunc->fixNpcdata();

    // Fix funcdata inline
    inlTreePass(BF, GoFunc.get(), OldPclntabNames, &DataFuncOffset,
                SectionData);

    // Fix deferreturn
    if (GoFunc->getDeferreturnOffset())
      GoFunc->setDeferreturnOffset(deferreturnPass(BC, BF));

    // Fix pcsp
    if (GoFunc->getPcspOffset()) {
      // TODO don't preserve PCSP table for functions with deferreturn
      bool ForcePreserve = GoFunc->getDeferreturnOffset();
      GoFunc->setPcspOffset(DataFuncOffset - SectionData);
      Ret = pcspPass(BF, &DataFuncOffset, GoFunc->getPcspIndex(),
                     Pclntab->getQuantum(), ForcePreserve);
      if (Ret < 0)
        goto failed;
    }

    GoFunc->write(BF, &FuncPart, SectionData, OutputSection);
    FuncPart = SectionData + alignTo(DataFuncOffset - SectionData, getPsize());
  }

  SectionSize = FuncPart - SectionData;

  {
    // The last OffsetPart is maxpc and offset to filetab
    std::vector<BinaryFunction *> BFs = BC.getSortedFunctions();
    BinaryFunction *LastBF = getLastBF(BC, BFs);
    uint64_t Delta = (uint64_t)(OffsetPart - SectionData);
    AddRelaReloc(BC, LastBF->getSymbol(), OutputSection, Delta);
    OffsetPart += getPsize();
    writeEndianPointer(BC, 0, &OffsetPart);
  }

  // Write fixed Pclntab structure
  Pclntab->setFunctionsCount(BFCount);
  Pclntab->writeHeader(BC, SectionData);

  // Fix section sizes size
  FirstModule->setPclntabSize(SectionSize);

  // Fix ftab size
  FirstModule->setFtabSize(BFCount);

  OutputSection->updateContents(SectionData, SectionSize);
  OutputSection->setIsFinalized();

  PclntabSym->setOutputSize(SectionSize);
  PclntabSym->setOutputLocation(*OutputSection, 0);
  return 0;

failed:;
  delete[] SectionData;
  return Ret;
}

int GolangPass::findFuncTabPass(BinaryContext &BC) {
  // const uint32_t minfunc = 16; // runtime/symtab.go: minimum function size
  const uint32_t pcsubbucketssize = 256;
  // const uint32_t pcbucketsize = pcsubbucketssize * minfunc; //
  // runtime/symtab.go: size of bucket in the pc->func lookup table

  uint64_t FindFuncTab = FirstModule->getFindfunctab();
  if (!FindFuncTab) {
    errs() << "BOLT-ERROR: findfunctab is zero!\n";
    return -1;
  }

  BinaryData *FindfunctabSym = BC.getBinaryDataAtAddress(FindFuncTab);
  if (!FindfunctabSym) {
    errs() << "BOLT-ERROR: failed to get findfunctab symbol!\n";
    return -1;
  }

  const unsigned SectionFlags = BinarySection::getFlags(/*IsReadOnly=*/true,
                                                        /*IsText=*/false,
                                                        /*IsAllocatable=*/true);
  uint64_t SectionSize = 0;
  BinarySection *OutputSection = &BC.registerOrUpdateSection(
      ".findfunctab", ELF::SHT_PROGBITS, SectionFlags, nullptr, ~0ULL,
      sizeof(uint64_t));
  // runtime/symtab.go
  struct {
    uint32_t Idx;
    uint8_t Subbuckets[16];
  } Findfuncbucket;

  // NOTE Currently we don't know how much BFs occupy in text section.
  // We will allocate 4 times more then original size using mmap.
  const uint64_t SectionMaxSize =
      alignTo(FindfunctabSym->getSize(), BC.RegularPageSize) * 4;
  uint8_t *const SectionData = new uint8_t[SectionMaxSize];
  if (!SectionData) {
    errs() << "BOLT-ERROR: failed to allocate new .findfunctab section\n";
    return -1;
  }

  uint32_t LastIdx = 0, SubIndex = 0, Index = 0;
  uint64_t Offset = getNewTextStart(BC),
           NextOffset = Offset; // NOTE For align calc
  uint8_t *Data = SectionData;
  std::vector<BinaryFunction *> BFs = getSortedGolangFunctions(BC);
  for (auto BFit = BFs.begin(); BFit != BFs.end(); ++BFit) {
    assert((uint64_t)(Data - SectionData) < SectionMaxSize && "Overflow error");
    BinaryFunction *BF = *BFit;
    uint64_t Size = getBFSize(BF);
    Offset += Size;
    auto BFNext = std::next(BFit, 1);
    if (BFNext != BFs.end()) {
      if (BC.HasRelocations)
        Offset = alignTo(Offset, BinaryFunction::MinAlign);

      unsigned Alignment, MaxAlignment;
      std::tie(Alignment, MaxAlignment) =
          BC.getBFAlignment(**BFNext, /*EmitColdPart*/ false);
      uint64_t Pad = offsetToAlignment(Offset, llvm::Align(Alignment));
      if (Pad <= MaxAlignment)
        Offset += Pad;

      // Cold part start, align section
      if (BF->getIndex() < INVALID_BF_INDEX &&
          (*BFNext)->getIndex() == INVALID_BF_INDEX)
        Offset = alignTo(Offset, opts::AlignFunctions);
    }

    // Offset points to the next BF
    // NextOffset points to the pcsubbucketssize aligned address somewhere
    // in the current BF
    if (Offset <= NextOffset) {
      ++Index;
      continue;
    }

    // We are interested in the part of the function starting from NextOffset
    Size = Offset - NextOffset;
    do {
      if (SubIndex % sizeof(Findfuncbucket.Subbuckets) == 0) {
        writeEndianVal(BC, Index, sizeof(Findfuncbucket.Idx), &Data);
        LastIdx = Index;
        SubIndex = 0;
      }

      *Data++ = Index - LastIdx;
      ++SubIndex;
      Size -= pcsubbucketssize;
    } while ((int64_t)Size > 0);

    NextOffset = alignTo(Offset, pcsubbucketssize);
    ++Index;
  }

  SectionSize = Data - SectionData;
  OutputSection->updateContents(SectionData, SectionSize);
  FindfunctabSym->setOutputSize(SectionSize);
  FindfunctabSym->setOutputLocation(*OutputSection, 0);

  // NOTE To be able to emit new data we need to have at least one relocation
  // for OutputSection to be created. Since the first 4 bytes of the findfunctab
  // is always 0 create dummy 4 bytes abs relocation there
  MCSymbol *ZeroSym = BC.registerNameAtAddress("Zero", 0, 0, 0);
  OutputSection->addRelocation(0, ZeroSym,
                               Relocation::getAbs(sizeof(uint32_t)));
  OutputSection->setIsFinalized();
  return 0;
}

int GolangPass::getSymbols(BinaryContext &BC) {
  // The iface/eface ifn/tfn addresses are set relative to this symbol
  BinaryData *TextSymbol = BC.getFirstBinaryDataByName(getFirstBFName());
  if (!TextSymbol) {
    outs() << "BOLT-WARNING: failed to get text start symbol!\n";
    return -1;
  }

  RuntimeText = TextSymbol->getAddress();
  return 0;
}

int GolangPass::checkGoVersion(BinaryContext &BC) {
  auto failed = [&](void) -> int {
    outs() << "BOLT-WARNING: could not idetifiy Go version for input binary!\n";
    if (opts::GolangPass != opts::GV_AUTO)
      return 0;

    outs() << "BOLT-ERROR: no compatible version found! Specify gc version "
              "explicitly\n";
    return -1;
  };

  BinaryData *BuildVersion =
      BC.getFirstBinaryDataByName("runtime.buildVersion");
  if (!BuildVersion)
    return failed();

  BinarySection *Section = &BuildVersion->getSection();
  DataExtractor DE =
      DataExtractor(Section->getContents(), BC.AsmInfo->isLittleEndian(),
                    BC.AsmInfo->getCodePointerSize());

  uint64_t GoVersionOffset = BuildVersion->getAddress() - Section->getAddress();
  uint64_t GoVersionAddr = DE.getAddress(&GoVersionOffset);
  ErrorOr<BinarySection &> GoVersionSection =
      BC.getSectionForAddress(GoVersionAddr);
  if (!GoVersionSection) {
    errs()
        << "BOLT-ERROR: failed to get binary section for go version string\n";
    return failed();
  }

  const char *BinaryVersion = GoVersionSection->getContents().data();
  BinaryVersion += (GoVersionAddr - GoVersionSection->getAddress());

  const unsigned MaxVerLen = 9;
  if (opts::GolangPass != opts::GV_AUTO) {
    const char *ExpectedVersion = GolangStringVer[opts::GolangPass];
    if (memcmp(BinaryVersion, ExpectedVersion, strlen(ExpectedVersion))) {
      // NOTE Limit expected version string to 9 chars
      outs() << "BOLT-WARNING: the binary expected version is "
             << ExpectedVersion
             << " but found: " << std::string(BinaryVersion, MaxVerLen) << "\n";
      return 0;
    }
  } else {
    for (int I = opts::GV_LATEST; I > opts::GV_FIRST; --I) {
      if (!memcmp(BinaryVersion, GolangStringVer[I],
                  strlen(GolangStringVer[I]))) {
        outs() << "BOLT-INFO: golang version is: " << GolangStringVer[I]
               << "\n";
        opts::GolangPass = (opts::GolangVersion)I;
        return 0;
      }
    }

    outs() << "BOLT-INFO: the binary version is: "
           << std::string(BinaryVersion, MaxVerLen) << "\n";
    return failed();
  }

  return 0;
}

void GolangPass::runOnFunctions(BinaryContext &BC) {
  int Ret;

#define CALL_STAGE(func)                                                       \
  Ret = func(BC);                                                              \
  if (Ret < 0) {                                                               \
    errs() << "BOLT-ERROR: golang " << #func << " stage failed!\n";            \
    exit(1);                                                                   \
  }

  CALL_STAGE(updateBBSizes);

  CALL_STAGE(typelinksPass);

  CALL_STAGE(pclntabPass);

  CALL_STAGE(findFuncTabPass);

  CALL_STAGE(textsectmapPass);

  CALL_STAGE(FirstModule->patch);

#undef CALL_STAGE
}

} // end namespace bolt
} // end namespace llvm
