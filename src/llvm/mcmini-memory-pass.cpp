#include "llvm/Analysis/ValueTracking.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Type.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace llvm;

namespace {

struct McMiniMemoryPass : public PassInfoMixin<McMiniMemoryPass> {
  PreservedAnalyses run(Module &module, ModuleAnalysisManager &) {
    LLVMContext &context = module.getContext();
    const DataLayout &data_layout = module.getDataLayout();

    Type *void_ty = Type::getVoidTy(context);
    Type *i8_ptr_ty = Type::getInt8PtrTy(context);
    Type *intptr_ty = data_layout.getIntPtrType(context);

    FunctionCallee read_hook = module.getOrInsertFunction(
        "__mcmini_read", void_ty, i8_ptr_ty, intptr_ty, intptr_ty);
    FunctionCallee write_hook = module.getOrInsertFunction(
        "__mcmini_write", void_ty, i8_ptr_ty, intptr_ty, intptr_ty);

    std::vector<Instruction *> memory_instructions;
    for (Function &function : module) {
      if (shouldSkipFunction(function)) continue;
      for (BasicBlock &block : function) {
        for (Instruction &instruction : block) {
          if (shouldInstrument(instruction, data_layout)) {
            memory_instructions.push_back(&instruction);
          }
        }
      }
    }

    std::vector<SiteRecord> sites;
    uint64_t site_id = 1;
    for (Instruction *instruction : memory_instructions) {
      IRBuilder<> builder(instruction);
      const uint64_t this_site = site_id++;
      if (auto *load = dyn_cast<LoadInst>(instruction)) {
        instrumentLoad(builder, data_layout, read_hook, load, intptr_ty,
                       this_site);
        sites.push_back(makeSiteRecord(this_site, /*is_write=*/false, *load));
      } else if (auto *store = dyn_cast<StoreInst>(instruction)) {
        instrumentStore(builder, data_layout, write_hook, store, intptr_ty,
                        this_site);
        sites.push_back(makeSiteRecord(this_site, /*is_write=*/true, *store));
      }
    }

    if (!sites.empty()) writeSiteMap(module, sites);

    return memory_instructions.empty() ? PreservedAnalyses::all()
                                       : PreservedAnalyses::none();
  }

 private:
  struct SiteRecord {
    uint64_t site_id;
    bool is_write;
    std::string file;
    unsigned line;
    unsigned column;
    std::string function;
  };

  static SiteRecord makeSiteRecord(uint64_t site_id, bool is_write,
                                   const Instruction &instruction) {
    SiteRecord record;
    record.site_id = site_id;
    record.is_write = is_write;
    record.line = 0;
    record.column = 0;
    record.function = instruction.getFunction()->getName().str();
    if (const DILocation *loc = instruction.getDebugLoc().get()) {
      record.file = loc->getFilename().str();
      record.line = loc->getLine();
      record.column = loc->getColumn();
    }
    return record;
  }

  // Emit a sidecar map so reported races (which carry only an integer site id)
  // can be symbolized back to source. Written next to the translation unit as
  // "<source>.mcmini-sites". NOTE: site ids are unique only within a single
  // module; multi-TU programs need the per-TU files disambiguated by caller.
  static void writeSiteMap(const Module &module,
                           const std::vector<SiteRecord> &sites) {
    std::string path = module.getSourceFileName() + ".mcmini-sites";
    std::error_code ec;
    raw_fd_ostream out(path, ec, sys::fs::OF_Text);
    if (ec) {
      errs() << "McMiniMemoryPass: could not write site map '" << path
             << "': " << ec.message() << "\n";
      return;
    }
    out << "# site_id\tkind\tfile\tline\tcol\tfunction\n";
    for (const SiteRecord &record : sites) {
      out << record.site_id << '\t' << (record.is_write ? "write" : "read")
          << '\t' << (record.file.empty() ? "?" : record.file) << '\t'
          << record.line << '\t' << record.column << '\t' << record.function
          << '\n';
    }
  }

  static bool shouldSkipFunction(const Function &function) {
    if (function.isDeclaration()) return true;

    StringRef name = function.getName();
    return name == "__mcmini_read" || name == "__mcmini_write" ||
           name.startswith("mcmini_") || name.startswith("mc_") ||
           name.startswith("libmcmini_") || name.startswith("thread_");
  }

  static bool isStackLocal(const Value *ptr) {
    const Value *underlying = getUnderlyingObject(ptr);
    return isa<AllocaInst>(underlying);
  }

  static bool shouldInstrument(const Instruction &instruction,
                               const DataLayout &data_layout) {
    if (auto *load = dyn_cast<LoadInst>(&instruction)) {
      return !load->isAtomic() && !load->isVolatile() &&
             load->getType()->isSized() && !isStackLocal(load->getPointerOperand());
    }
    if (auto *store = dyn_cast<StoreInst>(&instruction)) {
      return !store->isAtomic() && !store->isVolatile() &&
             store->getValueOperand()->getType()->isSized() &&
             !isStackLocal(store->getPointerOperand());
    }
    return false;
  }

  static Value *castPointer(IRBuilder<> &builder, Value *ptr) {
    return builder.CreatePointerCast(ptr, Type::getInt8PtrTy(ptr->getContext()));
  }

  static ConstantInt *constantIntPtr(LLVMContext &context, Type *intptr_ty,
                                     uint64_t value) {
    return ConstantInt::get(cast<IntegerType>(intptr_ty), value);
  }

  static void instrumentLoad(IRBuilder<> &builder, const DataLayout &data_layout,
                             FunctionCallee hook, LoadInst *load,
                             Type *intptr_ty, uint64_t site_id) {
    LLVMContext &context = load->getContext();
    uint64_t size = data_layout.getTypeStoreSize(load->getType());
    builder.CreateCall(hook, {castPointer(builder, load->getPointerOperand()),
                              constantIntPtr(context, intptr_ty, size),
                              constantIntPtr(context, intptr_ty, site_id)});
  }

  static void instrumentStore(IRBuilder<> &builder,
                              const DataLayout &data_layout,
                              FunctionCallee hook, StoreInst *store,
                              Type *intptr_ty, uint64_t site_id) {
    LLVMContext &context = store->getContext();
    Type *stored_ty = store->getValueOperand()->getType();
    uint64_t size = data_layout.getTypeStoreSize(stored_ty);
    builder.CreateCall(hook, {castPointer(builder, store->getPointerOperand()),
                              constantIntPtr(context, intptr_ty, size),
                              constantIntPtr(context, intptr_ty, site_id)});
  }
};

}  // namespace

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "McMiniMemoryPass", LLVM_VERSION_STRING,
          [](PassBuilder &pass_builder) {
            pass_builder.registerPipelineParsingCallback(
                [](StringRef name, ModulePassManager &module_pm,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (name != "mcmini-memory") return false;
                  module_pm.addPass(McMiniMemoryPass());
                  return true;
                });
            pass_builder.registerPipelineStartEPCallback(
                [](ModulePassManager &module_pm,
                   OptimizationLevel) {
                  module_pm.addPass(McMiniMemoryPass());
                });
          }};
}
