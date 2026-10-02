#include "CGModLoader.h"
#include "CGRecordLayout.h"
#include "CodeGenFunction.h"
#include "CodeGenModule.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ModLoaderSpaces.h"
#include "clang/AST/RecordLayout.h"
#include "llvm/IR/Module.h"

using namespace clang;
using namespace CodeGen;

void CodeGenModule::updateModLoaderDataLayout() const {
  const auto &Spaces = Context.getModLoaderSpaces().spaces();
  if (ModLoaderLayoutSpaceCount == Spaces.size())
    return;

  std::string Layout = TheModule.getDataLayoutStr();
  while (ModLoaderLayoutSpaceCount < Spaces.size()) {
    const auto &Space = Spaces[ModLoaderLayoutSpaceCount++];
    Layout += "-p" +
              std::to_string(llvm::ModLoader::getGuestAddressSpace(
                  Space.Id, Space.Width)) +
              (Space.Width == 32 ? ":32:32" : ":64:64");
  }

  TheModule.setDataLayout(Layout);
}

static void copyObject(CodeGenFunction &CGF, Address Dest, Address Src,
                       QualType Type, SourceLocation Loc, bool IsVolatile) {
  ASTContext &Context = CGF.getContext();
  CGBuilderTy &Builder = CGF.Builder;
  QualType Canonical = Context.getCanonicalType(Type);

  auto AtOffset = [&](Address Base, CharUnits Offset, llvm::Type *MemoryType) {
    return Builder
        .CreateConstInBoundsByteGEP(Base.withElementType(CGF.Int8Ty), Offset)
        .withElementType(MemoryType);
  };
  auto CopyAtOffset = [&](CharUnits Offset, QualType MemberType,
                          SourceLocation MemberLoc) {
    llvm::Type *MemoryType = CGF.ConvertTypeForMem(MemberType);
    copyObject(CGF, AtOffset(Dest, Offset, MemoryType),
               AtOffset(Src, Offset, MemoryType), MemberType, MemberLoc,
               IsVolatile);
  };

  if (const ConstantArrayType *Array =
          Context.getAsConstantArrayType(Canonical)) {
    QualType ElementType = Array->getElementType();
    uint64_t Count = Array->getZExtSize();
    CharUnits ElementSize = Context.getTypeSizeInChars(ElementType);
    if (Count <= 16) {
      for (uint64_t Index = 0; Index < Count; ++Index)
        CopyAtOffset(ElementSize * Index, ElementType, Loc);

      return;
    }

    llvm::Type *ElementLLVMType = CGF.ConvertTypeForMem(ElementType);
    llvm::BasicBlock *Entry = Builder.GetInsertBlock();
    llvm::BasicBlock *Body = CGF.createBasicBlock("modloader.copy.body");
    llvm::BasicBlock *Done = CGF.createBasicBlock("modloader.copy.done");
    CGF.EmitBlock(Body);
    llvm::PHINode *Index =
        Builder.CreatePHI(CGF.SizeTy, 2, "modloader.copy.index");
    Index->addIncoming(llvm::ConstantInt::get(CGF.SizeTy, 0), Entry);

    auto AtIndex = [&](Address Base) {
      return Address(Builder.CreateInBoundsGEP(ElementLLVMType,
                                               Base.emitRawPointer(CGF), Index),
                     ElementLLVMType,
                     Base.getAlignment().alignmentOfArrayElement(ElementSize));
    };
    Address DestElement = AtIndex(Dest);
    Address SrcElement = AtIndex(Src);
    copyObject(CGF, DestElement, SrcElement, ElementType, Loc, IsVolatile);

    llvm::Value *Next =
        Builder.CreateNUWAdd(Index, llvm::ConstantInt::get(CGF.SizeTy, 1));
    Index->addIncoming(Next, Builder.GetInsertBlock());
    Builder.CreateCondBr(
        Builder.CreateICmpEQ(Next, llvm::ConstantInt::get(CGF.SizeTy, Count)),
        Done, Body);
    CGF.EmitBlock(Done);
    return;
  }

  if (const auto *Record = Canonical->getAs<RecordType>()) {
    const RecordDecl *RD = Record->getDecl()->getDefinitionOrSelf();
    if (RD->isUnion()) {
      CGF.CGM.Error(Loc,
                    "copy the active union member explicitly when crossing "
                    "guest address spaces");
      return;
    }

    const ASTRecordLayout &Layout = Context.getASTRecordLayout(RD);
    if (const auto *CXXRD = dyn_cast<CXXRecordDecl>(RD)) {
      if (CXXRD->isDynamicClass()) {
        CGF.CGM.Error(Loc, "a class with virtual functions or virtual bases "
                           "cannot be copied to or from guest memory");
        return;
      }

      for (const CXXBaseSpecifier &Base : CXXRD->bases()) {
        const CXXRecordDecl *BaseDecl = Base.getType()->getAsCXXRecordDecl();
        CharUnits Offset = Layout.getBaseClassOffset(BaseDecl);
        CopyAtOffset(Offset, Base.getType(), Loc);
      }
    }

    std::optional<CharUnits> LastStorage;
    for (const FieldDecl *Field : RD->fields()) {
      if (Field->isUnnamedBitField() || Field->isZeroSize(Context))
        continue;

      if (Field->isBitField()) {
        const CGBitFieldInfo &Info =
            CGF.CGM.getTypes().getCGRecordLayout(RD).getBitFieldInfo(Field);
        if (LastStorage == Info.StorageOffset)
          continue;

        LastStorage = Info.StorageOffset;
        llvm::Type *StorageType =
            llvm::Type::getIntNTy(CGF.getLLVMContext(), Info.StorageSize);
        Address SrcUnit = AtOffset(Src, Info.StorageOffset, StorageType);
        Address DestUnit = AtOffset(Dest, Info.StorageOffset, StorageType);
        Builder.CreateStore(Builder.CreateLoad(SrcUnit, IsVolatile), DestUnit,
                            IsVolatile);
        continue;
      }

      CharUnits Offset = Context.toCharUnitsFromBits(
          Layout.getFieldOffset(Field->getFieldIndex()));
      CopyAtOffset(Offset, Field->getType(), Field->getLocation());
    }

    return;
  }

  if (modloader::isHostOnlyType(Canonical)) {
    CGF.CGM.Error(Loc, "a host pointer or reference cannot be copied to or "
                       "from guest memory");
    return;
  }

  if (Canonical->isAnyComplexType() || Canonical->isVectorType()) {
    CGF.CGM.Error(Loc, "complex and vector values cannot be copied to or from "
                       "guest memory member by member");
    return;
  }

  llvm::Type *MemoryType = CGF.ConvertTypeForMem(Type);
  llvm::Value *Value =
      Builder.CreateLoad(Src.withElementType(MemoryType), IsVolatile);
  Builder.CreateStore(Value, Dest.withElementType(MemoryType), IsVolatile);
}

