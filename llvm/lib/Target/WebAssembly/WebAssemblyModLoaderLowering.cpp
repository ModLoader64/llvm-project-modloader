#include "WebAssemblyModLoaderLowering.h"
#include "Utils/WasmAddressSpaces.h"
#include "WebAssembly.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ReplaceConstant.h"
#include "llvm/Pass.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/TargetParser/ModLoaderAddressSpaces.h"
#include "llvm/Transforms/Scalar/InferAddressSpaces.h"
#include "llvm/Transforms/Scalar/SROA.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include <map>

using namespace llvm;

#define DEBUG_TYPE "wasm-modloader-lowering"

namespace {

using ModLoader::Codec;
using ModLoader::isGuestAddressSpace;

struct GuestSpace {
  unsigned AddressSpace = 0;
  std::string Name;
  Codec Kind = Codec::LittleEndian;
  uint64_t Mask = 0;
  bool TrackDirty = false;
  GlobalVariable *WindowBase = nullptr;
  GlobalVariable *WindowSize = nullptr;
  GlobalVariable *DirtyMap = nullptr;
};

struct Region {
  unsigned FlatSpace;
  unsigned TargetSpace;
  uint64_t From;
  uint64_t To;
};

class ModLoaderLowering {
public:
  bool run(Module &Mod);

private:
  Module *M = nullptr;
  const DataLayout *DL = nullptr;
  LLVMContext *Ctx = nullptr;
  std::map<unsigned, GuestSpace> Spaces;
  SmallVector<Region, 8> Regions;

  bool parseMetadata();
  bool usesGuestSpaces() const;
  GuestSpace *getSpace(unsigned AS);
  void error(const Twine &Message);

  GlobalVariable *getExportedGlobal(StringRef Name, Type *Ty);
  GlobalVariable *getWindowBase(GuestSpace &S);
  GlobalVariable *getWindowSize(GuestSpace &S);
  GlobalVariable *getDirtyMap(GuestSpace &S);
  FunctionCallee getFlatImport(bool Store);

  void lowerSymbols();
  void lowerFunction(Function &F);
  void lowerLoad(LoadInst *LI);
  void lowerStore(StoreInst *SI);
  void lowerMemIntrinsic(MemIntrinsic *MI);
  void lowerSpaceCast(CallInst *CI);

  Value *getOffset(IRBuilder<> &B, GuestSpace &S, Value *Pointer);
  Value *loadInvariant(IRBuilder<> &B, GlobalVariable *G);
  void emitBoundsCheck(Instruction *InsertBefore, GuestSpace &S, Value *Offset,
                       Value *Size);
  Value *hostAddress(IRBuilder<> &B, GuestSpace &S, Value *Offset);
  Value *emitSpaceLoad(Instruction *InsertBefore, GuestSpace &S, Value *Offset,
                       Type *Ty, Align Alignment, bool Volatile);
  void emitSpaceStore(Instruction *InsertBefore, GuestSpace &S, Value *Offset,
                      Value *Val, Align Alignment, bool Volatile);
  Value *emitFlatAccess(Instruction *InsertBefore, GuestSpace &Flat,
                        Value *Address, Type *Ty, Value *StoredValue,
                        Align Alignment, bool Volatile);
  void markDirty(IRBuilder<> &B, GuestSpace &S, Value *Offset, uint64_t Size,
                 Align Alignment);

  IntegerType *getAddressType(unsigned AS);
  IntegerType *getAddressType(Value *Pointer);
  IntegerType *getIntType(Type *Ty);
  Value *toInt(IRBuilder<> &B, Value *V);
  Value *fromInt(IRBuilder<> &B, Value *V, Type *Ty);
};

bool isGuestPointer(const Value *V) {
  auto *PT = dyn_cast<PointerType>(V->getType());
  return PT && isGuestAddressSpace(PT->getAddressSpace());
}

} // end anonymous namespace

void ModLoaderLowering::error(const Twine &Message) {
  Ctx->emitError("ModLoader: " + Message);
}

