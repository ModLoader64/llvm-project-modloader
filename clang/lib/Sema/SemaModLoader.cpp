#include "SemaModLoader.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ModLoaderSpaces.h"
#include "clang/Sema/DeclSpec.h"
#include "clang/Sema/Initialization.h"
#include "clang/Sema/ParsedAttr.h"
#include "clang/Sema/Sema.h"
#include "llvm/ADT/STLExtras.h"

using namespace clang;

bool modloader::isGameFunction(Sema &S, Declarator &D, QualType Type) {
  return Type->isFunctionType() &&
         D.getDeclSpec().getStorageClassSpec() != DeclSpec::SCS_typedef &&
         !D.isFunctionDefinition() &&
         getDefaultAddressSpace(S.Context).has_value();
}

void modloader::setDefaultSpace(Sema &S, VarDecl *Var) {
  if (!Var->hasExternalStorage() ||
      !Var->getDeclContext()->getRedeclContext()->isFileContext())
    return;

  Var->setType(withDefaultSpace(S.Context, Var->getType()));
}

bool modloader::checkGuestVariable(Sema &S, VarDecl *Var) {
  if (!isGuestAddressSpace(Var->getType().getAddressSpace()) ||
      Var->hasExternalStorage())
    return true;

  S.Diag(Var->getLocation(), diag::err_modloader_guest_definition) << Var;
  Var->setInvalidDecl();
  return false;
}

bool modloader::checkGuestInitializer(Sema &S, VarDecl *Var) {
  if (!isGuestAddressSpace(Var->getType().getAddressSpace()))
    return true;

  S.Diag(Var->getLocation(), diag::err_modloader_guest_initializer) << Var;
  Var->setInvalidDecl();
  return false;
}

void modloader::finishRecord(Sema &S, RecordDecl *Record) {
  if (S.Context.getModLoaderSpaces().getDefaults().RuntimeLayout &&
      !Record->hasAttr<ModLoaderRuntimeLayoutAttr>())
    Record->addAttr(ModLoaderRuntimeLayoutAttr::CreateImplicit(S.Context));

  if (inGuestABIRegion(S.Context) && !Record->hasAttr<ModLoaderGuestABIAttr>())
    Record->addAttr(ModLoaderGuestABIAttr::CreateImplicit(S.Context));

  if (Record->isDependentType() || Record->isInvalidDecl())
    return;

  if (Record->hasAttr<ModLoaderStructSizeAttr>() ||
      llvm::any_of(Record->fields(), [](const FieldDecl *Field) {
        return Field->hasAttr<ModLoaderFieldOffsetAttr>();
      }))
    (void)S.Context.getASTRecordLayout(Record);
}

void modloader::handleLayoutAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  uint32_t Value;
  if (!S.checkUInt32Argument(AL, AL.getArgAsExpr(0), Value)) {
    AL.setInvalid();
    return;
  }

  if (AL.getKind() == ParsedAttr::AT_ModLoaderStructSize) {
    D->addAttr(::new (S.Context) ModLoaderStructSizeAttr(S.Context, AL, Value));
    return;
  }

  const auto *Field = cast<FieldDecl>(D);
  if (Field->isBitField() || Field->getParent()->isUnion()) {
    S.Diag(AL.getLoc(), diag::err_modloader_field_offset_member)
        << Field->isBitField();
    return;
  }

  D->addAttr(::new (S.Context) ModLoaderFieldOffsetAttr(S.Context, AL, Value));
}

bool modloader::checkGuestMember(Sema &S, Expr *Base, bool IsArrow,
                                 FieldDecl *Field, SourceLocation Loc) {
  QualType ObjectType = Base->getType();
  if (IsArrow) {
    if (const auto *Pointer = ObjectType->getAs<PointerType>())
      ObjectType = Pointer->getPointeeType();
  }

  if (!isGuestAddressSpace(ObjectType.getAddressSpace()))
    return true;

  if (Field->isBitField() &&
      !Field->getParent()->hasAttr<ModLoaderGuestABIAttr>()) {
    S.Diag(Loc, diag::err_modloader_guest_bitfield) << Field;
    return false;
  }

  if (isHostOnlyType(Field->getType())) {
    S.Diag(Loc, diag::err_modloader_guest_host_member)
        << Field << Field->getType();
    return false;
  }

  return true;
}

