#include "CGModLoader.h"
#include "CodeGenFunction.h"
#include "CodeGenModule.h"
#include "clang/AST/ModLoaderSpaces.h"
#include "clang/AST/RecordLayout.h"
#include "llvm/Support/JSON.h"

using namespace clang;
using namespace CodeGen;

static std::string recordName(const RecordDecl *Record) {
  if (Record->getIdentifier()) {
    std::string Name;
    llvm::raw_string_ostream Stream(Name);
    PrintingPolicy Policy(Record->getASTContext().getPrintingPolicy());
    Policy.PrintAsCanonical = true;
    Policy.SuppressDefaultTemplateArgs = false;
    Policy.UsePreferredNames = false;
    Policy.AlwaysIncludeTypeForTemplateArgument = true;
    Record->getNameForDiagnostic(Stream, Policy, true);
    return Name;
  }
  if (const TypedefNameDecl *Alias = Record->getTypedefNameForAnonDecl())
    return Alias->getQualifiedNameAsString();
  if (const auto *Parent = dyn_cast<RecordDecl>(Record->getDeclContext())) {
    for (const FieldDecl *Field : Parent->fields()) {
      const auto *Type =
          Field->getType()->getBaseElementTypeUnsafe()->getAs<RecordType>();
      if (Type &&
          Type->getDecl()->getCanonicalDecl() == Record->getCanonicalDecl())
        return recordName(Parent) + "." +
               (Field->getIdentifier()
                    ? Field->getNameAsString()
                    : "$" + std::to_string(Field->getFieldIndex()));
    }
  }
  return Record->getQualifiedNameAsString();
}

static std::string fieldName(const FieldDecl *Field) {
  return Field->getIdentifier() ? Field->getNameAsString()
                                : "$" + std::to_string(Field->getFieldIndex());
}

static llvm::GlobalVariable *layoutSlot(CodeGenModule &CGM, StringRef Name,
                                        uint64_t Baseline) {
  llvm::Module &Module = CGM.getModule();
  if (auto *Global = Module.getNamedGlobal(Name))
    return Global;
  auto *Global = new llvm::GlobalVariable(
      Module, CGM.Int64Ty, false, llvm::GlobalValue::LinkOnceODRLinkage,
      llvm::ConstantInt::get(CGM.Int64Ty, Baseline), Name, nullptr,
      llvm::GlobalVariable::NotThreadLocal, 1);
  Global->setExternallyInitialized(true);
  Global->addAttribute("wasm-export-name", Name);
  CGM.addUsedGlobal(Global);
  return Global;
}