bool ModLoaderLowering::parseMetadata() {
  NamedMDNode *SpacesMD = M->getNamedMetadata("modloader.spaces");
  if (!SpacesMD)
    return false;

  for (const MDNode *Node : SpacesMD->operands()) {
    if (Node->getNumOperands() != 5)
      continue;

    auto *AS = mdconst::dyn_extract<ConstantInt>(Node->getOperand(0));
    auto *Name = dyn_cast<MDString>(Node->getOperand(1));
    auto *CodecName = dyn_cast<MDString>(Node->getOperand(2));
    auto *Mask = mdconst::dyn_extract<ConstantInt>(Node->getOperand(3));
    auto *Dirty = mdconst::dyn_extract<ConstantInt>(Node->getOperand(4));
    if (!AS || !Name || !CodecName || !Mask || !Dirty)
      continue;

    GuestSpace S;
    S.AddressSpace = AS->getZExtValue();
    S.Name = Name->getString().str();
    if (auto Kind = ModLoader::parseCodec(CodecName->getString()))
      S.Kind = *Kind;
    else
      error("unknown codec '" + CodecName->getString() + "' for space " +
            S.Name);
    S.Mask = Mask->getZExtValue();
    S.TrackDirty = !Dirty->isZero();
    Spaces[S.AddressSpace] = S;
  }

  if (NamedMDNode *RegionsMD = M->getNamedMetadata("modloader.regions")) {
    for (const MDNode *Node : RegionsMD->operands()) {
      if (Node->getNumOperands() != 4)
        continue;

      auto *Flat = mdconst::dyn_extract<ConstantInt>(Node->getOperand(0));
      auto *Target = mdconst::dyn_extract<ConstantInt>(Node->getOperand(1));
      auto *From = mdconst::dyn_extract<ConstantInt>(Node->getOperand(2));
      auto *To = mdconst::dyn_extract<ConstantInt>(Node->getOperand(3));
      if (!Flat || !Target || !From || !To)
        continue;

      Regions.push_back({unsigned(Flat->getZExtValue()),
                         unsigned(Target->getZExtValue()), From->getZExtValue(),
                         To->getZExtValue()});
    }
  }

  return true;
}

bool ModLoaderLowering::usesGuestSpaces() const {
  for (const GlobalVariable &G : M->globals())
    if (isGuestAddressSpace(G.getAddressSpace()))
      return true;

  for (const Function &F : *M) {
    if (F.getName().starts_with("__modloader_space_cast."))
      return true;

    for (const Instruction &I : instructions(F)) {
      if (auto *LI = dyn_cast<LoadInst>(&I))
        if (isGuestPointer(LI->getPointerOperand()))
          return true;
      if (auto *SI = dyn_cast<StoreInst>(&I))
        if (isGuestPointer(SI->getPointerOperand()))
          return true;
      if (auto *MI = dyn_cast<MemIntrinsic>(&I))
        if (isGuestPointer(MI->getRawDest()) ||
            (isa<MemTransferInst>(MI) &&
             isGuestPointer(cast<MemTransferInst>(MI)->getRawSource())))
          return true;
    }
  }

  return false;
}

GuestSpace *ModLoaderLowering::getSpace(unsigned AS) {
  auto It = Spaces.find(AS);
  if (It == Spaces.end()) {
    error("address space " + Twine(AS) +
          " is used but no `#pragma modloader space` declares it");
    return nullptr;
  }

  return &It->second;
}

GlobalVariable *ModLoaderLowering::getExportedGlobal(StringRef Name, Type *Ty) {
  if (GlobalVariable *G = M->getNamedGlobal(Name))
    return G;

  auto *G = new GlobalVariable(
      *M, Ty, /*isConstant=*/false, GlobalValue::LinkOnceODRLinkage,
      Constant::getNullValue(Ty), Name, nullptr, GlobalVariable::NotThreadLocal,
      WebAssembly::WASM_ADDRESS_SPACE_VAR);
  G->addAttribute("wasm-export-name", Name);
  return G;
}

GlobalVariable *ModLoaderLowering::getWindowBase(GuestSpace &S) {
  if (!S.WindowBase)
    S.WindowBase = getExportedGlobal("modloader.window." + S.Name + ".base",
                                     Type::getInt64Ty(*Ctx));

  return S.WindowBase;
}

GlobalVariable *ModLoaderLowering::getWindowSize(GuestSpace &S) {
  if (!S.WindowSize)
    S.WindowSize = getExportedGlobal("modloader.window." + S.Name + ".size",
                                     Type::getInt64Ty(*Ctx));

  return S.WindowSize;
}

GlobalVariable *ModLoaderLowering::getDirtyMap(GuestSpace &S) {
  if (S.DirtyMap)
    return S.DirtyMap;

  std::string MapName = "__modloader_dirty_map." + S.Name;
  uint64_t Pages = (S.Mask >> 12) + 1;
  auto *MapTy = ArrayType::get(Type::getInt8Ty(*Ctx), Pages);
  S.DirtyMap = M->getNamedGlobal(MapName);
  if (!S.DirtyMap) {
    S.DirtyMap = new GlobalVariable(*M, MapTy, /*isConstant=*/false,
                                    GlobalValue::LinkOnceODRLinkage,
                                    ConstantAggregateZero::get(MapTy), MapName);
    S.DirtyMap->setAlignment(Align(16));
  }

  std::string AccessorName = "modloader.dirty." + S.Name;
  if (!M->getFunction(AccessorName)) {
    auto *IntPtrTy = DL->getIntPtrType(*Ctx);
    auto *Accessor =
        Function::Create(FunctionType::get(IntPtrTy, false),
                         GlobalValue::LinkOnceODRLinkage, AccessorName, M);
    Accessor->addFnAttr("wasm-export-name", AccessorName);
    IRBuilder<> B(BasicBlock::Create(*Ctx, "entry", Accessor));
    B.CreateRet(B.CreatePtrToInt(S.DirtyMap, IntPtrTy));
    // The host calls this export.
    appendToUsed(*M, {Accessor});
  }

  return S.DirtyMap;
}

