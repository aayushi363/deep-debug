// McMini Memory Instrumentation Pass — LLVM 3.2 Legacy Pass Manager
//
// Build: cmake pointing at LLVM 3.2 headers (detected automatically via
//        CMakeLists.txt when LLVM_VERSION_MAJOR < 13).
// Use:   opt -load McMiniMemoryPass.so -mcmini-memory input.bc -o output.bc
//
// Inserts __mcmini_read(addr, size, site_id) before every non-atomic,
// non-volatile, non-stack load, and __mcmini_write(...) before every
// matching store — identical ABI and instrumentation logic to the
// new-pass-manager version (mcmini-memory-pass.cpp).

#include "llvm/Pass.h"
#include "llvm/Module.h"
#include "llvm/Function.h"
#include "llvm/BasicBlock.h"
#include "llvm/Instructions.h"
#include "llvm/IRBuilder.h"
#include "llvm/DataLayout.h"
#include "llvm/Type.h"
#include "llvm/DerivedTypes.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Analysis/ValueTracking.h"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

using namespace llvm;

namespace {

struct McMiniMemoryPass : public ModulePass {
  static char ID;
  McMiniMemoryPass() : ModulePass(ID) {}

  virtual bool runOnModule(Module &module) {
    LLVMContext &context = module.getContext();

    // LLVM 3.2: Module::getDataLayout() returns const std::string& (the
    // layout string). Construct a DataLayout object from it directly.
    DataLayout DL(module.getDataLayout());

    Type *void_ty   = Type::getVoidTy(context);
    Type *i8_ptr_ty = Type::getInt8PtrTy(context);
    Type *intptr_ty = DL.getIntPtrType(context);

    // Hook type: void(i8*, intptr_t, intptr_t)
    std::vector<Type *> arg_types;
    arg_types.push_back(i8_ptr_ty);
    arg_types.push_back(intptr_ty);
    arg_types.push_back(intptr_ty);
    FunctionType *hook_ty = FunctionType::get(void_ty, arg_types, false);

    // In LLVM 3.2, getOrInsertFunction returns Constant* — cast to Function*
    Function *read_hook = cast<Function>(
        module.getOrInsertFunction("__mcmini_read", hook_ty));
    Function *write_hook = cast<Function>(
        module.getOrInsertFunction("__mcmini_write", hook_ty));

    // Collect instructions; skip re-instrumentation of the same pointer within
    // a BB (the shadow map keys at 8-byte word granularity, so a second access
    // to the same IR value within one BB is a guaranteed no-op in the runtime).
    std::vector<Instruction *> to_instrument;
    for (Module::iterator F = module.begin(), FE = module.end();
         F != FE; ++F) {
      if (shouldSkipFunction(*F)) continue;
      for (Function::iterator BB = F->begin(), BBE = F->end();
           BB != BBE; ++BB) {
        std::set<Value *> bb_seen;
        for (BasicBlock::iterator I = BB->begin(), IE = BB->end();
             I != IE; ++I) {
          if (!shouldInstrument(*I, DL)) continue;
          Value *ptr = NULL;
          if (LoadInst *L = dyn_cast<LoadInst>(&*I))
            ptr = L->getPointerOperand();
          else if (StoreInst *S = dyn_cast<StoreInst>(&*I))
            ptr = S->getPointerOperand();
          if (ptr && bb_seen.count(ptr)) continue;
          if (ptr) bb_seen.insert(ptr);
          to_instrument.push_back(&*I);
        }
      }
    }

    std::vector<SiteRecord> sites;
    uint64_t site_id = 1;

    for (std::vector<Instruction *>::iterator it = to_instrument.begin(),
                                              end = to_instrument.end();
         it != end; ++it) {
      Instruction *I = *it;
      IRBuilder<> builder(I);
      const uint64_t this_site = site_id++;

      if (LoadInst *load = dyn_cast<LoadInst>(I)) {
        instrumentLoad(builder, DL, read_hook, load, intptr_ty, this_site);
        sites.push_back(makeSiteRecord(this_site, /*is_write=*/false, *load,
                                       context));
      } else if (StoreInst *store = dyn_cast<StoreInst>(I)) {
        instrumentStore(builder, DL, write_hook, store, intptr_ty, this_site);
        sites.push_back(makeSiteRecord(this_site, /*is_write=*/true, *store,
                                       context));
      }
    }

    if (!sites.empty())
      writeSiteMap(module, sites);

    return !to_instrument.empty();
  }

 private:
  struct SiteRecord {
    uint64_t    site_id;
    bool        is_write;
    std::string file;
    unsigned    line;
    unsigned    column;
    std::string function;
  };