bool CodeGen::emitModLoaderAggregateCopy(CodeGenFunction &CGF, Address Dest,
                                         Address Src, QualType Type,
                                         bool IsVolatile) {
  unsigned DestAS = Dest.getAddressSpace();
  unsigned SrcAS = Src.getAddressSpace();
  if (DestAS == SrcAS || (!llvm::ModLoader::isGuestAddressSpace(DestAS) &&
                          !llvm::ModLoader::isGuestAddressSpace(SrcAS)))
    return false;

  // Do not expose guest padding
  if (!llvm::ModLoader::isGuestAddressSpace(DestAS))
    CGF.Builder.CreateMemSet(
        Dest, CGF.Builder.getInt8(0),
        CGF.Builder.getInt64(
            CGF.getContext().getTypeSizeInChars(Type).getQuantity()),
        /*IsVolatile=*/false);

  copyObject(CGF, Dest, Src, Type, SourceLocation(), IsVolatile);
  return true;
}

llvm::Value *CodeGen::emitModLoaderRangeCheckedCast(CodeGenFunction &CGF,
                                                    const CastExpr *Cast) {
  if (!isa<CXXAddrspaceCastExpr>(Cast))
    return nullptr;

  LangAS SrcAS =
      Cast->getSubExpr()->getType()->getPointeeType().getAddressSpace();
  LangAS DestAS = Cast->getType()->getPointeeType().getAddressSpace();
  if (!modloader::isGuestAddressSpace(SrcAS) ||
      !modloader::isGuestAddressSpace(DestAS) ||
      modloader::getGuestSpaceId(SrcAS) != modloader::FlatSpaceId ||
      modloader::getGuestSpaceId(DestAS) == modloader::FlatSpaceId)
    return nullptr;

  llvm::Value *Source = CGF.EmitScalarExpr(Cast->getSubExpr());
  llvm::FunctionType *MarkerType = llvm::FunctionType::get(
      CGF.ConvertType(Cast->getType()),
      {Source->getType(), CGF.Builder.getInt1Ty()}, false);
  std::string Name =
      "__modloader_space_cast." + std::to_string(toTargetAddressSpace(DestAS));
  llvm::FunctionCallee Marker = CGF.CGM.CreateRuntimeFunction(MarkerType, Name);
  return CGF.Builder.CreateCall(Marker, {Source, CGF.Builder.getFalse()});
}