FunctionCallee ModLoaderLowering::getFlatImport(bool Store) {
  auto *I32 = Type::getInt32Ty(*Ctx);
  auto *I64 = Type::getInt64Ty(*Ctx);
  StringRef Name = Store ? "flat_store" : "flat_load";
  SmallVector<Type *, 3> Parameters{I64, I32};
  if (Store)
    Parameters.push_back(I64);

  FunctionCallee Callee = M->getOrInsertFunction(
      ("__modloader_" + Name).str(),
      FunctionType::get(Store ? Type::getVoidTy(*Ctx) : I64, Parameters,
                        false));
  if (auto *F = dyn_cast<Function>(Callee.getCallee())) {
    F->addFnAttr("wasm-import-module", "modloader");
    F->addFnAttr("wasm-import-name", Name);
  }

  return Callee;
}

Value *ModLoaderLowering::loadInvariant(IRBuilder<> &B, GlobalVariable *G) {
  LoadInst *LI = B.CreateLoad(G->getValueType(), G);
  LI->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(*Ctx, {}));
  return LI;
}

IntegerType *ModLoaderLowering::getAddressType(unsigned AS) {
  return IntegerType::get(*Ctx, DL->getPointerSizeInBits(AS));
}

IntegerType *ModLoaderLowering::getAddressType(Value *Pointer) {
  return getAddressType(Pointer->getType()->getPointerAddressSpace());
}

IntegerType *ModLoaderLowering::getIntType(Type *Ty) {
  if (auto *IT = dyn_cast<IntegerType>(Ty)) {
    if (IT->getBitWidth() % 8 || IT->getBitWidth() > 64) {
      error("guest accesses must be 1 to 8 whole bytes wide");
      return nullptr;
    }

    return IT;
  }

  if (Ty->isFloatTy())
    return Type::getInt32Ty(*Ctx);
  if (Ty->isDoubleTy())
    return Type::getInt64Ty(*Ctx);
  if (auto *PT = dyn_cast<PointerType>(Ty)) {
    if (!isGuestAddressSpace(PT->getAddressSpace())) {
      error("a host pointer cannot be stored in guest memory");
      return nullptr;
    }

    return getAddressType(PT->getAddressSpace());
  }

  error("unsupported type in guest memory access");
  return nullptr;
}

Value *ModLoaderLowering::toInt(IRBuilder<> &B, Value *V) {
  Type *Ty = V->getType();
  if (Ty->isIntegerTy())
    return V;
  if (Ty->isPointerTy())
    return B.CreatePtrToInt(V, getAddressType(V));

  return B.CreateBitCast(V, getIntType(Ty));
}

Value *ModLoaderLowering::fromInt(IRBuilder<> &B, Value *V, Type *Ty) {
  if (Ty->isIntegerTy())
    return V;
  if (Ty->isPointerTy())
    return B.CreateIntToPtr(V, Ty);

  return B.CreateBitCast(V, Ty);
}

Value *ModLoaderLowering::getOffset(IRBuilder<> &B, GuestSpace &S,
                                    Value *Pointer) {
  Value *Address = B.CreatePtrToInt(Pointer, getAddressType(Pointer));
  Value *Offset = B.CreateZExtOrBitCast(Address, Type::getInt64Ty(*Ctx));
  return B.CreateAnd(Offset, S.Mask);
}

void ModLoaderLowering::emitBoundsCheck(Instruction *InsertBefore,
                                        GuestSpace &S, Value *Offset,
                                        Value *Size) {
  IRBuilder<> B(InsertBefore);
  Value *End = B.CreateAdd(Offset, Size);
  Value *Limit = loadInvariant(B, getWindowSize(S));
  if (S.Kind == Codec::WordSwapped)
    Limit = B.CreateAnd(Limit, ~uint64_t(3));

  Value *Overflow = B.CreateICmpULT(End, Offset, "modloader.overflow");
  Value *Outside =
      B.CreateOr(Overflow, B.CreateICmpUGT(End, Limit), "modloader.outside");
  Instruction *Then =
      SplitBlockAndInsertIfThen(Outside, InsertBefore, /*Unreachable=*/true);
  IRBuilder<> TrapBuilder(Then);
  TrapBuilder.CreateIntrinsic(Intrinsic::trap, {});
}

Value *ModLoaderLowering::hostAddress(IRBuilder<> &B, GuestSpace &S,
                                      Value *Offset) {
  Value *Base = loadInvariant(B, getWindowBase(S));
  Value *Address = B.CreateAdd(Base, Offset);
  return B.CreateIntToPtr(Address, PointerType::get(*Ctx, 0));
}

void ModLoaderLowering::markDirty(IRBuilder<> &B, GuestSpace &S, Value *Offset,
                                  uint64_t Size, Align Alignment) {
  if (!S.TrackDirty)
    return;

  GlobalVariable *Map = getDirtyMap(S);
  auto *I8 = Type::getInt8Ty(*Ctx);
  auto MarkPage = [&](Value *At) {
    Value *Page = B.CreateLShr(At, 12);
    Value *Entry = B.CreateInBoundsGEP(I8, Map, Page);
    B.CreateStore(ConstantInt::get(I8, 1), Entry);
  };

  MarkPage(Offset);
  if (Size > 1 && Alignment.value() < Size)
    MarkPage(
        B.CreateAdd(Offset, ConstantInt::get(Offset->getType(), Size - 1)));
}

