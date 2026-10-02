#ifndef LLVM_CLANG_LIB_SEMA_SEMAMODLOADER_H
#define LLVM_CLANG_LIB_SEMA_SEMAMODLOADER_H

#include "clang/AST/ModLoaderSpaces.h"
#include "clang/AST/OperationKinds.h"
#include "clang/AST/Type.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Sema/Ownership.h"

namespace clang {
class Decl;
class Declarator;
class Expr;
class FieldDecl;
class InitializedEntity;
class ParsedAttr;
class RecordDecl;
class Sema;
class VarDecl;

namespace modloader {

// Guest function declarations are omitted
bool isGameFunction(Sema &S, Declarator &D, QualType Type);
void setDefaultSpace(Sema &S, VarDecl *Var);
bool checkGuestVariable(Sema &S, VarDecl *Var);
bool checkGuestInitializer(Sema &S, VarDecl *Var);
void finishRecord(Sema &S, RecordDecl *Record);
void handleLayoutAttr(Sema &S, Decl *D, const ParsedAttr &AL);
bool checkGuestMember(Sema &S, Expr *Base, bool IsArrow, FieldDecl *Field,
                      SourceLocation Loc);
ExprResult buildObjectAssignment(Sema &S, SourceLocation OpLoc, Expr *LHS,
                                 Expr *RHS);
std::optional<bool> checkSpaceCast(Sema &S, QualType SrcType, QualType DestType,
                                   bool CStyle, unsigned &Message,
                                   CastKind &Kind);
void convertGuestSource(Sema &S, const InitializedEntity &Entity,
                        MultiExprArg Args);

} // namespace modloader
} // namespace clang

#endif // LLVM_CLANG_LIB_SEMA_SEMAMODLOADER_H