static void describeRecord(CodeGenModule &CGM, const RecordDecl *Record) {
  Record = Record->getDefinitionOrSelf();
  if (!Record->hasAttr<ModLoaderRuntimeLayoutAttr>())
    return;
  if (const auto *Class = dyn_cast<CXXRecordDecl>(Record)) {
    if (Class->getNumBases() || Class->isDynamicClass()) {
      CGM.Error(Record->getLocation(),
                "runtime guest layouts require records without base classes or "
                "virtual members");
      return;
    }
  }
  std::string Name = recordName(Record);
  std::string Prefix = "modloader.layout." + Name;
  std::string FunctionName = Prefix + ".describe";
  llvm::Module &Module = CGM.getModule();
  if (Module.getFunction(FunctionName))
    return;

  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(CGM.VoidPtrTy, false),
      llvm::GlobalValue::LinkOnceODRLinkage, FunctionName, Module);
  Function->addFnAttr("wasm-export-name", FunctionName);
  CGM.addUsedGlobal(Function);

  ASTContext &Context = CGM.getContext();
  const ASTRecordLayout &Layout = Context.getASTRecordLayout(Record);
  llvm::json::Array Fields;
  for (const FieldDecl *Field : Record->fields()) {
    if (Field->isUnnamedBitField())
      continue;
    QualType Type = Field->getType();
    uint64_t Count = 1;
    bool Array = false;
    while (const auto *ArrayType = Context.getAsConstantArrayType(Type)) {
      Array = true;
      Count *= ArrayType->getZExtSize();
      Type = ArrayType->getElementType();
    }
    bool Incomplete = Field->getType()->isIncompleteArrayType();
    uint64_t Size =
        Incomplete ? 0
                   : Context.getTypeSizeInChars(Field->getType()).getQuantity();
    uint64_t Alignment = Context.getDeclAlign(Field).getQuantity();
    uint64_t Offset =
        Context
            .toCharUnitsFromBits(Layout.getFieldOffset(Field->getFieldIndex()))
            .getQuantity();
    if (Field->isBitField())
      Size = (Layout.getFieldOffset(Field->getFieldIndex()) %
                  Context.getCharWidth() +
              Field->getBitWidthValue() + Context.getCharWidth() - 1) /
             Context.getCharWidth();
    llvm::json::Object Entry{{"name", fieldName(Field)},
                             {"offset", Offset},
                             {"size", Size},
                             {"align", Alignment}};
    Entry["kind"] = Field->isBitField()   ? "bitfield"
                    : Array || Incomplete ? "array"
                                          : "scalar";
    if (Array) {
      Entry["count"] = Count;
      Entry["element_size"] =
          uint64_t(Context.getTypeSizeInChars(Type).getQuantity());
    }
    if (Field->isBitField())
      Entry["bits"] = uint64_t(Field->getBitWidthValue());
    if (const auto *Child = Type->getAs<RecordType>()) {
      const RecordDecl *ChildRecord = Child->getDecl()->getDefinitionOrSelf();
      if (!Array)
        Entry["kind"] = ChildRecord->isUnion() ? "union" : "record";
      if (ChildRecord->hasAttr<ModLoaderRuntimeLayoutAttr>()) {
        Entry["record"] = recordName(ChildRecord);
        describeRecord(CGM, ChildRecord);
      }
    }
    Fields.push_back(std::move(Entry));
  }

  llvm::json::Object Description{
      {"record", Name},
      {"size", uint64_t(Layout.getSize().getQuantity())},
      {"align", uint64_t(Layout.getAlignment().getQuantity())},
      {"union", Record->isUnion()},
      {"fields", std::move(Fields)}};
  std::string Text;
  llvm::raw_string_ostream Stream(Text);
  Stream << llvm::json::Value(std::move(Description));
  auto *Data = llvm::ConstantDataArray::getString(CGM.getLLVMContext(), Text);
  auto *Global = new llvm::GlobalVariable(Module, Data->getType(), true,
                                          llvm::GlobalValue::PrivateLinkage,
                                          Data, Prefix + ".description");
  Global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  llvm::BasicBlock *Block =
      llvm::BasicBlock::Create(CGM.getLLVMContext(), "entry", Function);
  llvm::ReturnInst::Create(CGM.getLLVMContext(), Global, Block);
}

static llvm::Value *loadSlot(CodeGenFunction &CGF, const RecordDecl *Record,
                             StringRef Suffix, uint64_t Baseline) {
  describeRecord(CGF.CGM, Record);
  llvm::GlobalVariable *Slot = layoutSlot(
      CGF.CGM, "modloader.layout." + recordName(Record) + Suffix.str(),
      Baseline);
  return CGF.Builder.CreateLoad(
      Address(Slot, CGF.Int64Ty, CharUnits::fromQuantity(8)), "guest.layout");
}

llvm::Value *CodeGen::emitModLoaderSize(CodeGenFunction &CGF, QualType Type,
                                        bool Alignment) {
  if (!modloader::hasRuntimeLayout(Type))
    return nullptr;
  if (const auto *Reference = Type->getAs<ReferenceType>())
    Type = Reference->getPointeeType();
  uint64_t Count = 1;
  llvm::Value *VariableCount = nullptr;
  while (const ArrayType *Array = CGF.getContext().getAsArrayType(Type)) {
    if (const auto *Constant = dyn_cast<ConstantArrayType>(Array)) {
      Count *= Constant->getZExtSize();
      Type = Array->getElementType();
    } else if (const auto *Variable = dyn_cast<VariableArrayType>(Array)) {
      if (Alignment) {
        Type = Array->getElementType();
      } else {
        auto Size = CGF.getVLASize(Variable);
        VariableCount =
            CGF.Builder.CreateZExtOrTrunc(Size.NumElts, CGF.Int64Ty);
        Type = Size.Type;
      }
    } else {
      Type = Array->getElementType();
    }
  }
  const RecordDecl *Record =
      Type->castAs<RecordType>()->getDecl()->getDefinitionOrSelf();
  const ASTRecordLayout &Layout = CGF.getContext().getASTRecordLayout(Record);
  llvm::Value *Value = loadSlot(
      CGF, Record, Alignment ? ".align" : ".size",
      (Alignment ? Layout.getAlignment() : Layout.getSize()).getQuantity());
  if (VariableCount)
    Value = CGF.Builder.CreateMul(Value, VariableCount);
  if (!Alignment && Count != 1)
    Value = CGF.Builder.CreateMul(Value, CGF.Builder.getInt64(Count));
  return CGF.Builder.CreateZExtOrTrunc(Value, CGF.SizeTy);
}