Value *ModLoaderLowering::emitSpaceLoad(Instruction *InsertBefore,
                                        GuestSpace &S, Value *Offset, Type *Ty,
                                        Align Alignment, bool Volatile) {
  IntegerType *IntTy = getIntType(Ty);
  if (!IntTy)
    return PoisonValue::get(Ty);

  uint64_t Size = IntTy->getBitWidth() / 8;
  auto *I64 = Type::getInt64Ty(*Ctx);
  emitBoundsCheck(InsertBefore, S, Offset, ConstantInt::get(I64, Size));
  IRBuilder<> B(InsertBefore);
  auto *I8 = Type::getInt8Ty(*Ctx);
  auto *I32 = Type::getInt32Ty(*Ctx);
  Value *Result = nullptr;

  auto LoadAt = [&](Value *At, Type *LoadTy, Align A) -> Value * {
    LoadInst *LI = B.CreateAlignedLoad(LoadTy, hostAddress(B, S, At), A);
    LI->setVolatile(Volatile);
    return LI;
  };
  bool OddWidth = (Size & (Size - 1)) != 0;
  auto LoadBytes = [&](bool BigEndianOrder, uint64_t Swizzle) -> Value * {
    Value *Assembled = ConstantInt::get(IntTy, 0);
    for (uint64_t Index = 0; Index < Size; ++Index) {
      Value *At = B.CreateAdd(Offset, ConstantInt::get(I64, Index));
      if (Swizzle != 0)
        At = B.CreateXor(At, Swizzle);

      Value *Byte = B.CreateZExt(LoadAt(At, I8, Align(1)), IntTy);
      uint64_t Shift = BigEndianOrder ? 8 * (Size - 1 - Index) : 8 * Index;
      Assembled = B.CreateOr(Assembled, B.CreateShl(Byte, Shift));
    }

    return Assembled;
  };

  switch (S.Kind) {
  case Codec::LittleEndian:
    Result = OddWidth ? LoadBytes(false, 0) : LoadAt(Offset, IntTy, Alignment);
    break;
  case Codec::BigEndian:
    if (OddWidth) {
      Result = LoadBytes(true, 0);
      break;
    }

    Result = LoadAt(Offset, IntTy, Alignment);
    if (Size > 1)
      Result = B.CreateUnaryIntrinsic(Intrinsic::bswap, Result);
    break;
  case Codec::WordSwapped:
    if (Size == 1) {
      Result = LoadAt(B.CreateXor(Offset, 3), I8, Align(1));
    } else if (Size == 2 && Alignment >= Align(2)) {
      Result = LoadAt(B.CreateXor(Offset, 2), IntTy, Align(2));
    } else if (Size == 4 && Alignment >= Align(4)) {
      Result = LoadAt(Offset, IntTy, Align(4));
    } else if (Size == 8 && Alignment >= Align(4)) {
      Value *High = B.CreateZExt(LoadAt(Offset, I32, Align(4)), I64);
      Value *Low = B.CreateZExt(
          LoadAt(B.CreateAdd(Offset, ConstantInt::get(I64, 4)), I32, Align(4)),
          I64);
      Result = B.CreateOr(B.CreateShl(High, 32), Low);
    } else {
      Result = LoadBytes(true, 3);
    }
    break;
  case Codec::Flat:
    llvm_unreachable("flat spaces dispatch before this point");
  }

  return fromInt(B, Result, Ty);
}