static Expr *getCopyValue(Sema &S, Expr *Source) {
  if (!Source->isGLValue())
    return Source;

  return ImplicitCastExpr::Create(
      S.Context, Source->getType().getUnqualifiedType(), CK_LValueToRValue,
      Source, nullptr, VK_PRValue, FPOptionsOverride());
}

ExprResult modloader::buildObjectAssignment(Sema &S, SourceLocation OpLoc,
                                            Expr *LHS, Expr *RHS) {
  if (LHS->isTypeDependent() || RHS->isTypeDependent())
    return ExprEmpty();

  QualType LHSType = LHS->getType();
  if (!LHSType->isRecordType() || !LHS->isLValue() ||
      LHSType.isConstQualified())
    return ExprEmpty();

  bool GuestTarget = isGuestAddressSpace(LHSType.getAddressSpace());
  if (!GuestTarget && (isa<InitListExpr>(RHS) ||
                       !isGuestAddressSpace(RHS->getType().getAddressSpace())))
    return ExprEmpty();

  if (!LHSType.isTriviallyCopyableType(S.Context))
    return ExprEmpty();

  if (GuestTarget &&
      (isa<InitListExpr>(RHS) ||
       !S.Context.hasSameUnqualifiedType(LHSType, RHS->getType()))) {
    InitializedEntity Entity =
        InitializedEntity::InitializeTemporary(LHSType.getUnqualifiedType());
    InitializationKind Kind =
        InitializationKind::CreateCopy(RHS->getBeginLoc(), OpLoc);
    InitializationSequence Sequence(S, Entity, Kind, RHS);
    ExprResult Temporary = Sequence.Perform(S, Entity, Kind, RHS);
    if (Temporary.isInvalid())
      return ExprError();

    RHS = Temporary.get();
  }

  QualType RHSType = RHS->getType();
  if (!S.Context.hasSameUnqualifiedType(LHSType, RHSType))
    return ExprEmpty();

  return BinaryOperator::Create(S.Context, LHS, getCopyValue(S, RHS), BO_Assign,
                                LHSType, VK_LValue, OK_Ordinary, OpLoc,
                                S.CurFPFeatureOverrides());
}

std::optional<bool> modloader::checkSpaceCast(Sema &S, QualType SrcType,
                                              QualType DestType, bool CStyle,
                                              unsigned &Message,
                                              CastKind &Kind) {
  if (CStyle)
    return std::nullopt;

  const auto *SrcPtrType = SrcType->getAs<PointerType>();
  const auto *DestPtrType = DestType->getAs<PointerType>();
  if (!SrcPtrType || !DestPtrType)
    return std::nullopt;

  QualType SrcPointee = SrcPtrType->getPointeeType();
  QualType DestPointee = DestPtrType->getPointeeType();
  LangAS SrcAS = SrcPointee.getAddressSpace();
  LangAS DestAS = DestPointee.getAddressSpace();
  if (!isGuestAddressSpace(SrcAS) || !isGuestAddressSpace(DestAS)) {
    Message = diag::err_modloader_host_guest_cast;
    return false;
  }

  unsigned SrcId = getGuestSpaceId(SrcAS);
  unsigned DestId = getGuestSpaceId(DestAS);
  if (SrcId != DestId && SrcId != FlatSpaceId && DestId != FlatSpaceId) {
    Message = diag::err_modloader_space_cast_spaces;
    return false;
  }

  if (!S.Context.hasSameType(
          S.Context.removeAddrSpaceQualType(SrcPointee.getCanonicalType()),
          S.Context.removeAddrSpaceQualType(DestPointee.getCanonicalType())))
    return std::nullopt;

  Kind = SrcAS == DestAS ? CK_NoOp : CK_AddressSpaceConversion;
  return true;
}

void modloader::convertGuestSource(Sema &S, const InitializedEntity &Entity,
                                   MultiExprArg Args) {
  if (Args.size() != 1 || Args[0]->isTypeDependent() || !Args[0]->isGLValue())
    return;

  QualType SourceType = Args[0]->getType();
  QualType TargetType = Entity.getType();
  if (!SourceType->isRecordType() || !TargetType->isRecordType() ||
      !isGuestAddressSpace(SourceType.getAddressSpace()) ||
      isGuestAddressSpace(TargetType.getAddressSpace()) ||
      !S.Context.hasSameUnqualifiedType(SourceType, TargetType))
    return;

  if (SourceType.isTriviallyCopyableType(S.Context))
    Args[0] = getCopyValue(S, Args[0]);
}