  // LLVM 3.2 debug info: DebugLoc::isUnknown() / getLine() / getCol().
  // Filename lives in the scope MDNode; we walk it best-effort.
  static SiteRecord makeSiteRecord(uint64_t site_id, bool is_write,
                                   const Instruction &I,
                                   LLVMContext &ctx) {
    SiteRecord r;
    r.site_id  = site_id;
    r.is_write = is_write;
    r.line     = 0;
    r.column   = 0;
    r.function = I.getParent()->getParent()->getName().str();

    const DebugLoc &loc = I.getDebugLoc();
    if (loc.isUnknown()) return r;

    r.line   = loc.getLine();
    r.column = loc.getCol();

    // Walk the scope MDNode to extract the filename string.
    if (MDNode *scope = loc.getScope(ctx)) {
      // In LLVM 3.2 DIDescriptor layout, operand 1 of the subprogram/block
      // scope node is the DIFile node; its operand 0 is the filename string.
      if (scope->getNumOperands() > 1) {
        if (MDNode *file_node =
                dyn_cast_or_null<MDNode>(scope->getOperand(1))) {
          if (file_node->getNumOperands() > 0) {
            if (MDString *fname =
                    dyn_cast_or_null<MDString>(file_node->getOperand(0)))
              r.file = fname->getString().str();
          }
        }
      }
    }
    return r;
  }

  // LLVM 3.2: raw_fd_ostream takes (const char*, std::string& errstr, flags)
  static void writeSiteMap(const Module &module,
                           const std::vector<SiteRecord> &sites) {
    // getModuleIdentifier() is the LLVM 3.2 equivalent of getSourceFileName()
    std::string path = module.getModuleIdentifier() + ".mcmini-sites";
    std::string err;
    raw_fd_ostream out(path.c_str(), err, 0);  // 0 = text mode in LLVM 3.2
    if (!err.empty()) {
      errs() << "McMiniMemoryPass: could not write site map '" << path
             << "': " << err << "\n";
      return;
    }
    out << "# site_id\tkind\tfile\tline\tcol\tfunction\n";
    for (size_t i = 0; i < sites.size(); ++i) {
      const SiteRecord &r = sites[i];
      out << r.site_id << '\t' << (r.is_write ? "write" : "read") << '\t'
          << (r.file.empty() ? "?" : r.file) << '\t' << r.line << '\t'
          << r.column << '\t' << r.function << '\n';
    }
  }

  static bool shouldSkipFunction(const Function &F) {
    if (F.isDeclaration()) return true;
    StringRef name = F.getName();
    return name == "__mcmini_read" || name == "__mcmini_write" ||
           name.startswith("mcmini_")    ||
           name.startswith("mc_")        ||
           name.startswith("libmcmini_") ||
           name.startswith("thread_");
  }

  // LLVM 3.2: GetUnderlyingObject has a capital G and takes DataLayout*
  static bool isStackLocal(const Value *ptr, const DataLayout &DL) {
    const Value *underlying =
        GetUnderlyingObject(const_cast<Value *>(ptr), &DL);
    return isa<AllocaInst>(underlying);
  }

  static bool shouldInstrument(const Instruction &I, const DataLayout &DL) {
    if (const LoadInst *load = dyn_cast<LoadInst>(&I))
      return !load->isAtomic() && !load->isVolatile() &&
             load->getType()->isSized() &&
             !isStackLocal(load->getPointerOperand(), DL);
    if (const StoreInst *store = dyn_cast<StoreInst>(&I))
      return !store->isAtomic() && !store->isVolatile() &&
             store->getValueOperand()->getType()->isSized() &&
             !isStackLocal(store->getPointerOperand(), DL);
    return false;
  }

  static Value *castToI8Ptr(IRBuilder<> &builder, Value *ptr) {
    return builder.CreatePointerCast(
        ptr, Type::getInt8PtrTy(ptr->getContext()));
  }

  static ConstantInt *makeIntPtr(LLVMContext &ctx, Type *intptr_ty,
                                  uint64_t val) {
    return ConstantInt::get(cast<IntegerType>(intptr_ty), val);
  }

  static void instrumentLoad(IRBuilder<> &builder, const DataLayout &DL,
                              Function *hook, LoadInst *load,
                              Type *intptr_ty, uint64_t site_id) {
    LLVMContext &ctx = load->getContext();
    uint64_t size    = DL.getTypeStoreSize(load->getType());
    Value *args[]    = {castToI8Ptr(builder, load->getPointerOperand()),
                        makeIntPtr(ctx, intptr_ty, size),
                        makeIntPtr(ctx, intptr_ty, site_id)};
    builder.CreateCall(hook, args);
  }

  static void instrumentStore(IRBuilder<> &builder, const DataLayout &DL,
                               Function *hook, StoreInst *store,
                               Type *intptr_ty, uint64_t site_id) {
    LLVMContext &ctx = store->getContext();
    uint64_t size    = DL.getTypeStoreSize(
        store->getValueOperand()->getType());
    Value *args[]    = {castToI8Ptr(builder, store->getPointerOperand()),
                        makeIntPtr(ctx, intptr_ty, size),
                        makeIntPtr(ctx, intptr_ty, site_id)};
    builder.CreateCall(hook, args);
  }
};

char McMiniMemoryPass::ID = 0;

static RegisterPass<McMiniMemoryPass> X(
    "mcmini-memory",
    "McMini Memory Instrumentation Pass",
    false,   // does not only modify the CFG
    false);  // not a pure analysis

}  // namespace