void ModLoaderLowering::emitSpaceStore(Instruction *InsertBefore, GuestSpace &S,
                                       Value *Offset, Value *Val,
                                       Align Alignment, bool Volatile) {
  IntegerType *IntTy = getIntType(Val->getType());
  if (!IntTy)
    return;

  uint64_t Size = IntTy->getBitWidth() / 8;
  auto *I64 = Type::getInt64Ty(*Ctx);
  emitBoundsCheck(InsertBefore, S, Offset, ConstantInt::get(I64, Size));
  IRBuilder<> B(InsertBefore);
  auto *I8 = Type::getInt8Ty(*Ctx);
  auto *I32 = Type::getInt32Ty(*Ctx);
  Value *IntVal = toInt(B, Val);

  auto StoreAt = [&](Value *At, Value *V, Align A) {
    StoreInst *SI = B.CreateAlignedStore(V, hostAddress(B, S, At), A);
    SI->setVolatile(Volatile);
  };
  bool OddWidth = (Size & (Size - 1)) != 0;
  auto StoreBytes = [&](bool BigEndianOrder, uint64_t Swizzle) {
    for (uint64_t Index = 0; Index < Size; ++Index) {
      Value *At = B.CreateAdd(Offset, ConstantInt::get(I64, Index));
      if (Swizzle != 0)
        At = B.CreateXor(At, Swizzle);

      uint64_t Shift = BigEndianOrder ? 8 * (Size - 1 - Index) : 8 * Index;
      StoreAt(At, B.CreateTrunc(B.CreateLShr(IntVal, Shift), I8), Align(1));
    }
  };

  switch (S.Kind) {
  case Codec::LittleEndian:
    if (OddWidth)
      StoreBytes(false, 0);
    else
      StoreAt(Offset, IntVal, Alignment);
    break;
  case Codec::BigEndian:
    if (OddWidth)
      StoreBytes(true, 0);
    else
      StoreAt(Offset,
              Size > 1 ? B.CreateUnaryIntrinsic(Intrinsic::bswap, IntVal)
                       : IntVal,
              Alignment);
    break;
  case Codec::WordSwapped:
    if (Size == 1) {
      StoreAt(B.CreateXor(Offset, 3), IntVal, Align(1));
    } else if (Size == 2 && Alignment >= Align(2)) {
      StoreAt(B.CreateXor(Offset, 2), IntVal, Align(2));
    } else if (Size == 4 && Alignment >= Align(4)) {
      StoreAt(Offset, IntVal, Align(4));
    } else if (Size == 8 && Alignment >= Align(4)) {
      StoreAt(Offset, B.CreateTrunc(B.CreateLShr(IntVal, 32), I32), Align(4));
      StoreAt(B.CreateAdd(Offset, ConstantInt::get(I64, 4)),
              B.CreateTrunc(IntVal, I32), Align(4));
    } else {
      StoreBytes(true, 3);
    }
    break;
  case Codec::Flat:
    llvm_unreachable("flat spaces dispatch before this point");
  }

  markDirty(B, S, Offset, Size, Alignment);
}

Value *ModLoaderLowering::emitFlatAccess(Instruction *InsertBefore,
                                         GuestSpace &Flat, Value *Address,
                                         Type *Ty, Value *StoredValue,
                                         Align Alignment, bool Volatile) {
  bool Store = StoredValue != nullptr;
  IntegerType *IntTy = getIntType(Ty);
  if (!IntTy)
    return Store ? nullptr : PoisonValue::get(Ty);

  uint64_t Size = IntTy->getBitWidth() / 8;
  auto *I32 = Type::getInt32Ty(*Ctx);
  auto *I64 = Type::getInt64Ty(*Ctx);

  BasicBlock *Head = InsertBefore->getParent();
  BasicBlock *Join = Head->splitBasicBlock(InsertBefore, "modloader.flat.join");
  Head->getTerminator()->eraseFromParent();
  IRBuilder<> JoinBuilder(&*Join->begin());
  PHINode *Phi =
      Store ? nullptr
            : JoinBuilder.CreatePHI(Ty, Regions.size() + 1, "modloader.flat");

  BasicBlock *Check = Head;
  for (const Region &R : Regions) {
    if (R.FlatSpace != Flat.AddressSpace || R.To - R.From < Size)
      continue;

    GuestSpace *Target = getSpace(R.TargetSpace);
    if (!Target || Target->Kind == Codec::Flat)
      continue;

    IRBuilder<> B(Check);
    Value *Relative =
        B.CreateSub(Address, ConstantInt::get(Address->getType(), R.From));
    Value *Inside = B.CreateICmpULE(
        Relative, ConstantInt::get(Address->getType(), R.To - R.From - Size));
    BasicBlock *Hit = BasicBlock::Create(*Ctx, "modloader.flat." + Target->Name,
                                         Head->getParent(), Join);
    BasicBlock *Next = BasicBlock::Create(*Ctx, "modloader.flat.next",
                                          Head->getParent(), Join);
    B.CreateCondBr(Inside, Hit, Next);
    Instruction *Branch = UncondBrInst::Create(Join, Hit);
    IRBuilder<> HitBuilder(Branch);
    Value *Offset = HitBuilder.CreateZExtOrBitCast(Relative, I64);
    Align RegionAlignment = commonAlignment(Alignment, R.From);
    if (Store) {
      emitSpaceStore(Branch, *Target, Offset, StoredValue, RegionAlignment,
                     Volatile);
    } else {
      Value *V =
          emitSpaceLoad(Branch, *Target, Offset, Ty, RegionAlignment, Volatile);
      Phi->addIncoming(V, Branch->getParent());
    }
    Check = Next;
  }

  IRBuilder<> B(Check);
  SmallVector<Value *, 3> Arguments{B.CreateZExtOrBitCast(Address, I64),
                                    ConstantInt::get(I32, Size)};
  if (Store)
    Arguments.push_back(B.CreateZExtOrBitCast(toInt(B, StoredValue), I64));

  Value *Raw = B.CreateCall(getFlatImport(Store), Arguments);
  if (!Store) {
    Value *Narrow = B.CreateTruncOrBitCast(Raw, IntTy);
    Phi->addIncoming(fromInt(B, Narrow, Ty), Check);
  }

  B.CreateBr(Join);
  return Phi;
}