llvm::Value *CodeGen::emitModLoaderFieldLayout(CodeGenFunction &CGF,
                                               const FieldDecl *Field,
                                               bool Size, bool CheckPresence) {
  const RecordDecl *Record = Field->getParent();
  if (!Record->hasAttr<ModLoaderRuntimeLayoutAttr>())
    return nullptr;
  const ASTRecordLayout &Layout = CGF.getContext().getASTRecordLayout(Record);
  uint64_t Baseline =
      Size ? CGF.getContext().getTypeSizeInChars(Field->getType()).getQuantity()
           : CGF.getContext()
                 .toCharUnitsFromBits(
                     Layout.getFieldOffset(Field->getFieldIndex()))
                 .getQuantity();
  std::string Suffix = "." + fieldName(Field);
  uint64_t OffsetBaseline =
      CGF.getContext()
          .toCharUnitsFromBits(Layout.getFieldOffset(Field->getFieldIndex()))
          .getQuantity();
  llvm::Value *Offset =
      loadSlot(CGF, Record, Suffix + ".offset", OffsetBaseline);
  if (CheckPresence) {
    llvm::BasicBlock *Present = CGF.createBasicBlock("guest.field.present");
    llvm::BasicBlock *Missing = CGF.createBasicBlock("guest.field.absent");
    CGF.Builder.CreateCondBr(
        CGF.Builder.CreateICmpEQ(Offset, CGF.Builder.getInt64(UINT64_MAX)),
        Missing, Present);
    CGF.EmitBlock(Missing);
    CGF.Builder.CreateCall(CGF.CGM.getIntrinsic(llvm::Intrinsic::trap));
    CGF.Builder.CreateUnreachable();
    CGF.EmitBlock(Present);
  }
  llvm::Value *Value =
      Size ? loadSlot(CGF, Record, Suffix + ".size", Baseline) : Offset;
  return CGF.Builder.CreateZExtOrTrunc(Value, CGF.SizeTy);
}

llvm::Value *CodeGen::emitModLoaderLayoutQuery(CodeGenFunction &CGF,
                                               const Expr *E) {
  if (!modloader::isRuntimeLayoutQuery(E))
    return nullptr;
  if (const auto *Trait = dyn_cast<UnaryExprOrTypeTraitExpr>(E)) {
    bool Alignment = Trait->getKind() == UETT_AlignOf ||
                     Trait->getKind() == UETT_PreferredAlignOf;
    if (!Alignment && Trait->getTypeOfArgument()->isVariableArrayType()) {
      if (Trait->isArgumentType())
        CGF.EmitVariablyModifiedType(Trait->getTypeOfArgument());
      else
        CGF.EmitIgnoredExpr(Trait->getArgumentExpr());
    }
    if (!Alignment && !Trait->isArgumentType()) {
      if (const auto *Member =
              dyn_cast<MemberExpr>(Trait->getArgumentExpr()->IgnoreParens()))
        if (const auto *Field = dyn_cast<FieldDecl>(Member->getMemberDecl()))
          return emitModLoaderFieldLayout(CGF, Field, true);
    }
    if (auto *Value =
            emitModLoaderSize(CGF, Trait->getTypeOfArgument(), Alignment))
      return Value;
    if (Alignment)
      return llvm::ConstantInt::get(
          CGF.SizeTy, CGF.getContext()
                          .getTypeAlignInChars(Trait->getTypeOfArgument())
                          .getQuantity());
    return nullptr;
  }
  return nullptr;
}

llvm::Value *CodeGen::emitModLoaderPointerOffset(CodeGenFunction &CGF,
                                                 llvm::Value *Pointer,
                                                 QualType Element,
                                                 llvm::Value *Index) {
  if (!llvm::ModLoader::isGuestAddressSpace(
          Pointer->getType()->getPointerAddressSpace()))
    return nullptr;
  Element = CGF.getContext().getAddrSpaceQualType(
      Element,
      getLangASFromTargetAS(Pointer->getType()->getPointerAddressSpace()));
  llvm::Value *Size = emitModLoaderSize(CGF, Element);
  if (!Size)
    return nullptr;
  llvm::Type *IndexType =
      CGF.CGM.getDataLayout().getIndexType(Pointer->getType());
  Size = CGF.Builder.CreateZExtOrTrunc(Size, IndexType);
  Index = CGF.Builder.CreateSExtOrTrunc(Index, IndexType);
  return CGF.Builder.CreateGEP(
      CGF.Int8Ty, Pointer, CGF.Builder.CreateMul(Index, Size), "guest.index");
}
