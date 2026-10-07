#ifndef LLVM_CLANG_LIB_CODEGEN_CGMODLOADER_H
#define LLVM_CLANG_LIB_CODEGEN_CGMODLOADER_H

#include "Address.h"
#include "clang/AST/Type.h"

namespace llvm {
class GlobalVariable;
class Value;
} // namespace llvm

namespace clang {
class CastExpr;
class Expr;
class FieldDecl;
class RecordDecl;
class VarDecl;

namespace CodeGen {
class CodeGenFunction;
class CodeGenModule;

llvm::Value *emitModLoaderSize(CodeGenFunction &CGF, QualType Type,
                               bool Alignment = false);
llvm::Value *emitModLoaderFieldLayout(CodeGenFunction &CGF,
                                      const FieldDecl *Field, bool Size = false,
                                      bool CheckPresence = true);
llvm::Value *emitModLoaderLayoutQuery(CodeGenFunction &CGF, const Expr *E);
llvm::Value *emitModLoaderPointerOffset(CodeGenFunction &CGF,
                                        llvm::Value *Pointer, QualType Element,
                                        llvm::Value *Index);
bool emitModLoaderAggregateCopy(CodeGenFunction &CGF, Address Dest, Address Src,
                                QualType Type, bool IsVolatile);
llvm::Value *emitModLoaderRangeCheckedCast(CodeGenFunction &CGF,
                                           const CastExpr *Cast);
void emitModLoaderMetadata(CodeGenModule &CGM);
void setModLoaderSymbol(CodeGenModule &CGM, llvm::GlobalVariable *GV,
                        const VarDecl *D);

} // namespace CodeGen
} // namespace clang

#endif // LLVM_CLANG_LIB_CODEGEN_CGMODLOADER_H