void ModLoaderLowering::lowerLoad(LoadInst *LI) {
  unsigned AS = LI->getPointerAddressSpace();
  GuestSpace *S = getSpace(AS);
  if (!S)
    return;

  if (LI->isAtomic()) {
    error("atomic operations on guest memory are not supported");
    return;
  }

  IRBuilder<> B(LI);
  Value *Result;
  if (S->Kind == Codec::Flat) {
    Value *Address = B.CreatePtrToInt(LI->getPointerOperand(),
                                      getAddressType(LI->getPointerOperand()));
    Result = emitFlatAccess(LI, *S, Address, LI->getType(), nullptr,
                            LI->getAlign(), LI->isVolatile());
  } else {
    Value *Offset = getOffset(B, *S, LI->getPointerOperand());
    Result = emitSpaceLoad(LI, *S, Offset, LI->getType(), LI->getAlign(),
                           LI->isVolatile());
  }

  LI->replaceAllUsesWith(Result);
  LI->eraseFromParent();
}

void ModLoaderLowering::lowerStore(StoreInst *SI) {
  unsigned AS = SI->getPointerAddressSpace();
  GuestSpace *S = getSpace(AS);
  if (!S)
    return;

  if (SI->isAtomic()) {
    error("atomic operations on guest memory are not supported");
    return;
  }

  IRBuilder<> B(SI);
  if (S->Kind == Codec::Flat) {
    Value *Address = B.CreatePtrToInt(SI->getPointerOperand(),
                                      getAddressType(SI->getPointerOperand()));
    emitFlatAccess(SI, *S, Address, SI->getValueOperand()->getType(),
                   SI->getValueOperand(), SI->getAlign(), SI->isVolatile());
  } else {
    Value *Offset = getOffset(B, *S, SI->getPointerOperand());
    emitSpaceStore(SI, *S, Offset, SI->getValueOperand(), SI->getAlign(),
                   SI->isVolatile());
  }

  SI->eraseFromParent();
}

void ModLoaderLowering::lowerMemIntrinsic(MemIntrinsic *MI) {
  Function *F = MI->getFunction();
  auto *I8 = Type::getInt8Ty(*Ctx);
  auto *I64 = Type::getInt64Ty(*Ctx);
  IRBuilder<> B(MI);
  Value *Length = B.CreateZExtOrTrunc(MI->getLength(), I64);

  BasicBlock *Head = MI->getParent();
  BasicBlock *Exit = Head->splitBasicBlock(MI, "modloader.mem.exit");
  Head->getTerminator()->eraseFromParent();
  BasicBlock *Body = BasicBlock::Create(*Ctx, "modloader.mem.body", F, Exit);
  IRBuilder<> HeadBuilder(Head);
  HeadBuilder.CreateCondBr(
      HeadBuilder.CreateICmpEQ(Length, ConstantInt::get(I64, 0)), Exit, Body);

  IRBuilder<> BodyBuilder(Body);
  PHINode *Index = BodyBuilder.CreatePHI(I64, 2, "modloader.mem.index");
  Index->addIncoming(ConstantInt::get(I64, 0), Head);
  Value *Next = BodyBuilder.CreateAdd(Index, ConstantInt::get(I64, 1));
  Value *Done = BodyBuilder.CreateICmpEQ(Next, Length);
  Instruction *Latch = BodyBuilder.CreateCondBr(Done, Exit, Body);
  Index->addIncoming(Next, Body);

  auto ByteAt = [&](Value *Pointer) -> std::pair<GuestSpace *, Value *> {
    IRBuilder<> LB(Latch);
    unsigned AS = Pointer->getType()->getPointerAddressSpace();
    if (!isGuestAddressSpace(AS))
      return {nullptr, LB.CreateInBoundsGEP(I8, Pointer, Index)};

    GuestSpace *S = getSpace(AS);
    if (!S)
      return {nullptr, nullptr};

    IntegerType *AddressTy = getAddressType(AS);
    Value *Address = LB.CreateAdd(LB.CreatePtrToInt(Pointer, AddressTy),
                                  LB.CreateZExtOrTrunc(Index, AddressTy));
    if (S->Kind == Codec::Flat)
      return {S, Address};

    return {S, LB.CreateAnd(LB.CreateZExtOrBitCast(Address, I64), S->Mask)};
  };
  auto LoadByte = [&](Value *Pointer) -> Value * {
    auto [S, At] = ByteAt(Pointer);
    if (!At)
      return PoisonValue::get(I8);

    if (!S) {
      IRBuilder<> LB(Latch);
      return LB.CreateLoad(I8, At);
    }

    if (S->Kind == Codec::Flat)
      return emitFlatAccess(Latch, *S, At, I8, nullptr, Align(1),
                            MI->isVolatile());

    return emitSpaceLoad(Latch, *S, At, I8, Align(1), MI->isVolatile());
  };
  auto StoreByte = [&](Value *Pointer, Value *V) {
    auto [S, At] = ByteAt(Pointer);
    if (!At)
      return;

    if (!S) {
      IRBuilder<> LB(Latch);
      LB.CreateStore(V, At);
      return;
    }

    if (S->Kind == Codec::Flat)
      emitFlatAccess(Latch, *S, At, I8, V, Align(1), MI->isVolatile());
    else
      emitSpaceStore(Latch, *S, At, V, Align(1), MI->isVolatile());
  };

  if (auto *Transfer = dyn_cast<MemTransferInst>(MI)) {
    Value *Byte = LoadByte(Transfer->getRawSource());
    StoreByte(Transfer->getRawDest(), Byte);
  } else if (auto *Set = dyn_cast<MemSetInst>(MI)) {
    StoreByte(Set->getRawDest(), Set->getValue());
  }

  Index->setIncomingBlock(1, Latch->getParent());
  MI->eraseFromParent();
}

