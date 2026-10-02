#ifndef LLVM_LIB_TARGET_WEBASSEMBLY_WEBASSEMBLYMODLOADERLOWERING_H
#define LLVM_LIB_TARGET_WEBASSEMBLY_WEBASSEMBLYMODLOADERLOWERING_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class PassBuilder;

class WebAssemblyModLoaderLoweringPass
    : public RequiredPassInfoMixin<WebAssemblyModLoaderLoweringPass> {
  bool OptimizePointers;

public:
  explicit WebAssemblyModLoaderLoweringPass(bool OptimizePointers = false)
      : OptimizePointers(OptimizePointers) {}

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

bool isModLoaderNoopAddrSpaceCast(unsigned SrcAS, unsigned DestAS);
void registerModLoaderPassBuilderCallbacks(PassBuilder &PB);

} // namespace llvm

#endif // LLVM_LIB_TARGET_WEBASSEMBLY_WEBASSEMBLYMODLOADERLOWERING_H