void CodeGen::emitModLoaderMetadata(CodeGenModule &CGM) {
  CGM.getDataLayout();
  const modloader::SpaceTable &Table = CGM.getContext().getModLoaderSpaces();
  if (Table.spaces().empty())
    return;

  llvm::LLVMContext &Ctx = CGM.getLLVMContext();
  llvm::Module &M = CGM.getModule();
  auto Int = [&](llvm::Type *Ty, uint64_t Value) -> llvm::Metadata * {
    return llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(Ty, Value));
  };
  llvm::Type *I1 = llvm::Type::getInt1Ty(Ctx);
  llvm::Type *I32 = llvm::Type::getInt32Ty(Ctx);
  llvm::Type *I64 = llvm::Type::getInt64Ty(Ctx);
  llvm::NamedMDNode *Spaces = M.getOrInsertNamedMetadata("modloader.spaces");
  for (const modloader::Space &S : Table.spaces()) {
    llvm::Metadata *Fields[] = {
        Int(I32, Table.getTargetAddressSpace(S.Id)),
        llvm::MDString::get(Ctx, S.Name),
        llvm::MDString::get(Ctx, llvm::ModLoader::getCodecName(S.Kind)),
        Int(I64, S.Mask), Int(I1, S.TrackDirty)};
    Spaces->addOperand(llvm::MDNode::get(Ctx, Fields));
  }

  llvm::NamedMDNode *Regions = M.getOrInsertNamedMetadata("modloader.regions");
  for (const modloader::Region &R : Table.regions()) {
    llvm::Metadata *Fields[] = {
        Int(I32, Table.getTargetAddressSpace(R.FlatId)),
        Int(I32, Table.getTargetAddressSpace(R.TargetId)), Int(I64, R.From),
        Int(I64, R.To)};
    Regions->addOperand(llvm::MDNode::get(Ctx, Fields));
  }
}

void CodeGen::setModLoaderSymbol(CodeGenModule &CGM, llvm::GlobalVariable *GV,
                                 const VarDecl *D) {
  if (!modloader::isGuestAddressSpace(D->getType().getAddressSpace()))
    return;

  StringRef Symbol = D->getName();
  if (const auto *Label = D->getAttr<AsmLabelAttr>())
    Symbol = Label->getLabel();

  GV->setMetadata(
      "modloader.symbol",
      llvm::MDNode::get(CGM.getLLVMContext(),
                        llvm::MDString::get(CGM.getLLVMContext(), Symbol)));
}