void ModLoaderLowering::lowerSpaceCast(CallInst *CI) {
  // __modloader_space_cast.<AS>(ptr addrspace(Flat) P, i1 IsReference)
  Value *Pointer = CI->getArgOperand(0);
  bool IsReference = !cast<ConstantInt>(CI->getArgOperand(1))->isZero();
  auto *ResultTy = cast<PointerType>(CI->getType());
  unsigned From = Pointer->getType()->getPointerAddressSpace();
  unsigned To = ResultTy->getAddressSpace();
  IRBuilder<> B(CI);
  IntegerType *AddressTy = getAddressType(Pointer);
  Value *Address = B.CreatePtrToInt(Pointer, AddressTy);
  Value *Inside = B.getFalse();
  for (const Region &R : Regions) {
    if (R.FlatSpace != From || R.TargetSpace != To)
      continue;

    Value *Relative = B.CreateSub(Address, ConstantInt::get(AddressTy, R.From));
    Inside = B.CreateOr(
        Inside, B.CreateICmpULE(
                    Relative, ConstantInt::get(AddressTy, R.To - R.From - 1)));
  }

  Value *Cast = B.CreateIntToPtr(Address, ResultTy);
  Value *Result;
  if (IsReference) {
    Instruction *Then = SplitBlockAndInsertIfThen(B.CreateNot(Inside), CI,
                                                  /*Unreachable=*/true);
    IRBuilder<> TrapBuilder(Then);
    TrapBuilder.CreateIntrinsic(Intrinsic::trap, {});
    Result = Cast;
  } else {
    Result = B.CreateSelect(Inside, Cast, ConstantPointerNull::get(ResultTy));
  }

  CI->replaceAllUsesWith(Result);
  CI->eraseFromParent();
}

void ModLoaderLowering::lowerSymbols() {
  SmallVector<GlobalVariable *, 8> Symbols;
  for (GlobalVariable &G : M->globals())
    if (isGuestAddressSpace(G.getAddressSpace()))
      Symbols.push_back(&G);

  for (GlobalVariable *G : Symbols) {
    if (!G->isDeclaration()) {
      error("guest object '" + G->getName() +
            "' is defined; guest objects can only be declared extern");
      continue;
    }

    StringRef Symbol = G->getName();
    if (MDNode *Node = G->getMetadata("modloader.symbol"))
      if (Node->getNumOperands() == 1)
        if (auto *Name = dyn_cast<MDString>(Node->getOperand(0)))
          Symbol = Name->getString();
    GlobalVariable *Slot =
        getExportedGlobal(("modloader.sym." + Symbol).str(),
                          getAddressType(G->getAddressSpace()));

    convertUsersOfConstantsToInstructions({G});
    SmallVector<Use *, 8> Uses;
    for (Use &U : G->uses())
      Uses.push_back(&U);

    for (Use *U : Uses) {
      auto *User = dyn_cast<Instruction>(U->getUser());
      if (!User) {
        error(
            "guest symbol '" + Symbol +
            "' is used in a static initializer; take its address at run time");
        continue;
      }

      Instruction *InsertBefore = User;
      if (auto *Phi = dyn_cast<PHINode>(User))
        InsertBefore = Phi->getIncomingBlock(*U)->getTerminator();
      IRBuilder<> B(InsertBefore);
      Value *Address = loadInvariant(B, Slot);
      U->set(B.CreateIntToPtr(Address, G->getType()));
    }

    if (G->use_empty())
      G->eraseFromParent();
  }
}

void ModLoaderLowering::lowerFunction(Function &F) {
  SmallVector<Instruction *, 32> Work;
  for (Instruction &I : instructions(F)) {
    if (auto *LI = dyn_cast<LoadInst>(&I)) {
      if (isGuestPointer(LI->getPointerOperand()))
        Work.push_back(LI);
    } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
      if (isGuestPointer(SI->getPointerOperand()))
        Work.push_back(SI);
    } else if (auto *MI = dyn_cast<MemIntrinsic>(&I)) {
      bool Guest = isGuestPointer(MI->getRawDest());
      if (auto *Transfer = dyn_cast<MemTransferInst>(MI))
        Guest |= isGuestPointer(Transfer->getRawSource());
      if (Guest)
        Work.push_back(MI);
    } else if (auto *CI = dyn_cast<CallInst>(&I)) {
      if (Function *Callee = CI->getCalledFunction()) {
        if (Callee->getName().starts_with("__modloader_space_cast."))
          Work.push_back(CI);
        else if (Callee->isIntrinsic() && CI->arg_size() > 0) {
          Intrinsic::ID ID = Callee->getIntrinsicID();
          bool Marker = ID == Intrinsic::lifetime_start ||
                        ID == Intrinsic::lifetime_end ||
                        ID == Intrinsic::invariant_start ||
                        ID == Intrinsic::invariant_end;
          if (!Marker)
            continue;

          if (llvm::any_of(CI->args(), isGuestPointer))
            Work.push_back(CI);
        }
      }
    } else if (isa<AtomicRMWInst>(&I) || isa<AtomicCmpXchgInst>(&I)) {
      Value *Pointer = isa<AtomicRMWInst>(&I)
                           ? cast<AtomicRMWInst>(&I)->getPointerOperand()
                           : cast<AtomicCmpXchgInst>(&I)->getPointerOperand();
      if (isGuestPointer(Pointer))
        error("atomic operations on guest memory are not supported");
    }
  }

  for (Instruction *I : Work) {
    if (auto *LI = dyn_cast<LoadInst>(I))
      lowerLoad(LI);
    else if (auto *SI = dyn_cast<StoreInst>(I))
      lowerStore(SI);
    else if (auto *MI = dyn_cast<MemIntrinsic>(I))
      lowerMemIntrinsic(MI);
    else if (auto *CI = dyn_cast<CallInst>(I)) {
      if (CI->getCalledFunction()->getName().starts_with(
              "__modloader_space_cast."))
        lowerSpaceCast(CI);
      else
        CI->eraseFromParent(); // lifetime and invariant markers
    }
  }
}

bool ModLoaderLowering::run(Module &Mod) {
  M = &Mod;
  DL = &Mod.getDataLayout();
  Ctx = &Mod.getContext();
  if (!parseMetadata()) {
    if (usesGuestSpaces())
      error("guest address spaces are used without `#pragma modloader space`");
    return false;
  }

  lowerSymbols();
  for (Function &F : Mod)
    if (!F.isDeclaration())
      lowerFunction(F);

  for (Function &F : make_early_inc_range(Mod))
    if (F.getName().starts_with("__modloader_space_cast.") && F.use_empty())
      F.eraseFromParent();

  return true;
}

PreservedAnalyses
WebAssemblyModLoaderLoweringPass::run(Module &M, ModuleAnalysisManager &AM) {
  if (OptimizePointers && M.getNamedMetadata("modloader.spaces")) {
    FunctionPassManager FPM;
    FPM.addPass(SROAPass(SROAOptions::ModifyCFG));
    for (unsigned Width : {32, 64})
      FPM.addPass(
          InferAddressSpacesPass(ModLoader::getGuestAddressSpace(0, Width)));

    createModuleToFunctionPassAdaptor(std::move(FPM)).run(M, AM);
  }

  ModLoaderLowering Lowering;
  return Lowering.run(M) ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

namespace {

class WebAssemblyModLoaderLoweringLegacy final : public ModulePass {
  StringRef getPassName() const override {
    return "Lower ModLoader guest memory accesses";
  }

  bool runOnModule(Module &M) override {
    ModLoaderLowering Lowering;
    return Lowering.run(M);
  }

public:
  static char ID;
  WebAssemblyModLoaderLoweringLegacy() : ModulePass(ID) {}
};

} // end anonymous namespace

char WebAssemblyModLoaderLoweringLegacy::ID = 0;
INITIALIZE_PASS(WebAssemblyModLoaderLoweringLegacy, DEBUG_TYPE,
                "Lower ModLoader guest memory accesses", false, false)

ModulePass *llvm::createWebAssemblyModLoaderLowering() {
  return new WebAssemblyModLoaderLoweringLegacy();
}

bool llvm::isModLoaderNoopAddrSpaceCast(unsigned SrcAS, unsigned DestAS) {
  return ModLoader::isGuestAddressSpace(SrcAS) &&
         ModLoader::isGuestAddressSpace(DestAS) &&
         ModLoader::getGuestPointerWidth(SrcAS) ==
             ModLoader::getGuestPointerWidth(DestAS);
}

void llvm::registerModLoaderPassBuilderCallbacks(PassBuilder &PB) {
  PB.registerPipelineStartEPCallback(
      [](ModulePassManager &MPM, OptimizationLevel Level) {
        MPM.addPass(WebAssemblyModLoaderLoweringPass(
            /*OptimizePointers=*/Level != OptimizationLevel::O0));
      });
  PB.registerPipelineParsingCallback(
      [](StringRef Name, ModulePassManager &MPM,
         ArrayRef<PassBuilder::PipelineElement>) {
        if (Name == "wasm-modloader-lowering") {
          MPM.addPass(WebAssemblyModLoaderLoweringPass());
          return true;
        }

        return false;
      });
}
