/*
 * EnzymeLogic.cpp
 *
 * Copyright (C) 2019 William S. Moses (enzyme@wsmoses.com) - All Rights Reserved
 *
 * For commercial use of this code please contact the author(s) above.
 *
 * For research use of the code please use the following citation.
 *
 * \misc{mosesenzyme,
    author = {William S. Moses, Tim Kaler},
    title = {Enzyme: LLVM Automatic Differentiation},
    year = {2019},
    howpublished = {\url{https://github.com/wsmoses/Enzyme/}},
    note = {commit xxxxxxx}
 */

#include "EnzymeLogic.h"

#include "SCEV/ScalarEvolutionExpander.h"

#include <deque>

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Verifier.h"

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/ValueTracking.h"

#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include "llvm/Analysis/GlobalsModRef.h"
#include "llvm/Analysis/BasicAliasAnalysis.h"

#include "Utils.h"
#include "GradientUtils.h"
#include "FunctionUtils.h"
#include "LibraryFuncs.h"

using namespace llvm;

llvm::cl::opt<bool> enzyme_print("enzyme_print", cl::init(false), cl::Hidden,
                cl::desc("Print before and after fns for autodiff"));

cl::opt<bool> cache_reads_always(
            "enzyme_always_cache_reads", cl::init(false), cl::Hidden,
            cl::desc("Force always caching of all reads"));

cl::opt<bool> cache_reads_never(
            "enzyme_never_cache_reads", cl::init(false), cl::Hidden,
            cl::desc("Force never caching of all reads"));

cl::opt<bool> nonmarkedglobals_inactiveloads(
            "enzyme_nonmarkedglobals_inactiveloads", cl::init(true), cl::Hidden,
            cl::desc("Consider loads of nonmarked globals to be inactive"));

// Computes a map of LoadInst -> boolean for a function indicating whether that load is "uncacheable".
//   A load is considered "uncacheable" if the data at the loaded memory location can be modified after
//   the load instruction.
std::map<Instruction*, bool> compute_uncacheable_load_map(GradientUtils* gutils, AAResults& AA, TargetLibraryInfo& TLI,
    const std::map<Argument*, bool> uncacheable_args) {
  std::map<Instruction*, bool> can_modref_map;
  for (inst_iterator I = inst_begin(*gutils->oldFunc), E = inst_end(*gutils->oldFunc); I != E; ++I) {
    Instruction* inst = &*I;
      // For each load instruction, determine if it is uncacheable.
      if (auto op = dyn_cast<LoadInst>(inst)) {

        bool can_modref = false;
        // Find the underlying object for the pointer operand of the load instruction.
        auto obj = GetUnderlyingObject(op->getPointerOperand(), gutils->oldFunc->getParent()->getDataLayout(), 100);

        //llvm::errs() << "underlying object for load " << *op << " is " << *obj << "\n";
        // If the pointer operand is from an argument to the function, we need to check if the argument
        //   received from the caller is uncacheable.
        if (auto arg = dyn_cast<Argument>(obj)) {
          auto found = uncacheable_args.find(arg);
            if (found == uncacheable_args.end()) {
                llvm::errs() << "uncacheable_args:\n";
                for(auto& pair : uncacheable_args) {
                    llvm::errs() << " + " << *pair.first << ": " << pair.second << " of func " << pair.first->getParent()->getName() << "\n";
                }
                llvm::errs() << "could not find " << *arg << " of func " << arg->getParent()->getName() << " in args_map\n";
            }
          assert(found != uncacheable_args.end());
          if (found->second) {
            //llvm::errs() << "OP is uncacheable arg: " << *op << "\n";
            can_modref = true;
          }
          //llvm::errs() << " + argument (can_modref=" << can_modref << ") " << *op << " object: " << *obj << " arg: " << *arg << "e\n";
          //TODO this case (alloca goes out of scope/allocation is freed and we dont force it to continue needs to be forcibly cached)
        } else {
          // NOTE(TFK): In the case where the underlying object for the pointer operand is from a Load or Call we need
          //  to check if we need to cache. Likely, we need to play it safe in this case and cache.
          // NOTE(TFK): The logic below is an attempt at a conservative handling of the case mentioned above, but it
          //   needs to be verified.

          // Pointer operands originating from call instructions that are not malloc/free are conservatively considered uncacheable.
          if (auto obj_op = dyn_cast<CallInst>(obj)) {
            Function* called = obj_op->getCalledFunction();
            if (auto castinst = dyn_cast<ConstantExpr>(obj_op->getCalledValue())) {
              if (castinst->isCast()) {
                if (auto fn = dyn_cast<Function>(castinst->getOperand(0))) {
                  if (isAllocationFunction(*fn, TLI) || isDeallocationFunction(*fn, TLI)) {
                    called = fn;
                  }
                }
              }
            }
            if (called && isCertainMallocOrFree(called)) {
              //llvm::errs() << "OP is certain malloc or free: " << *op << "\n";
            } else {
              //llvm::errs() << "OP is a non malloc/free call so we need to cache " << *op << "\n";
              can_modref = true;
            }
          } else if (isa<LoadInst>(obj)) {
            // If obj is from a load instruction conservatively consider it uncacheable.
            //llvm::errs() << "OP is from a load, needing to cache " << *op << "\n";
            can_modref = true;
          } else {
            // In absence of more information, assume that the underlying object for pointer operand is uncacheable in caller.
            //llvm::errs() << "OP is an unknown instruction, needing to cache " << *op << "\n";
            can_modref = true;
          }
        }

        for (inst_iterator I2 = inst_begin(*gutils->oldFunc), E2 = inst_end(*gutils->oldFunc); I2 != E2; ++I2) {
            Instruction* inst2 = &*I2;
            assert(inst->getParent()->getParent() == inst2->getParent()->getParent());
            if (inst == inst2) continue;
            if (!gutils->OrigDT.dominates(inst2, inst)) {

              // Don't consider modref from malloc/free as a need to cache
              if (auto obj_op = dyn_cast<CallInst>(inst2)) {
                Function* called = obj_op->getCalledFunction();
                if (auto castinst = dyn_cast<ConstantExpr>(obj_op->getCalledValue())) {
                  if (castinst->isCast()) {
                    if (auto fn = dyn_cast<Function>(castinst->getOperand(0))) {
                      if (isAllocationFunction(*fn, TLI) || isDeallocationFunction(*fn, TLI)) {
                        called = fn;
                      }
                    }
                  }
                }
                if (called && isCertainMallocOrFree(called)) {
                  continue;
                }
              }

              if (llvm::isModSet(AA.getModRefInfo(inst2, MemoryLocation::get(op)))) {
                can_modref = true;
                //llvm::errs() << *inst << " needs to be cached due to: " << *inst2 << "\n";
                break;
              }
            }
        }
        can_modref_map[inst] = can_modref;
      }
  }
  return can_modref_map;
}

std::map<Argument*, bool> compute_uncacheable_args_for_one_callsite(CallInst* callsite_op, DominatorTree &DT,
    TargetLibraryInfo &TLI, AAResults& AA, GradientUtils* gutils, const std::map<Argument*, bool> parent_uncacheable_args) {

  std::vector<Value*> args;
  std::vector<bool> args_safe;

  // First, we need to propagate the uncacheable status from the parent function to the callee.
  //   because memory location x modified after parent returns => x modified after callee returns.
  for (unsigned i = 0; i < callsite_op->getNumArgOperands(); i++) {
      args.push_back(callsite_op->getArgOperand(i));
      bool init_safe = true;

      // If the UnderlyingObject is from one of this function's arguments, then we need to propagate the volatility.
      Value* obj = GetUnderlyingObject(callsite_op->getArgOperand(i),
                                       callsite_op->getParent()->getModule()->getDataLayout(),
                                       100);
      //llvm::errs() << "ocs underlying object for callsite " << *callsite_op << " idx: " << i << " is " << *obj << "\n";
      // If underlying object is an Argument, check parent volatility status.
      if (auto arg = dyn_cast<Argument>(obj)) {
        auto found = parent_uncacheable_args.find(arg);
        if (found == parent_uncacheable_args.end()) {
            llvm::errs() << "parent_uncacheable_args:\n";
            for(auto& pair : parent_uncacheable_args) {
                llvm::errs() << " + " << *pair.first << ": " << pair.second << " of func " << pair.first->getParent()->getName() << "\n";
            }
            llvm::errs() << "could not find " << *arg << " of func " << arg->getParent()->getName() << " in parent_args_map\n";
        }
        assert(found != parent_uncacheable_args.end());
        if (found->second) {
          init_safe = false;
        }
        //llvm::errs() << " + ocs argument (safe=" << init_safe << ") " << *callsite_op << " object: " << *obj << " arg: " << *arg << "e\n";
      } else {
        // Pointer operands originating from call instructions that are not malloc/free are conservatively considered uncacheable.
        if (auto obj_op = dyn_cast<CallInst>(obj)) {
          Function* called = obj_op->getCalledFunction();
          if (auto castinst = dyn_cast<ConstantExpr>(obj_op->getCalledValue())) {
            if (castinst->isCast()) {
              if (auto fn = dyn_cast<Function>(castinst->getOperand(0))) {
                if (isAllocationFunction(*fn, TLI) || isDeallocationFunction(*fn, TLI)) {
                  called = fn;
                }
              }
            }
          }

          //Only assume that a pointer from a malloc/free is cacheable
          // TODO make interprocedural
          if (!isCertainMallocOrFree(called)) {
            init_safe = false;
          }

          //llvm::errs() << " + ocs callinst (safe=" << init_safe << ") " << *callsite_op << " object: " << *obj << " call: " << *obj_op << "\n";
        } else if (isa<LoadInst>(obj)) {
          // If obj is from a load instruction conservatively consider it uncacheable.
          init_safe = false;
          //llvm::errs() << " + ocs load (safe=" << init_safe << ") " << *callsite_op << " object: " << *obj << "\n";
        } else {
          // In absence of more information, assume that the underlying object for pointer operand is uncacheable in caller.
          init_safe = false;
          //llvm::errs() << " + ocs unknown (safe=" << init_safe << ") " << *callsite_op << " object: " << *obj << "\n";
        }
      }
      //llvm::errs() << " +++ safety " << init_safe << " of underlying object for callsite " << *callsite_op << " idx: " << i << " is " << *obj << "\n";
      // TODO(TFK): Also need to check whether underlying object is traced to load / non-allocating-call instruction.
      args_safe.push_back(init_safe);
  }

  // Second, we check for memory modifications that can occur in the continuation of the
  //   callee inside the parent function.
  for (inst_iterator I = inst_begin(*gutils->oldFunc), E = inst_end(*gutils->oldFunc); I != E; ++I) {
    Instruction* inst = &*I;
    assert(inst->getParent()->getParent() == callsite_op->getParent()->getParent());

    if (inst == callsite_op) continue;

      // If the "inst" does not dominate "callsite_op" then we cannot prove that
      //   "inst" happens before "callsite_op". If "inst" modifies an argument of the call,
      //   then that call needs to consider the argument uncacheable.
      // To correctly handle case where inst == callsite_op, we need to look at next instruction after callsite_op.
      if (!gutils->OrigDT.dominates(inst, callsite_op)) {
        //llvm::errs() << "Instruction " << *inst << " DOES NOT dominates " << *callsite_op << "\n";
        // Consider Store Instructions.
        if (auto op = dyn_cast<StoreInst>(inst)) {
          for (unsigned i = 0; i < args.size(); i++) {
            // If the modification flag is set, then this instruction may modify the $i$th argument of the call.
            if (!llvm::isModSet(AA.getModRefInfo(op, MemoryLocation::getForArgument(callsite_op, i, TLI)))) {
              //llvm::errs() << "Instruction " << *op << " is NoModRef with call argument " << *args[i] << "\n";
            } else {
              //llvm::errs() << "Instruction " << *op << " is maybe ModRef with call argument " << *args[i] << "\n";
              args_safe[i] = false;
            }
          }
        }

        // Consider Call Instructions.
        if (auto op = dyn_cast<CallInst>(inst)) {
          //llvm::errs() << "OP is call inst: " << *op << "\n";
          // Ignore memory allocation functions.
          Function* called = op->getCalledFunction();
          if (auto castinst = dyn_cast<ConstantExpr>(op->getCalledValue())) {
            if (castinst->isCast()) {
              if (auto fn = dyn_cast<Function>(castinst->getOperand(0))) {
                if (isAllocationFunction(*fn, TLI) || isDeallocationFunction(*fn, TLI)) {
                  called = fn;
                }
              }
            }
          }
          if (isCertainMallocOrFree(called)) {
            //llvm::errs() << "OP is certain malloc or free: " << *op << "\n";
            continue;
          }

          // For all the arguments, perform same check as for Stores, but ignore non-pointer arguments.
          for (unsigned i = 0; i < args.size(); i++) {
            if (!args[i]->getType()->isPointerTy()) continue;  // Ignore non-pointer arguments.
            if (!llvm::isModSet(AA.getModRefInfo(op, MemoryLocation::getForArgument(callsite_op, i, TLI)))) {
              //llvm::errs() << "Instruction " << *op << " is NoModRef with call argument " << *args[i] << "\n";
            } else {
              //llvm::errs() << "Instruction " << *op << " is maybe ModRef with call argument " << *args[i] << "\n";
              args_safe[i] = false;
            }
          }
        }
      } else {
        //llvm::errs() << "Instruction " << *inst << " DOES dominates " << *callsite_op << "\n";
      }
  }

  std::map<Argument*, bool> uncacheable_args;
  //llvm::errs() << "CallInst: " << *callsite_op<< "CALL ARGUMENT INFO: \n";
  if (callsite_op->getCalledFunction()) {

  auto arg = callsite_op->getCalledFunction()->arg_begin();
  for (unsigned i = 0; i < args.size(); i++) {
    uncacheable_args[arg] = !args_safe[i];
    //llvm::errs() << "callArg: " << *args[i] << " arg:" << *arg << " STATUS: " << args_safe[i] << "\n";
    arg++;
  }

  }
  return uncacheable_args;
}

// Given a function and the arguments passed to it by its caller that are uncacheable (_uncacheable_args) compute
//   the set of uncacheable arguments for each callsite inside the function. A pointer argument is uncacheable at
//   a callsite if the memory pointed to might be modified after that callsite.
std::map<CallInst*, const std::map<Argument*, bool> > compute_uncacheable_args_for_callsites(
    Function* F, DominatorTree &DT, TargetLibraryInfo &TLI, AAResults& AA, GradientUtils* gutils,
    const std::map<Argument*, bool> uncacheable_args) {
  std::map<CallInst*, const std::map<Argument*, bool> > uncacheable_args_map;

  for (inst_iterator I = inst_begin(*gutils->oldFunc), E = inst_end(*gutils->oldFunc); I != E; ++I) {
    Instruction& inst = *I;
      if (auto op = dyn_cast<CallInst>(&inst)) {

        // We do not need uncacheable args for intrinsic functions. So skip such callsites.
        if(isa<IntrinsicInst>(&inst)) {
          continue;
        }

        /*
        // We do not need uncacheable args for memory allocation functions. So skip such callsites.
        Function* called = op->getCalledFunction();
        if (auto castinst = dyn_cast<ConstantExpr>(op->getCalledValue())) {
          if (castinst->isCast()) {
            if (auto fn = dyn_cast<Function>(castinst->getOperand(0))) {
              if (isAllocationFunction(*fn, TLI) || isDeallocationFunction(*fn, TLI)) {
                called = fn;
              }
            }
          }
        }
        if (isCertainMallocOrFree(called)) {
          continue;
        }*/

        // For all other calls, we compute the uncacheable args for this callsite.
        uncacheable_args_map.insert(std::pair<CallInst*, const std::map<Argument*, bool>>(op, compute_uncacheable_args_for_one_callsite(op,
            DT, TLI, AA, gutils, uncacheable_args)));
      }
  }
  return uncacheable_args_map;
}

std::string to_string(const std::map<Argument*, bool>& us) {
    std::string s = "{";
    for(auto y : us) s += y.first->getName().str() + "@" + y.first->getParent()->getName().str() + ":" + std::to_string(y.second) + ",";
    return s + "}";
}

// Determine if a value is needed in the reverse pass. We only use this logic in the top level function right now.
bool is_value_needed_in_reverse(TypeResults &TR, const GradientUtils* gutils, Value* inst, bool topLevel, std::map<Value*, bool> seen = {}) {
  if (seen.find(inst) != seen.end()) return seen[inst];

  //Inductively claim we aren't needed (and try to find contradiction)
  seen[inst] = false;

  //Consider all users of this value, do any of them need this in the reverse?
  for (auto use : inst->users()) {
    if (use == inst) continue;

    Instruction* user = dyn_cast<Instruction>(use);

    // One may need to this value in the computation of loop bounds/comparisons/etc (which even though not active -- will be used for the reverse pass)
    //   We only need this if we're not doing the combined forward/reverse since otherwise it will use the local cache (rather than save for a separate backwards cache)
    if (!topLevel) {
        //Proving that none of the uses (or uses' uses) are used in control flow allows us to safely not do this load

        if (isa<BranchInst>(use) || isa<SwitchInst>(use) || isa<CallInst>(use)) {
            //llvm::errs() << " had to use in reverse since used in branch/switch " << *inst << " use: " << *use << "\n";
            return seen[inst] = true;
        }

        if (is_value_needed_in_reverse(TR, gutils, user, topLevel, seen)) {
            //llvm::errs() << " had to use in reverse since used in " << *inst << " use: " << *use << "\n";
            return seen[inst] = true;
        }
    }
    //llvm::errs() << " considering use : " << *user << " of " <<  *inst << "\n";

    //The following are types we know we don't need to compute adjoints

    // A pointer is only needed in the reverse pass if its non-store uses are needed in the reverse pass
    //   Moreover, we only need this pointer in the reverse pass if all of its non-store users are not already cached for the reverse pass
    if (!inst->getType()->isFPOrFPVectorTy() && TR.query(inst)[{}].isPossiblePointer()) {
        //continue;
        bool unknown = true;
        for (auto zu : inst->users()) {
            // Stores to a pointer are not needed for the reverse pass
            if (auto si = dyn_cast<StoreInst>(zu)) {
                if (si->getPointerOperand() == inst) {
                    continue;
                }
            }

            if (isa<LoadInst>(zu) || isa<CastInst>(zu) || isa<PHINode>(zu)) {
                if (is_value_needed_in_reverse(TR, gutils, zu, topLevel, seen)) {
                    //llvm::errs() << " had to use in reverse since sub use " << *zu << " of " << *inst << "\n";
                    return seen[inst] = true;
                }
                continue;
            }

            if (isa<CallInst>(zu)) {
                //llvm::errs() << " had to use in reverse since call use " << *zu << " of " << *inst << "\n";
                return seen[inst] = true;
            }

            /*
            if (auto gep = dyn_cast<GetElementPtrInst>(zu)) {
                for(auto &idx : gep->indices()) {
                    if (idx == inst) {
                        return seen[inst] = true;
                    }
                }
                if (gep->getPointerOperand() == inst && is_value_needed_in_reverse(gutils, gep, topLevel, seen)) {
                    //llvm::errs() << " had to use in reverse since sub gep use " << *zu << " of " << *inst << "\n";
                    return seen[inst] = true;
                }
                continue;
            }
            */

            //TODO add handling of call and allow interprocedural
            //llvm::errs() << " unknown pointer use " << *zu << " of " << *inst << "\n";
            unknown = true;
        }
        if (!unknown)
          continue;
          //return seen[inst] = false;
    }

    if (isa<LoadInst>(user) || isa<CastInst>(user) || isa<PHINode>(user)) {
        if (!is_value_needed_in_reverse(TR, gutils, user, topLevel, seen)) {
            continue;
        }
    }

    if (auto op = dyn_cast<BinaryOperator>(user)) {
      if (op->getOpcode() == Instruction::FAdd || op->getOpcode() == Instruction::FSub) {
        continue;
      }
    }

    //We don't need only the indices of a GEP to compute the adjoint of a GEP
    if (auto gep = dyn_cast<GetElementPtrInst>(user)) {
        bool indexuse = false;
        for(auto &idx : gep->indices()) {
            if (idx == inst) {
                indexuse = true;
            }
        }
        if (!indexuse) continue;
    }

    //We don't need any of the input operands to compute the adjoint of a store instance
    if (isa<StoreInst>(use)) {
        continue;
    }

    if (isa<CmpInst>(use) || isa<BranchInst>(use) || isa<CastInst>(use) || isa<PHINode>(use) || isa<ReturnInst>(use) || isa<FPExtInst>(use) ||
        (isa<SelectInst>(use) && cast<SelectInst>(use)->getCondition() != inst) ||
        (isa<InsertElementInst>(use) && cast<InsertElementInst>(use)->getOperand(2) != inst) ||
        (isa<ExtractElementInst>(use) && cast<ExtractElementInst>(use)->getIndexOperand() != inst)
        //isa<LoadInst>(use) || (isa<SelectInst>(use) && cast<SelectInst>(use)->getCondition() != inst) //TODO remove load?
        //|| isa<SwitchInst>(use) || isa<ExtractElement>(use) || isa<InsertElementInst>(use) || isa<ShuffleVectorInst>(use) ||
        //isa<ExtractValueInst>(use) || isa<AllocaInst>(use)
        /*|| isa<StoreInst>(use)*/){
      continue;
    }

    //! Note it is important that return check comes before this as it may not have a new instruction

    // nonconstant
    if (isa<CallInst>(user) && (gutils->originalToNewFn.find(user) == gutils->originalToNewFn.end() || isa<ExtractValueInst>(gutils->getNewFromOriginal(user)))) {

    } else if (gutils->isConstantInstruction(gutils->getNewFromOriginal(user))) {
        //llvm::errs() << " skipping constant use : " << *user << " of " <<  *inst << "\n";
        continue;
    }

    //llvm::errs() << " + must have in reverse from considering use : " << *user << " of " <<  *inst << "\n";
    return seen[inst] = true;
  }
  return false;
}

//! assuming not top level
std::pair<SmallVector<Type*,4>,SmallVector<Type*,4>> getDefaultFunctionTypeForAugmentation(FunctionType* called, bool returnUsed, bool differentialReturn) {
    SmallVector<Type*, 4> args;
    SmallVector<Type*, 4> outs;
    for(auto &argType : called->params()) {
        args.push_back(argType);

        if (!argType->isFPOrFPVectorTy()) {
            args.push_back(argType);
        }
    }

    auto ret = called->getReturnType();
    outs.push_back(Type::getInt8PtrTy(called->getContext()));
    if (!ret->isVoidTy() && !ret->isEmptyTy()) {
        if (returnUsed) {
            outs.push_back(ret);
        }
        if (differentialReturn && !ret->isFPOrFPVectorTy()) {
            outs.push_back(ret);
        }
    }

    return std::pair<SmallVector<Type*,4>,SmallVector<Type*,4>>(args, outs);
}

//! assuming not top level
std::pair<SmallVector<Type*,4>,SmallVector<Type*,4>> getDefaultFunctionTypeForGradient(FunctionType* called, bool differentialReturn) {
    SmallVector<Type*, 4> args;
    SmallVector<Type*, 4> outs;
    for(auto &argType : called->params()) {
        args.push_back(argType);

        if (!argType->isFPOrFPVectorTy()) {
            args.push_back(argType);
        } else {
            outs.push_back(argType);
        }
    }

    auto ret = called->getReturnType();

    if (!ret->isVoidTy() && !ret->isEmptyTy()) {
        if (differentialReturn) {
            args.push_back(ret);
        }
    }

    return std::pair<SmallVector<Type*,4>,SmallVector<Type*,4>>(args, outs);
}

void handleAugmentedCallInst(TypeResults &TR, CallInst* op, GradientUtils* const gutils, const std::map<Argument*, bool> uncacheable_args, const SmallPtrSetImpl<Instruction*> &returnuses, std::function<unsigned(Instruction*, CacheType)> getIndex, std::map<const llvm::CallInst*, const AugmentedReturn*> &subaugmentations);

enum class DerivativeMode {
    Forward,
    Reverse,
    Both
};

std::string to_string(DerivativeMode mode) {
  switch(mode) {
    case DerivativeMode::Forward: return "Forward";
    case DerivativeMode::Reverse: return "Reverse";
    case DerivativeMode::Both: return "Both";
  }
  llvm_unreachable("illegal derivative mode");
}

template<class AugmentedReturnType = AugmentedReturn*>
class DerivativeMaker : public llvm::InstVisitor<DerivativeMaker<AugmentedReturnType>> {
public:
  DerivativeMode mode;
  GradientUtils *gutils;
  TypeResults &TR;
  std::function<unsigned(Instruction*, CacheType)> getIndex;
  const std::map<CallInst*, const std::map<Argument*, bool> > uncacheable_args_map;
  const SmallPtrSetImpl<Instruction*> *returnuses;
  AugmentedReturnType augmentedReturn;
  std::vector<Instruction*> *fakeTBAA;
  DerivativeMaker(DerivativeMode mode, GradientUtils *gutils, TypeResults &TR, std::function<unsigned(Instruction*, CacheType)> getIndex,
    const std::map<CallInst*, const std::map<Argument*, bool> > uncacheable_args_map, const SmallPtrSetImpl<Instruction*> *returnuses, AugmentedReturnType augmentedReturn,
    std::vector<Instruction*>* fakeTBAA
    ) : mode(mode), gutils(gutils), TR(TR),
        getIndex(getIndex), uncacheable_args_map(uncacheable_args_map),
        returnuses(returnuses), augmentedReturn(augmentedReturn), fakeTBAA(fakeTBAA) {

    assert(TR.info.function == gutils->oldFunc);
    for(auto &pair : TR.analysis.analyzedFunctions.find(TR.info)->second.analysis) {
        if (auto in = dyn_cast<Instruction>(pair.first)) {
          if (in->getParent()->getParent() != gutils->oldFunc) {
            llvm::errs() << "inf: " << *in->getParent()->getParent() << "\n";
            llvm::errs() << "gutils->oldFunc: " << *gutils->oldFunc << "\n";
            llvm::errs() << "in: " << *in << "\n";
          }
          assert(in->getParent()->getParent() == gutils->oldFunc);
        }
    }


  }

  void visitInstruction(llvm::Instruction& inst) {
    //TODO explicitly handle all instructions rather than using the catch all below
    if (mode == DerivativeMode::Forward) return;

    llvm::errs() << *gutils->oldFunc << "\n";
    llvm::errs() << *gutils->newFunc << "\n";
    llvm::errs() << "in mode: " << to_string(mode) << "\n";
    llvm::errs() << "cannot handle unknown instruction\n" << inst;
    report_fatal_error("unknown value");
  }

  void visitAllocaInst(llvm::AllocaInst &I) {}

  void visitLoadInst(llvm::LoadInst &LI) {
    bool constantval = gutils->isConstantValue(&LI);
    auto alignment = LI.getAlignment();
    Value* ptr  = LI.getPointerOperand();
    Instruction* orig = gutils->getOriginal(&LI);
    BasicBlock* parent = LI.getParent();
    Type*  type = LI.getType();

    //! Store inverted pointer loads that need to be cached for use in reverse pass
    if (!type->isEmptyTy() && !type->isFPOrFPVectorTy() && TR.query(gutils->getOriginal(&LI))[{}].isPossiblePointer()) {
      PHINode* placeholder = cast<PHINode>(gutils->invertedPointers[&LI]);
      assert(placeholder->getType() == type);
      gutils->invertedPointers.erase(&LI);

      //TODO consider optimizing when you know it isnt a pointer and thus don't need to store
      if (!constantval) {
        IRBuilder<> BuilderZ(placeholder);
        Value* newip = nullptr;

        switch(mode) {

          case DerivativeMode::Forward:
          case DerivativeMode::Both:{
            newip = gutils->invertPointerM(&LI, BuilderZ);
            assert(newip->getType() == type);

            if (mode == DerivativeMode::Forward && gutils->can_modref_map->find(orig)->second) {
              gutils->addMalloc(BuilderZ, newip, getIndex(orig, CacheType::Shadow));
            }
            placeholder->replaceAllUsesWith(newip);
            gutils->erase(placeholder);
            gutils->invertedPointers[&LI] = newip;
            break;
          }

          case DerivativeMode::Reverse:{
            //only make shadow where caching needed
            if (gutils->can_modref_map->find(orig)->second) {
              newip = gutils->addMalloc(BuilderZ, placeholder, getIndex(orig, CacheType::Shadow));
              assert(newip->getType() == type);
              gutils->invertedPointers[&LI] = newip;
            } else {
              newip = gutils->invertPointerM(&LI, BuilderZ);
              assert(newip->getType() == type);
              placeholder->replaceAllUsesWith(newip);
              gutils->erase(placeholder);
              gutils->invertedPointers[&LI] = newip;
            }
            break;
          }
        }

      } else {
        gutils->erase(placeholder);
      }
    }

    // Allow forcing cache reads to be on or off using flags.
    assert(!(cache_reads_always && cache_reads_never) && "Both cache_reads_always and cache_reads_never are true. This doesn't make sense.");

    Instruction* inst = &LI;

    //! Store loads that need to be cached for use in reverse pass
    if (cache_reads_always || (!cache_reads_never && gutils->can_modref_map->find(orig)->second && is_value_needed_in_reverse(TR, gutils, gutils->getOriginal(&LI), /*toplevel*/mode == DerivativeMode::Both))) {
      IRBuilder<> BuilderZ(LI.getNextNode());
      auto tbaa = inst->getMetadata(LLVMContext::MD_tbaa);

      inst = gutils->addMalloc(BuilderZ, &LI, getIndex(orig, CacheType::Self));
      assert(inst->getType() == type);

      if (mode == DerivativeMode::Reverse) {
        assert(inst != &LI);
        inst->setMetadata("enzyme_activity_inst", MDNode::get(inst->getContext(), {MDString::get(inst->getContext(), "const")}));
        if (!constantval) {
          gutils->nonconstant_values.insert(inst);
          gutils->nonconstant.insert(inst);
        }
        inst->setMetadata("enzyme_activity_value", MDNode::get(inst->getContext(), {MDString::get(inst->getContext(), constantval ? "const" : "active")}));
        if (tbaa) {
          inst->setMetadata(LLVMContext::MD_tbaa, tbaa);
          assert(fakeTBAA);
          fakeTBAA->push_back(inst);
        }
        gutils->originalInstructions.insert(inst);
      } else {
        assert(inst == &LI);
      }
    }

    if (mode == DerivativeMode::Forward) return;

    if (constantval) return;

    if (nonmarkedglobals_inactiveloads) {
      //Assume that non enzyme_shadow globals are inactive
      //  If we ever store to a global variable, we will error if it doesn't have a shadow
      //  This allows functions who only read global memory to have their derivative computed
      //  Note that this is too aggressive for general programs as if the global aliases with an argument something that is written to, then we will have a logical error
      if (auto arg = dyn_cast<GlobalVariable>(ptr)) {
        if (!hasMetadata(arg, "enzyme_shadow")) {
          return;
        }
      }
    }

    if (type->isFPOrFPVectorTy() || (type->isIntOrIntVectorTy() && TR.intType(orig).isFloat() )) {
      IRBuilder<> Builder2 = getReverseBuilder(parent);
      auto prediff = diffe(inst, Builder2);
      setDiffe(inst, Constant::getNullValue(type), Builder2);
          //llvm::errs() << "  + doing load propagation: orig:" << *oorig << " inst:" << *inst << " prediff: " << *prediff << " inverted_operand: " << *inverted_operand << "\n";

      Value* inverted_operand = gutils->invertPointerM(ptr, Builder2);
      assert(inverted_operand);
      ((DiffeGradientUtils*)gutils)->addToInvertedPtrDiffe(inverted_operand, prediff, Builder2, alignment);
    }
  }

  void visitStoreInst(llvm::StoreInst &SI) {

    Value* ptr = SI.getPointerOperand();
    Value* val = SI.getValueOperand();
    Type* valType = val->getType();

    if (gutils->isConstantValue(ptr)) return;

    //TODO allow recognition of other types that could contain pointers [e.g. {void*, void*} or <2 x i64> ]
    StoreInst* ts = nullptr;

    auto storeSize = gutils->newFunc->getParent()->getDataLayout().getTypeSizeInBits(valType) / 8;

    //! Storing a floating point value
    Type* FT = nullptr;
    if (valType->isFPOrFPVectorTy()) {
      FT = valType->getScalarType();
    } else if (!valType->isPointerTy()) {
      FT = TR.firstPointer(storeSize, gutils->getOriginal(ptr), /*errifnotfound*/true, /*pointerIntSame*/true).isFloat();
    }

    if (FT) {
      //! Only need to update the reverse function
      if (mode == DerivativeMode::Reverse || mode == DerivativeMode::Both) {
        IRBuilder<> Builder2 = getReverseBuilder(SI.getParent());

        if (gutils->isConstantValue(val)) {
          ts = setPtrDiffe(ptr, Constant::getNullValue(valType), Builder2);
        } else {
          auto dif1 = Builder2.CreateLoad(gutils->invertPointerM(ptr, Builder2));
          dif1->setAlignment(SI.getAlignment());
          ts = setPtrDiffe(ptr, Constant::getNullValue(valType), Builder2);
          addToDiffe(val, dif1, Builder2, FT);
        }
      }

    //! Storing an integer or pointer
    } else {
      //if (gutils->isConstantValue(ptr)) return;
      //! Only need to update the forward function
      if (mode == DerivativeMode::Forward || mode == DerivativeMode::Both) {
        IRBuilder <> storeBuilder(&SI);

        Value* valueop = nullptr;

        //Fallback mechanism, TODO check
        if (gutils->isConstantValue(val)) {
            valueop = val; //Constant::getNullValue(op->getValueOperand()->getType());
        } else {
            valueop = gutils->invertPointerM(val, storeBuilder);
        }
        ts = setPtrDiffe(ptr, valueop, storeBuilder);
      }

    }

    if (ts) {
      ts->setAlignment(SI.getAlignment());
      ts->setVolatile(SI.isVolatile());
      ts->setOrdering(SI.getOrdering());
      ts->setSyncScopeID(SI.getSyncScopeID());
    }
  }

  void visitGetElementPtrInst(llvm::GetElementPtrInst &gep) {}

  void visitPHINode(llvm::PHINode& phi) {}

  void visitTruncInst(llvm::TruncInst &I) {}

  void visitZExtInst(llvm::ZExtInst &I) {}

  void visitSExtInst(llvm::SExtInst &I) {}

  void visitAddrSpaceCastInst(llvm::AddrSpaceCastInst &I) {}

  void visitFPToUIInst(llvm::FPToUIInst &I) {}

  void visitFPToSIInst(llvm::FPToSIInst &I) {}

  void visitUIToFPInst(llvm::UIToFPInst &I) {}

  void visitSIToFPInst(llvm::SIToFPInst &I) {}

  void visitPtrToIntInst(llvm::PtrToIntInst &I) {}

  void visitIntToPtrInst(llvm::IntToPtrInst &I) {}

  void visitBitCastInst(llvm::BitCastInst &I) {}

  void visitSelectInst(llvm::SelectInst &I) {}

  void visitExtractElementInst(llvm::ExtractElementInst &I) {}

  void visitInsertElementInst(llvm::InsertElementInst &I) {}

  void visitShuffleVectorInst(llvm::ShuffleVectorInst &I) {}

  void visitExtractValueInst(llvm::ExtractValueInst &I) {}

  void visitInsertValueInst(llvm::InsertValueInst &I) {}

  inline IRBuilder<> getReverseBuilder(BasicBlock* BB) {
    auto BB2 = gutils->reverseBlocks[BB];
    if (!BB2) {
      llvm::errs() << "oldFunc: " << *gutils->oldFunc << "\n";
      llvm::errs() << "newFunc: " << *gutils->newFunc << "\n";
      llvm::errs() << "could not invert " << *BB;
    }
    assert(BB2);

    IRBuilder<> Builder2(BB2);
    //if (BB2->size() > 0) {
    //    Builder2.SetInsertPoint(BB2->getFirstNonPHI());
    //}
    Builder2.setFastMathFlags(getFast());
    return Builder2;
  }

  Value* diffe(Value* val, IRBuilder<> &Builder) {
    assert(mode == DerivativeMode::Reverse || mode == DerivativeMode::Both);
    return ((DiffeGradientUtils*)gutils)->diffe(val, Builder);
  }

  void setDiffe(Value* val, Value* dif, IRBuilder<> &Builder) {
    assert(mode == DerivativeMode::Reverse || mode == DerivativeMode::Both);
    ((DiffeGradientUtils*)gutils)->setDiffe(val, dif, Builder);
  }

  StoreInst* setPtrDiffe(Value* val, Value* dif, IRBuilder<> &Builder) {
    return gutils->setPtrDiffe(val, dif, Builder);
  }

  std::vector<SelectInst*> addToDiffe(Value* val, Value* dif, IRBuilder<> &Builder, Type* T) {
    assert(mode == DerivativeMode::Reverse || mode == DerivativeMode::Both);
    return ((DiffeGradientUtils*)gutils)->addToDiffe(val, dif, Builder, T);
  }

  Value* lookup(Value* val, IRBuilder<> &Builder) {
    return gutils->lookupM(val, Builder);
  }

  void visitBinaryOperator(llvm::BinaryOperator &BO) {
    if (mode != DerivativeMode::Reverse && mode != DerivativeMode::Both) return;

    if (gutils->isConstantValue(&BO)) return;

    if (BO.getType()->isIntOrIntVectorTy() && TR.intType(gutils->getOriginal(&BO), /*errifnotfound*/false) == IntType::Pointer) {
      return;
    }

    IRBuilder<> Builder2 = getReverseBuilder(BO.getParent());

    Value* dif0 = nullptr;
    Value* dif1 = nullptr;
    Value* idiff = diffe(&BO, Builder2);

    Type* addingType = BO.getType();

    switch(BO.getOpcode()) {
      case Instruction::FMul:{
        if (!gutils->isConstantValue(BO.getOperand(0)))
          dif0 = Builder2.CreateFMul(idiff, lookup(BO.getOperand(1), Builder2), "m0diffe"+BO.getOperand(0)->getName());
        if (!gutils->isConstantValue(BO.getOperand(1)))
          dif1 = Builder2.CreateFMul(idiff, lookup(BO.getOperand(0), Builder2), "m1diffe"+BO.getOperand(1)->getName());
        break;
      }
      case Instruction::FAdd:{
        if (!gutils->isConstantValue(BO.getOperand(0)))
          dif0 = idiff;
        if (!gutils->isConstantValue(BO.getOperand(1)))
          dif1 = idiff;
        break;
      }
      case Instruction::FSub:{
        if (!gutils->isConstantValue(BO.getOperand(0)))
          dif0 = idiff;
        if (!gutils->isConstantValue(BO.getOperand(1)))
          dif1 = Builder2.CreateFNeg(idiff);
        break;
      }
      case Instruction::FDiv:{
        if (!gutils->isConstantValue(BO.getOperand(0)))
          dif0 = Builder2.CreateFDiv(idiff, lookup(BO.getOperand(1), Builder2), "d0diffe"+BO.getOperand(0)->getName());
        if (!gutils->isConstantValue(BO.getOperand(1)))
          dif1 = Builder2.CreateFNeg(
            Builder2.CreateFDiv(
              Builder2.CreateFMul(idiff, lookup(&BO, Builder2)),
              lookup(BO.getOperand(1), Builder2))
          );
        break;
      }
      case Instruction::LShr:{
        if (!gutils->isConstantValue(BO.getOperand(0))) {
          if (auto ci = dyn_cast<ConstantInt>(BO.getOperand(1))) {
            if (Type* flt = TR.intType(gutils->getOriginal(BO.getOperand(0)), /*necessary*/false).isFloat()) {
              auto bits = gutils->newFunc->getParent()->getDataLayout().getTypeAllocSizeInBits(flt);
              if (ci->getSExtValue() >= bits && ci->getSExtValue() % bits == 0) {
                dif0 = Builder2.CreateShl(idiff, ci);
                addingType = flt;
                goto done;
              }
            }
          }
        }
        goto def;
      }
      default:def:;
        llvm::errs() << *gutils->newFunc << "\n";
        llvm::errs() << "cannot handle unknown binary operator: " << BO << "\n";
        report_fatal_error("unknown binary operator");
    }

    done:;
    if (dif0 || dif1) setDiffe(&BO, Constant::getNullValue(BO.getType()), Builder2);
    if (dif0) addToDiffe(BO.getOperand(0), dif0, Builder2, addingType);
    if (dif1) addToDiffe(BO.getOperand(1), dif1, Builder2, addingType);
  }

  void visitCallInst(llvm::CallInst &call);

  void visitMemSetInst(llvm::MemSetInst &MS) {
    if (gutils->isConstantInstruction(&MS)) return;

    //TODO this should 1) assert that the value being meset is constant
    //                 2) duplicate the memset for the inverted pointer

    if (!gutils->isConstantValue(MS.getOperand(1))) {
        llvm::errs() << "couldn't handle non constant inst in memset to propagate differential to\n" << MS;
        report_fatal_error("non constant in memset");
    }

    if (mode == DerivativeMode::Forward || mode == DerivativeMode::Both) {
      IRBuilder <>BuilderZ(&MS);

      SmallVector<Value*, 4> args;
      if (!gutils->isConstantValue(MS.getOperand(0))) {
        args.push_back(gutils->invertPointerM(MS.getOperand(0), BuilderZ));
      } else {
        //If constant destination then no operation needs doing
        return;
        //args.push_back(gutils->lookupM(MS.getOperand(0), BuilderZ));
      }

      args.push_back(gutils->lookupM(MS.getOperand(1), BuilderZ));
      args.push_back(gutils->lookupM(MS.getOperand(2), BuilderZ));
      args.push_back(gutils->lookupM(MS.getOperand(3), BuilderZ));

      Type *tys[] = {args[0]->getType(), args[2]->getType()};
      auto cal = BuilderZ.CreateCall(Intrinsic::getDeclaration(MS.getParent()->getParent()->getParent(), Intrinsic::memset, tys), args);
      cal->setAttributes(MS.getAttributes());
      cal->setCallingConv(MS.getCallingConv());
      cal->setTailCallKind(MS.getTailCallKind());
    }

    if (mode == DerivativeMode::Reverse || mode == DerivativeMode::Both) {
      //TODO consider what reverse pass memset should be
    }
  }

  void visitMemTransferInst(llvm::MemTransferInst& MTI) {
    if (gutils->isConstantInstruction(&MTI)) return;

    // copying into nullptr is invalid (not sure why it exists here), but we shouldn't do it in reverse pass or shadow
    if (isa<ConstantPointerNull>(MTI.getOperand(0)) || TR.query(gutils->getOriginal(MTI.getOperand(0)))[{}] == IntType::Anything) return;

    size_t size = 1;
    if (auto ci = dyn_cast<ConstantInt>(MTI.getOperand(2))) {
      size = ci->getLimitedValue();
    }

    auto tr = TR.query(gutils->getOriginal(MTI.getOperand(0)));

    //TODO note that we only handle memcpy/etc of ONE type (aka memcpy of {int, double} not allowed)

    //llvm::errs() << *gutils->oldFunc << "\n";
    //TR.dump();
    //llvm::errs() << "MIT: " << MTI << "|size: " << size << " dr: " << tr.str() << "\n";

    if (Type* secretty = TR.firstPointer(size, gutils->getOriginal(MTI.getOperand(0)), /*errifnotfound*/true, /*pointerIntSame*/true).isFloat()) {
      //no change to forward pass if represents floats
      if (mode == DerivativeMode::Reverse || mode == DerivativeMode::Both) {
        IRBuilder<> Builder2 = getReverseBuilder(MTI.getParent());
        SmallVector<Value*, 4> args;
        auto secretpt = PointerType::getUnqual(secretty);

        args.push_back(Builder2.CreatePointerCast(gutils->invertPointerM(MTI.getOperand(0), Builder2), secretpt));
        args.push_back(Builder2.CreatePointerCast(gutils->invertPointerM(MTI.getOperand(1), Builder2), secretpt));
        args.push_back(Builder2.CreateUDiv(lookup(MTI.getOperand(2), Builder2),

            ConstantInt::get(MTI.getOperand(2)->getType(), Builder2.GetInsertBlock()->getParent()->getParent()->getDataLayout().getTypeAllocSizeInBits(secretty)/8)
        ));
        unsigned dstalign = 0;
        if (MTI.paramHasAttr(0, Attribute::Alignment)) {
            dstalign = MTI.getParamAttr(0, Attribute::Alignment).getValueAsInt();
        }
        unsigned srcalign = 0;
        if (MTI.paramHasAttr(1, Attribute::Alignment)) {
            srcalign = MTI.getParamAttr(1, Attribute::Alignment).getValueAsInt();
        }

        auto dmemcpy = ( (MTI.getIntrinsicID() == Intrinsic::memcpy) ? getOrInsertDifferentialFloatMemcpy : getOrInsertDifferentialFloatMemmove)(*MTI.getParent()->getParent()->getParent(), secretpt, dstalign, srcalign);
        Builder2.CreateCall(dmemcpy, args);
      }
    } else {

      //if represents pointer or integer type then only need to modify forward pass with the copy
      if (mode == DerivativeMode::Forward || mode == DerivativeMode::Both) {
        //It is questionable how the following case would even occur, but if the dst is constant, we shouldn't do anything extra
        if (gutils->isConstantValue(MTI.getOperand(0))) return;

        SmallVector<Value*, 4> args;
        IRBuilder <>BuilderZ(&MTI);

        //If src is inactive, then we should copy from the regular pointer (i.e. suppose we are copying constant memory representing dimensions into a tensor)
        //  to ensure that the differential tensor is well formed for use OUTSIDE the derivative generation (as enzyme doesn't need this), we should also perform the copy
        //  onto the differential. Future Optimization (not implemented): If dst can never escape Enzyme code, we may omit this copy.
        //no need to update pointers, even if dst is active
        args.push_back(gutils->invertPointerM(MTI.getOperand(0), BuilderZ));

        if (!gutils->isConstantValue(MTI.getOperand(1)))
            args.push_back(gutils->invertPointerM(MTI.getOperand(1), BuilderZ));
        else
            args.push_back(MTI.getOperand(1));

        args.push_back(MTI.getOperand(2));
        args.push_back(MTI.getOperand(3));

        Type *tys[] = {args[0]->getType(), args[1]->getType(), args[2]->getType()};
        auto cal = BuilderZ.CreateCall(Intrinsic::getDeclaration(gutils->newFunc->getParent(), MTI.getIntrinsicID(), tys), args);
        cal->setAttributes(MTI.getAttributes());
        cal->setCallingConv(MTI.getCallingConv());
        cal->setTailCallKind(MTI.getTailCallKind());
      }
    }
  }

  void visitIntrinsicInst(llvm::IntrinsicInst &II) {
    if (mode == DerivativeMode::Forward) {
      switch(II.getIntrinsicID()) {
        case Intrinsic::stacksave:
        case Intrinsic::prefetch:
        case Intrinsic::stackrestore:
        case Intrinsic::dbg_declare:
        case Intrinsic::dbg_value:
        #if LLVM_VERSION_MAJOR > 6
        case Intrinsic::dbg_label:
        #endif
        case Intrinsic::dbg_addr:
        case Intrinsic::lifetime_start:
        case Intrinsic::lifetime_end:
        case Intrinsic::assume:
        case Intrinsic::fabs:
        case Intrinsic::x86_sse_max_ss:
        case Intrinsic::x86_sse_max_ps:
        case Intrinsic::maxnum:
        case Intrinsic::x86_sse_min_ss:
        case Intrinsic::x86_sse_min_ps:
        case Intrinsic::minnum:
        case Intrinsic::log:
        case Intrinsic::log2:
        case Intrinsic::log10:
        case Intrinsic::exp:
        case Intrinsic::exp2:
        case Intrinsic::pow:
        case Intrinsic::sin:
        case Intrinsic::cos:
        case Intrinsic::floor:
        case Intrinsic::ceil:
        case Intrinsic::trunc:
        case Intrinsic::rint:
        case Intrinsic::nearbyint:
        case Intrinsic::round:
        case Intrinsic::sqrt:
          return;
        default:
          if (gutils->isConstantInstruction(&II)) return;
          llvm::errs() << *gutils->oldFunc << "\n";
          llvm::errs() << *gutils->newFunc << "\n";
          llvm::errs() << "cannot handle (augmented) unknown intrinsic\n" << II;
          report_fatal_error("(augmented) unknown intrinsic");
      }
    }

    if (mode == DerivativeMode::Both || mode == DerivativeMode::Reverse) {
      IRBuilder<> Builder2 = getReverseBuilder(II.getParent());
      Module* M = II.getParent()->getParent()->getParent();

      Value* vdiff = nullptr;
      if (!gutils->isConstantValue(&II)) {
        vdiff = diffe(&II, Builder2);
        setDiffe(&II, Constant::getNullValue(II.getType()), Builder2);
      }

      switch(II.getIntrinsicID()) {

        case Intrinsic::assume:
        case Intrinsic::stacksave:
        case Intrinsic::prefetch:
        case Intrinsic::stackrestore:
        case Intrinsic::dbg_declare:
        case Intrinsic::dbg_value:
        #if LLVM_VERSION_MAJOR > 6
        case Intrinsic::dbg_label:
        #endif
        case Intrinsic::dbg_addr:
        case Intrinsic::floor:
        case Intrinsic::ceil:
        case Intrinsic::trunc:
        case Intrinsic::rint:
        case Intrinsic::nearbyint:
        case Intrinsic::round:
          //Derivative of these is zero and requires no modification
          return;

        case Intrinsic::lifetime_start:{
          if (gutils->isConstantInstruction(&II)) return;
          SmallVector<Value*, 2> args = {lookup(II.getOperand(0), Builder2), lookup(II.getOperand(1), Builder2)};
          Type *tys[] = {args[1]->getType()};
          auto cal = Builder2.CreateCall(Intrinsic::getDeclaration(M, Intrinsic::lifetime_end, tys), args);
          cal->setAttributes(II.getAttributes());
          cal->setCallingConv(II.getCallingConv());
          cal->setTailCallKind(II.getTailCallKind());
          return;
        }

        case Intrinsic::lifetime_end:{
          gutils->erase(&II);
          return;
        }

        case Intrinsic::sqrt: {
          if (vdiff && !gutils->isConstantValue(II.getOperand(0))) {
            Value* dif0 = Builder2.CreateBinOp(Instruction::FDiv,
              Builder2.CreateFMul(ConstantFP::get(II.getType(), 0.5), vdiff),
              lookup(&II, Builder2)
            );
            addToDiffe(II.getOperand(0), dif0, Builder2, II.getType());
          }
          return;
        }

        case Intrinsic::fabs: {
          if (!gutils->isConstantValue(&II) && !gutils->isConstantValue(II.getOperand(0))) {
            Value* cmp  = Builder2.CreateFCmpOLT(lookup(II.getOperand(0), Builder2), ConstantFP::get(II.getOperand(0)->getType(), 0));
            Value* dif0 = Builder2.CreateFMul(Builder2.CreateSelect(cmp, ConstantFP::get(II.getOperand(0)->getType(), -1), ConstantFP::get(II.getOperand(0)->getType(), 1)), vdiff);
            addToDiffe(II.getOperand(0), dif0, Builder2, II.getType());
          }
          return;
        }

        case Intrinsic::x86_sse_max_ss:
        case Intrinsic::x86_sse_max_ps:
        case Intrinsic::maxnum: {
          if (!gutils->isConstantValue(&II) && !gutils->isConstantValue(II.getOperand(0))) {
            Value* cmp  = Builder2.CreateFCmpOLT(lookup(II.getOperand(0), Builder2), lookup(II.getOperand(1), Builder2));
            Value* dif0 = Builder2.CreateSelect(cmp, ConstantFP::get(II.getOperand(0)->getType(), 0), vdiff);
            addToDiffe(II.getOperand(0), dif0, Builder2, II.getType());
          }
          if (!gutils->isConstantValue(&II) && !gutils->isConstantValue(II.getOperand(1))) {
            Value* cmp  = Builder2.CreateFCmpOLT(lookup(II.getOperand(0), Builder2), lookup(II.getOperand(1), Builder2));
            Value* dif1 = Builder2.CreateSelect(cmp, vdiff, ConstantFP::get(II.getOperand(0)->getType(), 0));
            addToDiffe(II.getOperand(1), dif1, Builder2, II.getType());
          }
          return;
        }

        case Intrinsic::x86_sse_min_ss:
        case Intrinsic::x86_sse_min_ps:
        case Intrinsic::minnum: {
          if (!gutils->isConstantValue(&II) && !gutils->isConstantValue(II.getOperand(0))) {
            Value* cmp = Builder2.CreateFCmpOLT(lookup(II.getOperand(0), Builder2), lookup(II.getOperand(1), Builder2));
            Value* dif0 = Builder2.CreateSelect(cmp, vdiff, ConstantFP::get(II.getOperand(0)->getType(), 0));
            addToDiffe(II.getOperand(0), dif0, Builder2, II.getType());
          }
          if (!gutils->isConstantValue(&II) && !gutils->isConstantValue(II.getOperand(1))) {
            Value* cmp = Builder2.CreateFCmpOLT(lookup(II.getOperand(0), Builder2), lookup(II.getOperand(1), Builder2));
            Value* dif1 = Builder2.CreateSelect(cmp, ConstantFP::get(II.getOperand(0)->getType(), 0), vdiff);
            addToDiffe(II.getOperand(1), dif1, Builder2, II.getType());
          }
          return;
        }

        case Intrinsic::log: {
          if (!gutils->isConstantValue(&II) && !gutils->isConstantValue(II.getOperand(0))) {
            Value* dif0 = Builder2.CreateFDiv(vdiff, lookup(II.getOperand(0), Builder2));
            addToDiffe(II.getOperand(0), dif0, Builder2, II.getType());
          }
          return;
        }

        case Intrinsic::log2: {
          if (!gutils->isConstantValue(&II) && !gutils->isConstantValue(II.getOperand(0))) {
            Value* dif0 = Builder2.CreateFDiv(vdiff,
              Builder2.CreateFMul(ConstantFP::get(II.getType(), 0.6931471805599453), lookup(II.getOperand(0), Builder2))
            );
            addToDiffe(II.getOperand(0), dif0, Builder2, II.getType());
          }
          return;
        }
        case Intrinsic::log10: {
          if (!gutils->isConstantValue(&II) && !gutils->isConstantValue(II.getOperand(0))) {
            Value* dif0 = Builder2.CreateFDiv(vdiff,
              Builder2.CreateFMul(ConstantFP::get(II.getType(), 2.302585092994046), lookup(II.getOperand(0), Builder2))
            );
            addToDiffe(II.getOperand(0), dif0, Builder2, II.getType());
          }
          return;
        }

        case Intrinsic::exp: {
          if (!gutils->isConstantValue(&II) && !gutils->isConstantValue(II.getOperand(0))) {
            Value* dif0 = Builder2.CreateFMul(vdiff, lookup(&II, Builder2));
            addToDiffe(II.getOperand(0), dif0, Builder2, II.getType());
          }
          return;
        }
        case Intrinsic::exp2: {
          if (!gutils->isConstantValue(&II) && !gutils->isConstantValue(II.getOperand(0))) {
            Value* dif0 = Builder2.CreateFMul(
              Builder2.CreateFMul(vdiff, lookup(&II, Builder2)), ConstantFP::get(II.getType(), 0.6931471805599453)
            );
            addToDiffe(II.getOperand(0), dif0, Builder2, II.getType());
          }
          return;
        }
        case Intrinsic::pow: {
          if (!gutils->isConstantValue(&II) && !gutils->isConstantValue(II.getOperand(0))) {

            /*
            dif0 = Builder2.CreateFMul(
              Builder2.CreateFMul(vdiff,
                Builder2.CreateFDiv(lookup(&II), lookup(II.getOperand(0)))), lookup(II.getOperand(1))
            );
            */
            SmallVector<Value*, 2> args = {lookup(II.getOperand(0), Builder2), Builder2.CreateFSub(lookup(II.getOperand(1), Builder2), ConstantFP::get(II.getType(), 1.0))};
            Type *tys[] = {args[1]->getType()};
            auto cal = Builder2.CreateCall(Intrinsic::getDeclaration(M, Intrinsic::pow, tys), args);
            cal->setAttributes(II.getAttributes());
            cal->setCallingConv(II.getCallingConv());
            cal->setTailCallKind(II.getTailCallKind());
            Value* dif0 = Builder2.CreateFMul(
              Builder2.CreateFMul(vdiff, cal)
              , lookup(II.getOperand(1), Builder2)
            );
            addToDiffe(II.getOperand(0), dif0, Builder2, II.getType());
          }

          if (!gutils->isConstantValue(&II) && !gutils->isConstantValue(II.getOperand(1))) {
            Value *args[] = {lookup(II.getOperand(1), Builder2)};
            Type *tys[] = {II.getOperand(1)->getType()};

            Value* dif1 = Builder2.CreateFMul(
              Builder2.CreateFMul(vdiff, lookup(&II, Builder2)),
              Builder2.CreateCall(Intrinsic::getDeclaration(M, Intrinsic::log, tys), args)
            );
            addToDiffe(II.getOperand(1), dif1, Builder2, II.getType());
          }

          return;
        }
        case Intrinsic::sin: {
          if (!gutils->isConstantValue(&II) && !gutils->isConstantValue(II.getOperand(0))) {
            Value *args[] = {lookup(II.getOperand(0), Builder2)};
            Type *tys[] = {II.getOperand(0)->getType()};
            Value* dif0 = Builder2.CreateFMul(vdiff,
              Builder2.CreateCall(Intrinsic::getDeclaration(M, Intrinsic::cos, tys), args) );
            addToDiffe(II.getOperand(0), dif0, Builder2, II.getType());
          }
          return;
        }
        case Intrinsic::cos: {
          if (!gutils->isConstantValue(&II) && !gutils->isConstantValue(II.getOperand(0))) {
            Value *args[] = {lookup(II.getOperand(0), Builder2)};
            Type *tys[] = {II.getOperand(0)->getType()};
            Value* dif0 = Builder2.CreateFMul(vdiff,
              Builder2.CreateFNeg(
                Builder2.CreateCall(Intrinsic::getDeclaration(M, Intrinsic::sin, tys), args) )
            );
            addToDiffe(II.getOperand(0), dif0, Builder2, II.getType());
          }
          return;
        }

        default:
          if (gutils->isConstantInstruction(&II)) return;
          llvm::errs() << *gutils->oldFunc << "\n";
          llvm::errs() << *gutils->newFunc << "\n";
          llvm::errs() << "cannot handle (augmented) unknown intrinsic\n" << II;
          report_fatal_error("(augmented) unknown intrinsic");
      }
    }

    llvm::InstVisitor<DerivativeMaker<AugmentedReturnType>>::visitIntrinsicInst(II);
  }
};

template <>
void DerivativeMaker<AugmentedReturn*>::visitCallInst(llvm::CallInst &call) {
  assert(mode == DerivativeMode::Forward);
  auto orig = gutils->getOriginal(&call);

  if(uncacheable_args_map.find(orig) == uncacheable_args_map.end()) {
    llvm::errs() << " call: " << call << "\n";
    llvm::errs() << " orig: " << *orig << "\n";
    for(auto & pair: uncacheable_args_map) {
      llvm::errs() << " + " << *pair.first << "\n";
    }
  }

  assert(uncacheable_args_map.find(orig) != uncacheable_args_map.end());
  assert(augmentedReturn);
  handleAugmentedCallInst(TR, &call, gutils, uncacheable_args_map.find(orig)->second, *returnuses, getIndex, augmentedReturn->subaugmentations);
}

template <>
void DerivativeMaker<const AugmentedReturn*>::visitCallInst(llvm::CallInst &call) {
  assert(mode == DerivativeMode::Both || mode == DerivativeMode::Reverse);
  llvm::errs() << "calling meta call\n";
  llvm::InstVisitor<DerivativeMaker<const AugmentedReturn*>>::visitCallInst(call);
}

// TODO note this doesn't go through [loop, unreachable], and we could get more performance by doing this
// can consider doing some domtree magic potentially
static inline SmallPtrSet<BasicBlock*, 4> getGuaranteedUnreachable(Function* F) {
  SmallPtrSet<BasicBlock*, 4> knownUnreachables;
  std::deque<BasicBlock*> todo;
  for(auto &BB : *F) { todo.push_back(&BB); }

  while(!todo.empty()) {
    BasicBlock* next = todo.front();
    todo.pop_front();

    if (knownUnreachables.find(next) != knownUnreachables.end()) continue;

    if (isa<ReturnInst>(next->getTerminator())) continue;

    if (isa<UnreachableInst>(next->getTerminator())) {
      knownUnreachables.insert(next);
      for (BasicBlock *Pred : predecessors(next)) {
        todo.push_back(Pred);
      }
      continue;
    }

    bool unreachable = true;
    for (BasicBlock *Succ : successors(next)) {
      if (knownUnreachables.find(Succ) == knownUnreachables.end()) {
        unreachable = false;
        break;
      }
    }

    if (!unreachable) continue;
    knownUnreachables.insert(next);
    for (BasicBlock *Pred : predecessors(next)) {
      todo.push_back(Pred);
    }
    continue;
  }

  return knownUnreachables;
}

template<typename K, typename V, typename K2>
typename std::map<K, V>::iterator insert_or_assign(std::map<K, V>& map, K2 key, V&& val) {
  // map.insert_or_assign(key, val);
  auto found = map.find(key);
  if (found == map.end()) {}
    return map.emplace(key, val).first;

  map.at(key) = val;
  //map->second = val;
  return map.find(key);
}

template<typename K, typename V, typename K2>
typename std::map<K, V>::iterator insert_or_assign(std::map<K, V>& map, K2 key, const V& val) {
  // map.insert_or_assign(key, val);
  auto found = map.find(key);
  if (found == map.end()) {}
    return map.emplace(key, val).first;

  map.at(key) = val;
  //map->second = val;
  return map.find(key);
}


//! return structtype if recursive function
const AugmentedReturn& CreateAugmentedPrimal(Function* todiff, const std::set<unsigned>& constant_args, TargetLibraryInfo &TLI, TypeAnalysis &TA, AAResults &global_AA,
                                             bool differentialReturn, bool returnUsed, const NewFnTypeInfo& oldTypeInfo,
                                             const std::map<Argument*, bool> _uncacheable_args, bool forceAnonymousTape) {
  if (returnUsed) assert(!todiff->getReturnType()->isEmptyTy() && !todiff->getReturnType()->isVoidTy());
  if (differentialReturn) assert(!todiff->getReturnType()->isEmptyTy() && !todiff->getReturnType()->isVoidTy());

  static std::map<std::tuple<Function*,std::set<unsigned>/*constant_args*/, std::map<Argument*, bool>/*uncacheable_args*/, bool/*differentialReturn*/, bool/*returnUsed*/, const NewFnTypeInfo>, AugmentedReturn> cachedfunctions;
  static std::map<std::tuple<Function*,std::set<unsigned>/*constant_args*/, std::map<Argument*, bool>/*uncacheable_args*/, bool/*differentialReturn*/, bool/*returnUsed*/, const NewFnTypeInfo>, bool> cachedfinished;
  std::tuple<Function*,std::set<unsigned>/*constant_args*/, std::map<Argument*, bool>/*uncacheable_args*/, bool/*differentialReturn*/, bool/*returnUsed*/, const NewFnTypeInfo> tup = std::make_tuple(todiff, std::set<unsigned>(constant_args.begin(), constant_args.end()), std::map<Argument*, bool>(_uncacheable_args.begin(), _uncacheable_args.end()), differentialReturn, returnUsed, oldTypeInfo);
  auto found = cachedfunctions.find(tup);
  //llvm::errs() << "augmenting function " << todiff->getName() << " constant args " << to_string(constant_args) << " uncacheable_args: " << to_string(_uncacheable_args) << " differet" << differentialReturn << " returnUsed: " << returnUsed << " found==" << (found != cachedfunctions.end()) << "\n";
  if (found != cachedfunctions.end()) {
    return found->second;
  }

    if (constant_args.size() == 0 && hasMetadata(todiff, "enzyme_augment")) {
      auto md = todiff->getMetadata("enzyme_augment");
      if (!isa<MDTuple>(md)) {
          llvm::errs() << *todiff << "\n";
          llvm::errs() << *md << "\n";
          report_fatal_error("unknown augment for noninvertible function -- metadata incorrect");
      }
      std::map<AugmentedStruct, unsigned> returnMapping;
      returnMapping[AugmentedStruct::Tape] = 0;
      returnMapping[AugmentedStruct::Return] = 1;
      returnMapping[AugmentedStruct::DifferentialReturn] = 2;

      auto md2 = cast<MDTuple>(md);
      assert(md2->getNumOperands() == 1);
      auto gvemd = cast<ConstantAsMetadata>(md2->getOperand(0));
      auto foundcalled = cast<Function>(gvemd->getValue());

      if (foundcalled->getReturnType() == todiff->getReturnType()) {
        FunctionType *FTy = FunctionType::get(StructType::get(todiff->getContext(), {StructType::get(todiff->getContext(), {}), foundcalled->getReturnType()}),
                                   foundcalled->getFunctionType()->params(), foundcalled->getFunctionType()->isVarArg());
        Function *NewF = Function::Create(FTy, Function::LinkageTypes::InternalLinkage, "fixaugmented_"+todiff->getName(), todiff->getParent());
        NewF->setAttributes(foundcalled->getAttributes());
        if (NewF->hasFnAttribute(Attribute::NoInline)) {
            NewF->removeFnAttr(Attribute::NoInline);
        }
        for (auto i=foundcalled->arg_begin(), j=NewF->arg_begin(); i != foundcalled->arg_end(); ) {
             j->setName(i->getName());
             if (j->hasAttribute(Attribute::Returned))
                 j->removeAttr(Attribute::Returned);
             if (j->hasAttribute(Attribute::StructRet))
                 j->removeAttr(Attribute::StructRet);
             i++;
             j++;
         }
        BasicBlock *BB = BasicBlock::Create(NewF->getContext(), "entry", NewF);
        IRBuilder <>bb(BB);
        SmallVector<Value*,4> args;
        for(auto &a : NewF->args()) args.push_back(&a);
        auto cal = bb.CreateCall(foundcalled, args);
        cal->setCallingConv(foundcalled->getCallingConv());
        auto ut = UndefValue::get(NewF->getReturnType());
        auto val = bb.CreateInsertValue(ut, cal, {1u});
        bb.CreateRet(val);
        return insert_or_assign(cachedfunctions, tup, AugmentedReturn(NewF, nullptr, {}, returnMapping, {}, {}))->second;
      }

      //assert(st->getNumElements() > 0);
      return insert_or_assign(cachedfunctions, tup, AugmentedReturn(foundcalled, nullptr, {}, returnMapping, {}, {}))->second; //dyn_cast<StructType>(st->getElementType(0)));
    }

  if (todiff->empty()) {
    llvm::errs() << "mod: " << *todiff->getParent() << "\n";
    llvm::errs() << *todiff << "\n";
  }
  assert(!todiff->empty());
  std::map<AugmentedStruct, unsigned> returnMapping;
  AAResults AA(TLI);
  //AA.addAAResult(global_AA);

  GradientUtils *gutils = GradientUtils::CreateFromClone(todiff, TLI, TA, AA, constant_args, /*returnUsed*/returnUsed, /*differentialReturn*/differentialReturn, returnMapping);
  const SmallPtrSet<BasicBlock*, 4> guaranteedUnreachable = getGuaranteedUnreachable(gutils->oldFunc);

  gutils->forceContexts();

  NewFnTypeInfo typeInfo(gutils->oldFunc);
  {
    auto toarg = todiff->arg_begin();
    auto olarg = gutils->oldFunc->arg_begin();
    for(; toarg != todiff->arg_end(); toarg++, olarg++) {

        {
        auto fd = oldTypeInfo.first.find(toarg);
        assert(fd != oldTypeInfo.first.end());
        typeInfo.first.insert(std::pair<Argument*, ValueData>(olarg, fd->second));
        }

        {
        auto cfd = oldTypeInfo.knownValues.find(toarg);
        assert(cfd != oldTypeInfo.knownValues.end());
        typeInfo.knownValues.insert(std::pair<Argument*, std::set<int64_t>>(olarg, cfd->second));
        }
    }
    typeInfo.second = oldTypeInfo.second;
  }
  TypeResults TR = TA.analyzeFunction(typeInfo);
  assert(TR.info.function == gutils->oldFunc);
  gutils->forceActiveDetection(AA, TR);


  gutils->forceAugmentedReturns(TR, guaranteedUnreachable);

  // Convert uncacheable args from the input function to the preprocessed function
  std::map<Argument*, bool> _uncacheable_argsPP;
  {
    auto in_arg = todiff->arg_begin();
    auto pp_arg = gutils->oldFunc->arg_begin();
    for(; pp_arg != gutils->oldFunc->arg_end(); ) {
        _uncacheable_argsPP[pp_arg] = _uncacheable_args.find(in_arg)->second;
        pp_arg++;
        in_arg++;
    }
  }

  const std::map<CallInst*, const std::map<Argument*, bool> > uncacheable_args_map =
      compute_uncacheable_args_for_callsites(gutils->oldFunc, gutils->DT, TLI, AA, gutils, _uncacheable_argsPP);

  const std::map<Instruction*, bool> can_modref_map = compute_uncacheable_load_map(gutils, AA, TLI, _uncacheable_argsPP);

  //for (auto &iter : can_modref_map) {
  //  llvm::errs() << "isneeded: " << iter.second << " augmented can_modref_map: " << *iter.first << " fn: " << iter.first->getParent()->getParent()->getName() << "\n";
  //}


  insert_or_assign(cachedfunctions, tup, AugmentedReturn(gutils->newFunc, nullptr, {}, returnMapping, uncacheable_args_map, can_modref_map));
  cachedfinished[tup] = false;

  auto getIndex = [&](Instruction* I, CacheType u)-> unsigned {
    //std::map<std::pair<Instruction*,std::string>,unsigned>& mapping = cachedfunctions[tup].tapeIndices;
    return gutils->getIndex( std::make_pair(I, u), cachedfunctions.find(tup)->second.tapeIndices);
  };
  gutils->can_modref_map = &can_modref_map;

  //! Explicitly handle all returns first to ensure that all instructions know whether or not they are used
  SmallPtrSet<Instruction*, 4> returnuses;

  //! Similarly keep track of inverted pointers we may need to return
  ValueToValueMapTy invertedRetPs;

  for(BasicBlock* BB: gutils->originalBlocks) {
    if(auto ri = dyn_cast<ReturnInst>(BB->getTerminator())) {
        auto oldval = ri->getReturnValue();
        IRBuilder <>ib(ri);
        Value* rt = UndefValue::get(gutils->newFunc->getReturnType());
        if (oldval && returnUsed) {
            assert(returnMapping.find(AugmentedStruct::Return) != returnMapping.end());
            //llvm::errs() << " rt: " << *rt << " oldval:" << *oldval << "\n";
            //llvm::errs() << "    returnIndex: " << returnMapping.find(AugmentedStruct::Return)->second << "\n";
            rt = ib.CreateInsertValue(rt, oldval, {returnMapping.find(AugmentedStruct::Return)->second});
            if (Instruction* inst = dyn_cast<Instruction>(rt)) {
                returnuses.insert(inst);
            }
        }

        auto newri = ib.CreateRet(rt);
        ib.SetInsertPoint(newri);

        //Only get the inverted pointer if necessary
        if (differentialReturn && oldval && !oldval->getType()->isFPOrFPVectorTy()) {
            // We need to still get even if not a constant value so the other end can handle all returns with a reasonable value
            //   That said, if we return a constant value (i.e. nullptr) we shouldn't try inverting the pointer and return undef instead (since it [hopefully] shouldn't be used)
            if (!gutils->isConstantValue(oldval)) {
                invertedRetPs[newri] = gutils->invertPointerM(oldval, ib);
            } else {
                invertedRetPs[newri] = UndefValue::get(oldval->getType());
            }
        }

        gutils->erase(ri);
        /*
         TODO should call DCE now ideally
        if (auto inst = dyn_cast<Instruction>(rt)) {
            SmallSetVector<Instruction *, 16> WorkList;
            DCEInstruction(inst, WorkList, TLI);
            while (!WorkList.empty()) {
                Instruction *I = WorkList.pop_back_val();
                MadeChange |= DCEInstruction(I, WorkList, TLI);
            }
        }
        */
    }
  }

  for(BasicBlock* BB: gutils->originalBlocks) {
      auto term = BB->getTerminator();
      assert(term);
      BasicBlock* oBB = gutils->getOriginal(BB);

      // Don't create derivatives for code that results in termination
      if (guaranteedUnreachable.find(oBB) != guaranteedUnreachable.end()) continue;

      if (isa<ReturnInst>(term) || isa<BranchInst>(term) || isa<SwitchInst>(term)) {
      } else {
        assert(BB);
        assert(BB->getParent());
        assert(term);
        llvm::errs() << *BB->getParent() << "\n";
        llvm::errs() << "unknown terminator instance " << *term << "\n";
        assert(0 && "unknown terminator inst");
      }

      BasicBlock::reverse_iterator I = oBB->rbegin(), E = oBB->rend();
      I++;
      for (; I != E; I++) {
        Instruction* inst = gutils->getNewFromOriginal(&*I);
        assert(inst);

        DerivativeMaker<AugmentedReturn*> maker(DerivativeMode::Forward, gutils, TR, getIndex, uncacheable_args_map, &returnuses, &cachedfunctions.find(tup)->second, nullptr);
        maker.visit(*inst);
     }
  }

  auto nf = gutils->newFunc;

  while(gutils->inversionAllocs->size() > 0) {
    gutils->inversionAllocs->back().moveBefore(gutils->newFunc->getEntryBlock().getFirstNonPHIOrDbgOrLifetime());
  }

  (IRBuilder <>(gutils->inversionAllocs)).CreateUnreachable();
  DeleteDeadBlock(gutils->inversionAllocs);

  for (Argument &Arg : gutils->newFunc->args()) {
      if (Arg.hasAttribute(Attribute::Returned))
          Arg.removeAttr(Attribute::Returned);
      if (Arg.hasAttribute(Attribute::StructRet))
          Arg.removeAttr(Attribute::StructRet);
  }

  if (gutils->newFunc->hasFnAttribute(Attribute::OptimizeNone))
    gutils->newFunc->removeFnAttr(Attribute::OptimizeNone);

  if (auto bytes = gutils->newFunc->getDereferenceableBytes(llvm::AttributeList::ReturnIndex)) {
    AttrBuilder ab;
    ab.addDereferenceableAttr(bytes);
    gutils->newFunc->removeAttributes(llvm::AttributeList::ReturnIndex, ab);
  }

  if (gutils->newFunc->hasAttribute(llvm::AttributeList::ReturnIndex, llvm::Attribute::NoAlias)) {
    gutils->newFunc->removeAttribute(llvm::AttributeList::ReturnIndex, llvm::Attribute::NoAlias);
  }
  if (gutils->newFunc->hasAttribute(llvm::AttributeList::ReturnIndex, llvm::Attribute::ZExt)) {
    gutils->newFunc->removeAttribute(llvm::AttributeList::ReturnIndex, llvm::Attribute::ZExt);
  }

  gutils->cleanupActiveDetection();

  if (llvm::verifyFunction(*gutils->newFunc, &llvm::errs())) {
      llvm::errs() << *gutils->oldFunc << "\n";
      llvm::errs() << *gutils->newFunc << "\n";
      report_fatal_error("function failed verification (2)");
  }

  std::vector<Type*> RetTypes(cast<StructType>(gutils->newFunc->getReturnType())->elements());

  std::vector<Type*> MallocTypes;

  for(auto a:gutils->getMallocs()) {
      MallocTypes.push_back(a->getType());
  }

  StructType* tapeType = StructType::get(nf->getContext(), MallocTypes);

  bool recursive = cachedfunctions.find(tup)->second.fn->getNumUses() > 0 || forceAnonymousTape;

  if (recursive) {
    assert(RetTypes[returnMapping.find(AugmentedStruct::Tape)->second] == Type::getInt8PtrTy(nf->getContext()));
  } else {
    RetTypes[returnMapping.find(AugmentedStruct::Tape)->second] = tapeType;
  }

  Type* RetType = StructType::get(nf->getContext(), RetTypes);

 ValueToValueMapTy VMap;
 std::vector<Type*> ArgTypes;
 for (const Argument &I : nf->args()) {
     ArgTypes.push_back(I.getType());
 }

 // Create a new function type...
 FunctionType *FTy = FunctionType::get(RetType,
                                   ArgTypes, nf->getFunctionType()->isVarArg());

 // Create the new function...
 Function *NewF = Function::Create(FTy, nf->getLinkage(), "augmented_"+todiff->getName(), nf->getParent());

 unsigned ii = 0, jj = 0;
 for (auto i=nf->arg_begin(), j=NewF->arg_begin(); i != nf->arg_end(); ) {
    VMap[i] = j;
    if (nf->hasParamAttribute(ii, Attribute::NoCapture)) {
      NewF->addParamAttr(jj, Attribute::NoCapture);
    }
    if (nf->hasParamAttribute(ii, Attribute::NoAlias)) {
       NewF->addParamAttr(jj, Attribute::NoAlias);
    }

     j->setName(i->getName());
     j++;
     jj++;

     i++;
     ii++;
 }

  SmallVector <ReturnInst*,4> Returns;
  CloneFunctionInto(NewF, nf, VMap, nf->getSubprogram() != nullptr, Returns, "",
                   nullptr);

  IRBuilder<> ib(NewF->getEntryBlock().getFirstNonPHI());

  Value* ret = ib.CreateAlloca(RetType);

  Value* tapeMemory;
  if (recursive) {
      auto i64 = Type::getInt64Ty(NewF->getContext());
      tapeMemory = CallInst::CreateMalloc(NewF->getEntryBlock().getFirstNonPHI(),
                    i64,
                    tapeType,
                    ConstantInt::get(i64, NewF->getParent()->getDataLayout().getTypeAllocSizeInBits(tapeType)/8),
                    nullptr,
                    nullptr,
                    "tapemem"
                    );
    CallInst* malloccall = dyn_cast<CallInst>(tapeMemory);
    if (malloccall == nullptr) {
        malloccall = cast<CallInst>(cast<Instruction>(tapeMemory)->getOperand(0));
    }
    malloccall->addAttribute(AttributeList::ReturnIndex, Attribute::NoAlias);
    malloccall->addAttribute(AttributeList::ReturnIndex, Attribute::NonNull);
    Value *Idxs[] = {
        ib.getInt32(0),
        ib.getInt32(returnMapping.find(AugmentedStruct::Tape)->second),
    };
    assert(malloccall);
    assert(ret);
    ib.CreateStore(malloccall, ib.CreateGEP(ret, Idxs, ""));
  } else {
    Value *Idxs[] = {
        ib.getInt32(0),
        ib.getInt32(returnMapping.find(AugmentedStruct::Tape)->second),
    };
    tapeMemory = ib.CreateGEP(ret, Idxs, "");
  }

  unsigned i=0;
  for (auto v: gutils->getMallocs()) {
      if (!isa<UndefValue>(v)) {
          //llvm::errs() << "v: " << *v << "VMap[v]: " << *VMap[v] << "\n";
          IRBuilder <>ib(cast<Instruction>(VMap[v])->getNextNode());
          Value *Idxs[] = {
            ib.getInt32(0),
            ib.getInt32(i)
          };
          auto gep = ib.CreateGEP(tapeMemory, Idxs, "");
          ib.CreateStore(VMap[v], gep);
      }
      i++;
  }
  if (tapeMemory->hasNUses(0)) gutils->erase(cast<Instruction>(tapeMemory));

  for (inst_iterator I = inst_begin(nf), E = inst_end(nf); I != E; ++I) {
      if (ReturnInst* ri = dyn_cast<ReturnInst>(&*I)) {
          ReturnInst* rim = cast<ReturnInst>(VMap[ri]);
          Type* oldretTy = gutils->oldFunc->getReturnType();
          IRBuilder <>ib(rim);
          if (returnUsed) {
            Value* rv = rim->getReturnValue();
            assert(rv);
            Value* actualrv = nullptr;
            if (auto iv = dyn_cast<InsertValueInst>(rv)) {
              if (iv->getNumIndices() == 1 && iv->getIndices()[0] == returnMapping.find(AugmentedStruct::Return)->second) {
                actualrv = iv->getInsertedValueOperand();
              }
            }
            if (actualrv == nullptr) {
              actualrv = ib.CreateExtractValue(rv, {returnMapping.find(AugmentedStruct::Return)->second});
            }

            ib.CreateStore(actualrv, ib.CreateConstGEP2_32(RetType, ret, 0, returnMapping.find(AugmentedStruct::Return)->second, ""));
          }

          if (differentialReturn && !oldretTy->isFPOrFPVectorTy()) {
              assert(invertedRetPs[ri]);
              if (!isa<UndefValue>(invertedRetPs[ri])) {
                assert(VMap[invertedRetPs[ri]]);
                ib.CreateStore( VMap[invertedRetPs[ri]], ib.CreateConstGEP2_32(RetType, ret, 0, returnMapping.find(AugmentedStruct::DifferentialReturn)->second, ""));
              }
          }
          ib.CreateRet(ib.CreateLoad(ret));
          gutils->erase(cast<Instruction>(VMap[ri]));
      }
  }

  for (Argument &Arg : NewF->args()) {
      if (Arg.hasAttribute(Attribute::Returned))
          Arg.removeAttr(Attribute::Returned);
      if (Arg.hasAttribute(Attribute::StructRet))
          Arg.removeAttr(Attribute::StructRet);
  }
  if (NewF->hasFnAttribute(Attribute::OptimizeNone))
    NewF->removeFnAttr(Attribute::OptimizeNone);

  if (auto bytes = NewF->getDereferenceableBytes(llvm::AttributeList::ReturnIndex)) {
    AttrBuilder ab;
    ab.addDereferenceableAttr(bytes);
    NewF->removeAttributes(llvm::AttributeList::ReturnIndex, ab);
  }
  if (NewF->hasAttribute(llvm::AttributeList::ReturnIndex, llvm::Attribute::NoAlias)) {
    NewF->removeAttribute(llvm::AttributeList::ReturnIndex, llvm::Attribute::NoAlias);
  }
  if (NewF->hasAttribute(llvm::AttributeList::ReturnIndex, llvm::Attribute::ZExt)) {
    NewF->removeAttribute(llvm::AttributeList::ReturnIndex, llvm::Attribute::ZExt);
  }

  if (llvm::verifyFunction(*NewF, &llvm::errs())) {
      llvm::errs() << *gutils->oldFunc << "\n";
      llvm::errs() << *NewF << "\n";
      report_fatal_error("augmented function failed verification (3)");
  }

  SmallVector<User*,4> fnusers;
  for(auto user : cachedfunctions.find(tup)->second.fn->users()) {
    fnusers.push_back(user);
  }
  for(auto user : fnusers) {
    cast<CallInst>(user)->setCalledFunction(NewF);
  }
  cachedfunctions.find(tup)->second.fn = NewF;
  if (recursive)
      cachedfunctions.find(tup)->second.tapeType = tapeType;
  insert_or_assign(cachedfinished, tup, true);

  //llvm::errs() << "augmented fn seeing sub_index_map of " << std::get<2>(cachedfunctions[tup]).size() << " in ap " << NewF->getName() << "\n";
  gutils->newFunc->eraseFromParent();

  delete gutils;
  if (enzyme_print)
    llvm::errs() << *NewF << "\n";
  return cachedfunctions.find(tup)->second;
}

void createInvertedTerminator(TypeResults& TR, DiffeGradientUtils* gutils, BasicBlock *BB, AllocaInst* retAlloca, AllocaInst* dretAlloca, unsigned extraArgs) {
    LoopContext loopContext;
    bool inLoop = gutils->getContext(BB, loopContext);
    BasicBlock* BB2 = gutils->reverseBlocks[BB];
    assert(BB2);
    IRBuilder<> Builder(BB2);
    Builder.setFastMathFlags(getFast());

    std::map<BasicBlock*, std::vector<BasicBlock*>> targetToPreds;
    for(auto pred : predecessors(BB)) {
        targetToPreds[gutils->getReverseOrLatchMerge(pred, BB)].emplace_back(pred);
    }

    if (targetToPreds.size() == 0) {
        SmallVector<Value *,4> retargs;

        if (retAlloca) {
          auto result = Builder.CreateLoad(retAlloca, "retreload");
          //TODO reintroduce invariant load/group
          //result->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(retAlloca->getContext(), {}));
          assert(gutils->isConstantInstruction(result));
          retargs.push_back(result);
        }

        if (dretAlloca) {
          auto result = Builder.CreateLoad(dretAlloca, "dretreload");
          //TODO reintroduce invariant load/group
          //result->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(dretAlloca->getContext(), {}));
          assert(gutils->isConstantInstruction(result));
          retargs.push_back(result);
        }

        for (auto& I: gutils->oldFunc->args()) {
          if (!gutils->isConstantValue(gutils->getNewFromOriginal(&I)) && whatType(I.getType()) == DIFFE_TYPE::OUT_DIFF ) {
            retargs.push_back(gutils->diffe((Value*)gutils->getNewFromOriginal(&I), Builder));
          }
        }

        Value* toret = UndefValue::get(gutils->newFunc->getReturnType());
        for(unsigned i=0; i<retargs.size(); i++) {
          unsigned idx[] = { i };
          toret = Builder.CreateInsertValue(toret, retargs[i], idx);
        }
        Builder.CreateRet(toret);
        return;
    }

    //PHINodes to replace that will contain true iff the predecessor was given basicblock
    std::map<BasicBlock*, PHINode*> replacePHIs;
    std::vector<SelectInst*> selects;

    IRBuilder <>phibuilder(BB2);
    bool setphi = false;

    // Ensure phi values have their derivatives propagated
    for (auto I = BB->begin(), E = BB->end(); I != E; I++) {
        if(PHINode* PN = dyn_cast<PHINode>(&*I)) {
            if (gutils->isConstantValue(PN)) continue;

            auto PNtype = TR.intType(gutils->getOriginal(PN), /*necessary*/false);

            //TODO remove explicit type check and only use PNtype
            if (PNtype == IntType::Pointer || PN->getType()->isPointerTy()) continue;

            //TODO consider skipping if not a secret float
            //if (!isIntASecretFloat(PN))) continue;

            auto prediff = gutils->diffe(PN, Builder);
            gutils->setDiffe(PN, Constant::getNullValue(PN->getType()), Builder);

            //TODO make the necessary flag just true.
            //
            Type* PNfloatType = PNtype.isFloat();
            if (!PNfloatType)
              llvm::errs() << " for PN " << *PN << " saw " << TR.intType(gutils->getOriginal(PN), /*necessary*/false).str() << "\n";
            TR.intType(gutils->getOriginal(PN), /*necessary*/true);
            assert(PNfloatType);

            for (BasicBlock* pred : predecessors(BB)) {
                if (gutils->isConstantValue(PN->getIncomingValueForBlock(pred))) {
                    continue;
                }

                if (PN->getNumIncomingValues() == 1) {
                    gutils->addToDiffe(PN->getIncomingValueForBlock(pred), prediff, Builder, PNfloatType);
                } else {
                    if (replacePHIs.find(pred) == replacePHIs.end()) {
                        replacePHIs[pred] = Builder.CreatePHI(Type::getInt1Ty(pred->getContext()), 1);
                        if (!setphi) {
                            phibuilder.SetInsertPoint(replacePHIs[pred]);
                            setphi = true;
                        }
                    }
                    SelectInst* dif = cast<SelectInst>(Builder.CreateSelect(replacePHIs[pred], prediff, Constant::getNullValue(prediff->getType())));
                    //llvm::errs() << "creating prediff " << *dif << " for value incoming " << PN->getIncomingValueForBlock(pred) << " for " << *PN << "\n";
                    auto addedSelects = gutils->addToDiffe(PN->getIncomingValueForBlock(pred), dif, Builder, PNfloatType);
                    /*if (dif->getNumUses() != 0) {
                      llvm::errs() << "oldFunc: " << *gutils->oldFunc << "\n";
                      llvm::errs() << "newFunc: " << *gutils->newFunc << "\n";
                      for (auto use : dif->users()) {
                        llvm::errs() << "user: " << *use << "\n";
                      }
                      llvm::errs() << "dif: " << *dif << "\n";
                    }
                    assert(dif->getNumUses() == 0);
                    dif->eraseFromParent();
                    */
                    for (auto select : addedSelects)
                        selects.emplace_back(select);
                }
            }
        } else break;
    }
    if (!setphi) {
        phibuilder.SetInsertPoint(Builder.GetInsertBlock(), Builder.GetInsertPoint());
    }

    if (inLoop && BB == loopContext.header) {
        std::map<BasicBlock*, std::vector<BasicBlock*>> targetToPreds;
        for(auto pred : predecessors(BB)) {
            if (pred == loopContext.preheader) continue;
            targetToPreds[gutils->getReverseOrLatchMerge(pred, BB)].emplace_back(pred);
        }

        assert(targetToPreds.size() && "only loops with one backedge are presently supported");

        Value* av = phibuilder.CreateLoad(loopContext.antivaralloc);
        Value* phi = phibuilder.CreateICmpEQ(av, Constant::getNullValue(av->getType()));
        Value* nphi = phibuilder.CreateNot(phi);

        for (auto pair : replacePHIs) {
            Value* replaceWith = nullptr;

            if (pair.first == loopContext.preheader) {
                replaceWith = phi;
            } else {
                replaceWith = nphi;
            }

            pair.second->replaceAllUsesWith(replaceWith);
            pair.second->eraseFromParent();
        }

        Builder.SetInsertPoint(BB2);

        Builder.CreateCondBr(phi, gutils->getReverseOrLatchMerge(loopContext.preheader, BB), targetToPreds.begin()->first);

    } else {
        std::map<BasicBlock*, std::vector< std::pair<BasicBlock*, BasicBlock*> >> phiTargetToPreds;
        for (auto pair : replacePHIs) {
            phiTargetToPreds[pair.first].emplace_back(std::make_pair(pair.first, BB));
        }
        BasicBlock* fakeTarget = nullptr;
        for (auto pred : predecessors(BB)) {
            if (phiTargetToPreds.find(pred) != phiTargetToPreds.end()) continue;
            if (fakeTarget == nullptr) fakeTarget = pred;
            phiTargetToPreds[fakeTarget].emplace_back(std::make_pair(pred, BB));
        }
        gutils->branchToCorrespondingTarget(BB, phibuilder, phiTargetToPreds, &replacePHIs);

        std::map<BasicBlock*, std::vector< std::pair<BasicBlock*, BasicBlock*> >> targetToPreds;
        for(auto pred : predecessors(BB)) {
            targetToPreds[gutils->getReverseOrLatchMerge(pred, BB)].emplace_back(std::make_pair(pred, BB));
        }
        Builder.SetInsertPoint(BB2);
        gutils->branchToCorrespondingTarget(BB, Builder, targetToPreds);
    }

    // Optimize select of not to just be a select with operands switched
    for (SelectInst* select : selects) {
        if (BinaryOperator* bo = dyn_cast<BinaryOperator>(select->getCondition())) {
            if (bo->getOpcode() == BinaryOperator::Xor) {
                //llvm::errs() << " considering " << *select << " " << *bo << "\n";
                if (isa<ConstantInt>(bo->getOperand(0)) && cast<ConstantInt>(bo->getOperand(0))->isOne()) {
                    select->setCondition(bo->getOperand(1));
                    auto tmp = select->getTrueValue();
                    select->setTrueValue(select->getFalseValue());
                    select->setFalseValue(tmp);
                    if (bo->getNumUses() == 0) bo->eraseFromParent();
                } else if (isa<ConstantInt>(bo->getOperand(1)) && cast<ConstantInt>(bo->getOperand(1))->isOne()) {
                    select->setCondition(bo->getOperand(0));
                    auto tmp = select->getTrueValue();
                    select->setTrueValue(select->getFalseValue());
                    select->setFalseValue(tmp);
                    if (bo->getNumUses() == 0) bo->eraseFromParent();
                }
            }
        }
    }
}

static bool shouldAugmentCall(CallInst* op, const GradientUtils* gutils) {
  Function *called = op->getCalledFunction();

  bool modifyPrimal = !called || !called->hasFnAttribute(Attribute::ReadNone);

  if (modifyPrimal) {
     //llvm::errs() << "primal modified " << called->getName() << " modified via reading from memory" << "\n";
  }

  if ( !op->getType()->isFPOrFPVectorTy() && !gutils->isConstantValue(op) ) {
     modifyPrimal = true;
     //llvm::errs() << "primal modified " << called->getName() << " modified via return" << "\n";
  }

  if (!called || called->empty()) modifyPrimal = true;

  for(unsigned i=0;i<op->getNumArgOperands(); i++) {
    if (gutils->isConstantValue(op->getArgOperand(i)) && called && !called->empty()) {
        continue;
    }

    auto argType = op->getArgOperand(i)->getType();

    if (!argType->isFPOrFPVectorTy()) {
        if (called && ! ( called->hasParamAttribute(i, Attribute::ReadOnly) || called->hasParamAttribute(i, Attribute::ReadNone)) ) {
            modifyPrimal = true;
            //llvm::errs() << "primal modified " << called->getName() << " modified via arg " << i << "\n";
        }
    }
  }

  // Don't need to augment calls that are certain to not hit return
  if (isa<UnreachableInst>(op->getParent()->getTerminator())) {
    llvm::errs() << "augunreachable op " << *op << "\n";
    modifyPrimal = false;
  }

  return modifyPrimal;
}

void handleAugmentedCallInst(TypeResults &TR, CallInst* op, GradientUtils* const gutils, const std::map<Argument*, bool> uncacheable_args, const SmallPtrSetImpl<Instruction*> &returnuses, std::function<unsigned(Instruction*, CacheType)> getIndex, std::map<const llvm::CallInst*, const AugmentedReturn*> &subaugmentations) {
    Function *called = op->getCalledFunction();

    if (auto castinst = dyn_cast<ConstantExpr>(op->getCalledValue())) {
        if (castinst->isCast())
        if (auto fn = dyn_cast<Function>(castinst->getOperand(0))) {
            if (fn->getName() == "malloc" || fn->getName() == "free" || fn->getName() == "_Znwm" || fn->getName() == "_ZdlPv" || fn->getName() == "_ZdlPvm") {
                called = fn;
            }
        }
    }

    if (called && (called->getName() == "printf" || called->getName() == "puts"))
        return;

    // Handle lgamma, safe to recompute so no store/change to forward
    if (called) {
      auto n = called->getName();
      if (n == "lgamma" || n == "lgammaf" || n == "lgammal" || n == "lgamma_r" || n == "lgammaf_r" || n == "lgammal_r"
        || n == "__lgamma_r_finite" || n == "__lgammaf_r_finite" || n == "__lgammal_r_finite" ) {
        return;
      }
    }

    if (called && (called->getName()=="malloc" || called->getName()=="_Znwm")) {
        if (is_value_needed_in_reverse(TR, gutils, gutils->getOriginal(op), /*topLevel*/false)) {
            IRBuilder<> BuilderZ(op);
            gutils->addMalloc(BuilderZ, op, getIndex(gutils->getOriginal(op), CacheType::Self) );
        }
        if (!gutils->isConstantValue(op)) {
            gutils->createAntiMalloc(op, getIndex(gutils->getOriginal(op), CacheType::Shadow));
        }
        return;
    }

    //Remove free's in forward pass so the memory can be used in the reverse pass
    if (called && (called->getName()=="free" ||
        called->getName()=="_ZdlPv" || called->getName()=="_ZdlPvm")) {
        gutils->erase(op);
        return;
    }

    if (gutils->isConstantInstruction(op)) {
        if (op->getNumUses() != 0 && !op->doesNotAccessMemory()) {
            IRBuilder<> BuilderZ(op);
            gutils->addMalloc(BuilderZ, op, getIndex(gutils->getOriginal(op), CacheType::Self) );
        }
        return;
    }
    //llvm::errs() << "creating augmented func call for " << *op << "\n";


    std::set<unsigned> subconstant_args;

      SmallVector<Value*, 8> args;
      SmallVector<DIFFE_TYPE, 8> argsInverted;
      const bool modifyPrimal = shouldAugmentCall(op, gutils);

      IRBuilder<> BuilderZ(op);
      BuilderZ.setFastMathFlags(getFast());

      for(unsigned i=0;i<op->getNumArgOperands(); i++) {
        args.push_back(op->getArgOperand(i));
        //llvm::errs() << " considering arg " << *op << " operand: " << *op->getArgOperand(i) << "\n";

        // For constant args, we should use the more efficient formulation; however, if given a function we call that is either empty or unknown
        //   we will fall back to an implementation that assumes no arguments are constant
        if (gutils->isConstantValue(op->getArgOperand(i)) && called && !called->empty()) {
            subconstant_args.insert(i);
            argsInverted.push_back(DIFFE_TYPE::CONSTANT);
            continue;
        }

        auto argType = op->getArgOperand(i)->getType();

        //if (!argType->isFPOrFPVectorTy() && TR.query(gutils->getOriginal(op->getArgOperand(i)))[{}].isPossiblePointer()) {
        if (!argType->isFPOrFPVectorTy()) {
            argsInverted.push_back(DIFFE_TYPE::DUP_ARG);
            args.push_back(gutils->invertPointerM(op->getArgOperand(i), BuilderZ));

            //Note sometimes whattype mistakenly says something should be constant [because composed of integer pointers alone]
            assert(whatType(argType) == DIFFE_TYPE::DUP_ARG || whatType(argType) == DIFFE_TYPE::CONSTANT);
        } else {
            argsInverted.push_back(DIFFE_TYPE::OUT_DIFF);
            assert(whatType(argType) == DIFFE_TYPE::OUT_DIFF || whatType(argType) == DIFFE_TYPE::CONSTANT);
        }
      }

      bool subretused = op->getNumUses() != 0;

      //We check uses of the original function as that includes potential uses in the return,
      //  specifically consider case where the value returned isn't necessary but the subdifferentialreturn is
      bool subdifferentialreturn = (!gutils->isConstantValue(op));// && (gutils->getOriginal(op)->getNumUses() != 0);

      //! We only need to cache something if it is used in a non return setting (since the backard pass doesnt need to use it if just returned)
        bool hasNonReturnUse = false;//outermostAugmentation;
        for(auto use : op->users()) {
            if (!isa<Instruction>(use) || returnuses.find(cast<Instruction>(use)) == returnuses.end()) {
                hasNonReturnUse = true;
                //llvm::errs() << "shouldCache for " << *op << " use " << *use << "\n";
            }
        }

      //llvm::errs() << " augmp op: " << *op << " modifyPrimal: " << modifyPrimal << " subretused: " << subretused << " subdifferentialreturn: " << subdifferentialreturn << " opuses: " << op->getNumUses() << " ipcount: " << gutils->invertedPointers.count(op) << " constantval: " << gutils->isConstantValue(op) << "\n";

      if (!modifyPrimal) {
        if (hasNonReturnUse && !op->doesNotAccessMemory()) {
          gutils->addMalloc(BuilderZ, op, getIndex(gutils->getOriginal(op), CacheType::Self));
        }
        return;
      }

      Value* newcalled = nullptr;

      unsigned tapeIdx = 0xDEADBEEF;
      unsigned returnIdx = 0XDEADBEEF;
      unsigned differeturnIdx = 0XDEADBEEF;


      if (called) {
        NewFnTypeInfo nextTypeInfo(called);
        int argnum = 0;

        for(auto &arg : called->args()) {
            nextTypeInfo.first.insert(std::pair<Argument*, ValueData>(&arg, TR.query(gutils->getOriginal(op->getArgOperand(argnum)))));
            nextTypeInfo.knownValues.insert(std::pair<Argument*, std::set<int64_t>>(&arg, TR.isConstantInt(gutils->getOriginal(op->getArgOperand(argnum)))));
            argnum++;
        }
        nextTypeInfo.second = TR.query(gutils->getOriginal(op));

        const AugmentedReturn& augmentation = CreateAugmentedPrimal(called, subconstant_args, gutils->TLI, TR.analysis, gutils->AA, /*differentialReturn*/subdifferentialreturn, /*return is used*/subretused, nextTypeInfo, uncacheable_args, false);
        insert_or_assign(subaugmentations, cast<CallInst>(gutils->getOriginal(op)), &augmentation);
        newcalled = augmentation.fn;

        auto found = augmentation.returns.find(AugmentedStruct::Tape);
        if (found != augmentation.returns.end()) {
            tapeIdx = found->second;
        }
        found = augmentation.returns.find(AugmentedStruct::Return);
        if (found != augmentation.returns.end()) {
            returnIdx = found->second;
        }
        found = augmentation.returns.find(AugmentedStruct::DifferentialReturn);
        if (found != augmentation.returns.end()) {
            differeturnIdx = found->second;
        }

      } else {
        tapeIdx = 0;
        if (!op->getType()->isEmptyTy() && !op->getType()->isVoidTy()) {
            returnIdx = 1;
            differeturnIdx = 2;
        }
        IRBuilder<> pre(op);
        newcalled = gutils->invertPointerM(op->getCalledValue(), pre);

        auto ft = cast<FunctionType>(cast<PointerType>(op->getCalledValue()->getType())->getElementType());
        auto res = getDefaultFunctionTypeForAugmentation(ft, /*returnUsed*/true, /*subdifferentialreturn*/true);
        auto fptype = PointerType::getUnqual(FunctionType::get(StructType::get(newcalled->getContext(), res.second), res.first, ft->isVarArg()));
        newcalled = pre.CreatePointerCast(newcalled, PointerType::getUnqual(fptype));
        newcalled = pre.CreateLoad(newcalled);
      }

        CallInst* augmentcall = BuilderZ.CreateCall(newcalled, args);
        assert(augmentcall->getType()->isStructTy());
        augmentcall->setCallingConv(op->getCallingConv());
        augmentcall->setDebugLoc(op->getDebugLoc());

        gutils->originalInstructions.insert(augmentcall);
        gutils->nonconstant.insert(augmentcall);
        augmentcall->setMetadata("enzyme_activity_inst", MDNode::get(augmentcall->getContext(), {MDString::get(augmentcall->getContext(), "active")}));
        if (!gutils->isConstantValue(op)) {
          gutils->nonconstant_values.insert(augmentcall);
        }
        op->setMetadata("enzyme_activity_value", MDNode::get(op->getContext(), {MDString::get(op->getContext(), gutils->isConstantValue(op) ? "const" : "active")}));

        augmentcall->setName(op->getName()+"_augmented");

        Value* tp = BuilderZ.CreateExtractValue(augmentcall, {tapeIdx}, "subcache");
        if (tp->getType()->isEmptyTy()) {
            auto tpt = tp->getType();
            gutils->erase(cast<Instruction>(tp));
            tp = UndefValue::get(tpt);
        }

        gutils->addMalloc(BuilderZ, tp, getIndex(gutils->getOriginal(op), CacheType::Tape) );
        if (gutils->invertedPointers.count(op) != 0) {
            auto placeholder = cast<PHINode>(gutils->invertedPointers[op]);
            gutils->invertedPointers.erase(op);

            if (subdifferentialreturn) {
              auto antiptr = cast<Instruction>(BuilderZ.CreateExtractValue(augmentcall, {differeturnIdx}, "antiptr_" + op->getName() ));
              assert(antiptr->getType() == op->getType());
              gutils->invertedPointers[op] = antiptr;
              placeholder->replaceAllUsesWith(antiptr);

              if (hasNonReturnUse) {
                  gutils->addMalloc(BuilderZ, antiptr, getIndex(gutils->getOriginal(op), CacheType::Shadow) );
              }
            }
            gutils->erase(placeholder);
        }

        if (subretused) {
          auto rv = cast<Instruction>(BuilderZ.CreateExtractValue(augmentcall, {returnIdx}));
          assert(rv->getType() == op->getType());
          gutils->originalInstructions.insert(rv);
          gutils->nonconstant.insert(rv);
          rv->setMetadata("enzyme_activity_inst", MDNode::get(rv->getContext(), {MDString::get(rv->getContext(), "const")}));
          if (!gutils->isConstantValue(op)) {
            gutils->nonconstant_values.insert(rv);
          }
          rv->setMetadata("enzyme_activity_value", MDNode::get(rv->getContext(), {MDString::get(rv->getContext(), gutils->isConstantValue(op) ? "const" : "active")}));
          assert(op->getType() == rv->getType());

          if (gutils->invertedPointers.count(op) != 0) {
              gutils->invertedPointers[rv] = gutils->invertedPointers[op];
              gutils->invertedPointers.erase(op);
          }

          if (hasNonReturnUse) {
            gutils->addMalloc(BuilderZ, rv, getIndex(gutils->getOriginal(op), CacheType::Self) );
          }
          gutils->originalToNewFn[gutils->getOriginal(op)] = rv;
          gutils->replaceAWithB(op,rv);
          std::string nm = op->getName().str();
          op->setName("");
          rv->setName(nm);
        } else {
          gutils->originalToNewFn[gutils->getOriginal(op)] = augmentcall;
        }

        gutils->erase(op);
}

void handleGradientCallInst(TypeResults &TR, IRBuilder <>& Builder2, CallInst* op, DiffeGradientUtils* const gutils, const bool topLevel, const std::map<ReturnInst*,StoreInst*> &replacedReturns, AllocaInst* dretAlloca, const std::map<Argument*, bool> uncacheable_args, std::function<unsigned(Instruction*, CacheType)> getIndex, const bool metaretused, const AugmentedReturn* subdata) {
  Function *called = op->getCalledFunction();

  if (auto castinst = dyn_cast<ConstantExpr>(op->getCalledValue())) {
    if (castinst->isCast()) {
      if (auto fn = dyn_cast<Function>(castinst->getOperand(0))) {
        if (isAllocationFunction(*fn, gutils->TLI) || isDeallocationFunction(*fn, gutils->TLI)) {
          called = fn;
        }
      }
    }
  }

  if (called && (called->getName() == "printf" || called->getName() == "puts")) {
    SmallVector<Value*, 4> args;
    for(unsigned i=0; i<op->getNumArgOperands(); i++) {
      args.push_back(gutils->lookupM(op->getArgOperand(i), Builder2));
    }
    CallInst* cal = Builder2.CreateCall(called, args);
    cal->setAttributes(op->getAttributes());
    cal->setCallingConv(op->getCallingConv());
    cal->setTailCallKind(op->getTailCallKind());
    return;
  }

  bool subretused = op->getNumUses() != 0;
  bool augmentedsubretused = subretused;
  // double check for uses that may have been removed by loads from a cache (specifically if this returns a pointer)
  // Note this is not true: we can safely ignore (and should ignore) return instances, since those are already taken into account by a store if we do need to return them
  if (!subretused) {
    for( auto user : gutils->getOriginal(op)->users()) {
        if (isa<ReturnInst>(user)) {
            continue;
        }
        subretused = true;
        break;
    }
    augmentedsubretused = subretused;
    for( auto user : gutils->getOriginal(op)->users()) {
        if (isa<ReturnInst>(user)) {
            if (metaretused) {
                augmentedsubretused = true;
            }
            continue;
        }
    }
  }

  //llvm::errs() << "newFunc:" << *gutils->oldFunc << "\n";
  //llvm::errs() << "subretused: " << subretused << " metaretused: " << metaretused << " op: " << *op << "\n";

  if (called && isAllocationFunction(*called, gutils->TLI)) {
    bool constval = gutils->isConstantValue(op);
    if (!constval) {
      auto anti = gutils->createAntiMalloc(op, getIndex(gutils->getOriginal(op), CacheType::Shadow));
      Value* tofree = gutils->lookupM(anti, Builder2);
      assert(tofree);
      assert(tofree->getType());
      assert(Type::getInt8Ty(tofree->getContext()));
      assert(PointerType::getUnqual(Type::getInt8Ty(tofree->getContext())));
      assert(Type::getInt8PtrTy(tofree->getContext()));
      freeKnownAllocation(Builder2, tofree, *called, gutils->TLI)->addAttribute(AttributeList::FirstArgIndex, Attribute::NonNull);
    }

    //TODO enable this if we need to free the memory
    // NOTE THAT TOPLEVEL IS THERE SIMPLY BECAUSE THAT WAS PREVIOUS ATTITUTE TO FREE'ing
      Instruction* inst = op;
      if (!topLevel) {
        if (is_value_needed_in_reverse(TR, gutils, gutils->getOriginal(op), /*topLevel*/topLevel)) {
            IRBuilder<> BuilderZ(op);
            inst = gutils->addMalloc(BuilderZ, op, getIndex(gutils->getOriginal(op), CacheType::Self) );
            inst->setMetadata("enzyme_activity_value", MDNode::get(inst->getContext(), {MDString::get(inst->getContext(), constval ? "const" : "active")}));
        } else {
            //Note that here we cannot simply replace with null as users who try to find the shadow pointer will use the shadow of null rather than the true shadow of this
            IRBuilder<> BuilderZ(op);
            auto pn = BuilderZ.CreatePHI(op->getType(), 1, (op->getName()+"_replacement").str());

            pn->setMetadata("enzyme_activity_value", op->getMetadata("enzyme_activity_value"));
            pn->setMetadata("enzyme_activity_inst", op->getMetadata("enzyme_activity_inst"));
            gutils->originalInstructions.insert(pn);

            gutils->fictiousPHIs.push_back(pn);

            gutils->replaceAWithB(op, pn);

            gutils->erase(op);
            inst = nullptr;
            op = nullptr;
        }
      }

    if (topLevel) {
      freeKnownAllocation(Builder2, gutils->lookupM(inst, Builder2), *called, gutils->TLI);
    }
    return;
  }

  if (called && called->getName()=="free") {
    if( gutils->invertedPointers.count(op) ) {
        auto placeholder = cast<PHINode>(gutils->invertedPointers[op]);
        gutils->invertedPointers.erase(op);
        gutils->erase(placeholder);
    }

    llvm::Value* val = op->getArgOperand(0);
    while(auto cast = dyn_cast<CastInst>(val)) val = cast->getOperand(0);

    if (auto dc = dyn_cast<CallInst>(val)) {
      if (dc->getCalledFunction()->getName() == "malloc") {
        gutils->erase(op);
        return;
      }
    }

    if (isa<ConstantPointerNull>(val)) {
      gutils->erase(op);
      llvm::errs() << "removing free of null pointer\n";
      return;
    }

    //TODO HANDLE FREE
    llvm::errs() << "freeing without malloc " << *val << "\n";
    gutils->erase(op);
    return;
  }

  if (called && (called->getName()=="_ZdlPv" || called->getName()=="_ZdlPvm")) {
    if( gutils->invertedPointers.count(op) ) {
        auto placeholder = cast<PHINode>(gutils->invertedPointers[op]);
        gutils->invertedPointers.erase(op);
        gutils->erase(placeholder);
    }

    llvm::Value* val = op->getArgOperand(0);
    while(auto cast = dyn_cast<CastInst>(val)) val = cast->getOperand(0);

    if (auto dc = dyn_cast<CallInst>(val)) {
      if (dc->getCalledFunction()->getName() == "_Znwm") {
        gutils->erase(op);
        return;
      }
    }
    //TODO HANDLE DELETE
    llvm::errs() << "deleting without new " << *val << "\n";
    gutils->erase(op);
    return;
  }

  // Handle lgamma, safe to recompute so no store/change to forward
  if (called) {
    //TODO use doing a different location for the r variants
      auto n = called->getName();
      if (n == "lgamma" || n == "lgammaf" || n == "lgammal" || n == "lgamma_r" || n == "lgammaf_r" || n == "lgammal_r"
        || n == "__lgamma_r_finite" || n == "__lgammaf_r_finite" || n == "__lgammal_r_finite" ) {
        return;
      }
  }

  //llvm::errs() << " considering op: " << *op << " isConstantInstruction:" << gutils->isConstantInstruction(op) << " subretused: " << subretused << " !op->doesNotAccessMemory: " << !op->doesNotAccessMemory() << "\n";
  if (gutils->isConstantInstruction(op)) {
    if (!topLevel && subretused && !op->doesNotAccessMemory()) {
      IRBuilder<> BuilderZ(op);
      auto inst = gutils->addMalloc(BuilderZ, op, getIndex(gutils->getOriginal(op), CacheType::Self) );
      inst->setMetadata("enzyme_activity_value", MDNode::get(inst->getContext(), {MDString::get(inst->getContext(), "const")}));
    }
    return;
  }

  bool modifyPrimal = shouldAugmentCall(op, gutils);

  bool foreignFunction = called == nullptr || called->empty();

  std::set<unsigned> subconstant_args;

  SmallVector<Value*, 8> args;
  SmallVector<Value*, 8> pre_args;
  SmallVector<DIFFE_TYPE, 8> argsInverted;
  IRBuilder<> BuilderZ(op);
  std::vector<Instruction*> postCreate;
  BuilderZ.setFastMathFlags(getFast());

  for(unsigned i=0;i<op->getNumArgOperands(); i++) {
    args.push_back(gutils->lookupM(op->getArgOperand(i), Builder2));
    pre_args.push_back(op->getArgOperand(i));

    if (gutils->isConstantValue(op->getArgOperand(i)) && !foreignFunction) {
      subconstant_args.insert(i);
      argsInverted.push_back(DIFFE_TYPE::CONSTANT);
      continue;
    }

    auto argType = op->getArgOperand(i)->getType();

    if (!argType->isFPOrFPVectorTy()) {
      argsInverted.push_back(DIFFE_TYPE::DUP_ARG);

      if ( argType->isIntOrIntVectorTy() && TR.intType(gutils->getOriginal(op->getArgOperand(i)), /*errifnotfound*/false).isFloat()) {
        args.push_back(Constant::getNullValue(argType));
        pre_args.push_back(Constant::getNullValue(argType));
      } else {
        args.push_back(gutils->invertPointerM(op->getArgOperand(i), Builder2));
        pre_args.push_back(gutils->invertPointerM(op->getArgOperand(i), BuilderZ));
      }

      //Note sometimes whattype mistakenly says something should be constant [because composed of integer pointers alone]
      assert(whatType(argType) == DIFFE_TYPE::DUP_ARG || whatType(argType) == DIFFE_TYPE::CONSTANT);
    } else {
      argsInverted.push_back(DIFFE_TYPE::OUT_DIFF);
      assert(whatType(argType) == DIFFE_TYPE::OUT_DIFF || whatType(argType) == DIFFE_TYPE::CONSTANT);
    }
  }

  bool replaceFunction = false;

  if (topLevel && op->getParent()->getSingleSuccessor() == gutils->reverseBlocks[op->getParent()] && !foreignFunction) {
      auto origop = cast<CallInst>(gutils->getOriginal(op));
      auto OBB = cast<BasicBlock>(gutils->getOriginal(op->getParent()));
                //TODO fix this to be more accurate
      auto iter = OBB->rbegin();
      SmallPtrSet<Instruction*,4> usetree;
      usetree.insert(origop);
      for(auto uinst = origop->getNextNode(); uinst != nullptr; uinst = uinst->getNextNode()) {
        bool usesInst = false;
        for(auto &operand : uinst->operands()) {
          if (auto usedinst = dyn_cast<Instruction>(operand.get())) {
            if (usetree.find(usedinst) != usetree.end()) {
              usesInst = true;
              break;
            }
          }
        }
        if (usesInst) {
          usetree.insert(uinst);
          continue;
        }

        ModRefInfo mri = ModRefInfo::NoModRef;
        if (uinst->mayReadOrWriteMemory()) {
          mri = gutils->AA.getModRefInfo(uinst, origop);
        }

        if (mri == ModRefInfo::NoModRef) { continue; }

        usetree.insert(uinst);

        if (auto li = dyn_cast<LoadInst>(uinst)) {
          for(Instruction* it = uinst; it != nullptr; it = it->getNextNode()) {
              if (auto call = dyn_cast<CallInst>(it)) {
                 if (isCertainMallocOrFree(call->getCalledFunction())) {
                   continue;
                 }
               }
               if (gutils->AA.canInstructionRangeModRef(*it, *it, MemoryLocation::get(li), ModRefInfo::Mod)) {
                  usetree.insert(it);
               }
            }
        }


      }

      while(iter != OBB->rend() && &*iter != origop) {
        //llvm::errs() << " forwardback considering: " << *iter << " origop: " << *origop << "\n";

        if (auto call = dyn_cast<CallInst>(&*iter)) {
          if (isCertainMallocOrFree(call->getCalledFunction())) {
            iter++;
            continue;
          }
        }

        if (auto ri = dyn_cast<ReturnInst>(&*iter)) {
          auto fd = replacedReturns.find(ri);
          if (fd != replacedReturns.end()) {
            auto si = fd->second;
            if (usetree.find(dyn_cast<Instruction>(gutils->getOriginal(si->getValueOperand()))) != usetree.end()) {
              postCreate.push_back(si);
            }
          }
          iter++;
          continue;
        }

        //TODO usesInst has created a bug here
        // if it is used by the reverse pass before this call creates it
        // and thus it loads an undef from cache
        bool usesInst = usetree.find(&*iter) != usetree.end();

        //TODO remove this upon start of more accurate)
        //if (usesInst) break;
        if (!usesInst && (!iter->mayReadOrWriteMemory() || isa<BinaryOperator>(&*iter))) {
          iter++;
          continue;
        }

        ModRefInfo mri = ModRefInfo::NoModRef;
        if (iter->mayReadOrWriteMemory()) {
          mri = gutils->AA.getModRefInfo(&*iter, origop);
          /*
          llvm::errs() << "  **** &AA" << &AA << "\n";
          for(auto& a : AA.AAs) {
            llvm::errs() << " subAA: " << a->getName() << " PTR:" << a.get() << "\n";
          }

          llvm::errs() << " iter: " << *iter << " origop: " << *origop << " mri: ";
          if (mri == ModRefInfo::NoModRef) llvm::errs() << "nomodref";
          if (mri == ModRefInfo::ModRef) llvm::errs() << "modref";
          if (mri == ModRefInfo::Mod) llvm::errs() << "mod";
          if (mri == ModRefInfo::Ref) llvm::errs() << "ref";
          if (mri == ModRefInfo::MustModRef) llvm::errs() << "mustmodref";
          if (mri == ModRefInfo::MustMod) llvm::errs() << "mustmod";
          if (mri == ModRefInfo::MustRef) llvm::errs() << "mustref";
          llvm::errs() << "\n";
          */
        }

        if (mri == ModRefInfo::NoModRef && !usesInst) {
          iter++;
          continue;
        }

        //load that follows the original
        if (auto li = dyn_cast<LoadInst>(&*iter)) {
          bool modref = false;
          for(Instruction* it = li; it != nullptr; it = it->getNextNode()) {
            if (auto call = dyn_cast<CallInst>(it)) {
             if (isCertainMallocOrFree(call->getCalledFunction())) {
               continue;
             }
           }
           if (gutils->AA.canInstructionRangeModRef(*it, *it, MemoryLocation::get(li), ModRefInfo::Mod)) {
            modref = true;
          }
        }

        if (modref)
          break;
        postCreate.push_back(cast<Instruction>(gutils->getNewFromOriginal(&*iter)));
        iter++;
        continue;
      }

                  //call that follows the original
      if (auto li = dyn_cast<IntrinsicInst>(&*iter)) {
        if (li->getIntrinsicID() == Intrinsic::memcpy) {
         auto mem0 = gutils->AA.getModRefInfo(&*iter, li->getOperand(0), MemoryLocation::UnknownSize);
         auto mem1 = gutils->AA.getModRefInfo(&*iter, li->getOperand(1), MemoryLocation::UnknownSize);

         llvm::errs() << "modrefinfo for mem0 " << *li->getOperand(0) << " " << (unsigned int)mem0 << "\n";
         llvm::errs() << "modrefinfo for mem1 " << *li->getOperand(1) << " " << (unsigned int)mem1 << "\n";
                      //TODO TEMPORARILY REMOVED THIS TO TEST SOMETHING
                      // PUT BACK THIS CONDITION!!
                      //if ( !isModSet(mem0) )
         {
          bool modref = false;
          for(Instruction* it = li; it != nullptr; it = it->getNextNode()) {
            if (auto call = dyn_cast<CallInst>(it)) {
             if (isCertainMallocOrFree(call->getCalledFunction())) {
               continue;
             }
           }
           if (gutils->AA.canInstructionRangeModRef(*it, *it, li->getOperand(1), MemoryLocation::UnknownSize, ModRefInfo::Mod)) {
            modref = true;
            llvm::errs() << " inst  found mod " << *iter << " " << *it << "\n";
          }
        }

        if (modref)
          break;
        postCreate.push_back(cast<Instruction>(gutils->getNewFromOriginal(&*iter)));
        iter++;
        continue;
      }
    }
  }

  if (usesInst) {
    bool modref = false;
                      //auto start = &*iter;
    if (mri != ModRefInfo::NoModRef) {
      modref = true;
                          /*
                          for(Instruction* it = start; it != nullptr; it = it->getNextNode()) {
                              if (auto call = dyn_cast<CallInst>(it)) {
                                   if (isCertainMallocOrFree(call->getCalledFunction())) {
                                       continue;
                                   }
                              }
                              if ( (isa<StoreInst>(start) || isa<CallInst>(start)) && AA.canInstructionRangeModRef(*it, *it, MemoryLocation::get(start), ModRefInfo::Mod)) {
                                  modref = true;
                          llvm::errs() << " inst  found mod " << *iter << " " << *it << "\n";
                              }
                          }
                          */
    }

    if (modref)
      break;

    //If already did forward reverse replacement for iter, we cannot put it into post create (since it would have a dependence on our output, making a circular dependency)
    if (gutils->originalToNewFn.find(&*iter) == gutils->originalToNewFn.end()) {
      break;
    }

    postCreate.push_back(cast<Instruction>(gutils->getNewFromOriginal(&*iter)));
    iter++;
    continue;
  }

  break;
  }
  if (&*iter == gutils->getOriginal(op)) {
    User* outsideuse = nullptr;
    //If we don't post dominate a user, consider what outside use entails?
    for(auto user : op->users()) {
      if (gutils->originalInstructions.find(cast<Instruction>(user)) == gutils->originalInstructions.end()) {
        if (StoreInst* si = dyn_cast<StoreInst>(user)) {
            bool returned = false;
            for(auto rep : replacedReturns) {
                if (rep.second == si) {
                    returned = true;
                    break;
                }
            }
            if (returned) continue;
        }
        outsideuse = user;
      }
    }

    if (subretused && isa<PointerType>(op->getType())) {
      if (called)
        llvm::errs() << " [not implemented] pointer return for combined forward/reverse " << called->getName() << "\n";
      else
        llvm::errs() << " [not implemented] pointer return for combined forward/reverse " << *op->getCalledValue() << "\n";
      outsideuse = op;
    }

    if (!outsideuse) {
      if (called)
        llvm::errs() << " choosing to replace function " << (called->getName()) << " and do both forward/reverse\n";
      else
        llvm::errs() << " choosing to replace function " << (*op->getCalledValue()) << " and do both forward/reverse\n";
      replaceFunction = true;
      modifyPrimal = false;
    } else {
      if (called)
        llvm::errs() << " failed to replace function (cacheuse)" << (called->getName()) << " due to " << *outsideuse << "\n";
      else
        llvm::errs() << " failed to replace function (cacheuse)" << (*op->getCalledValue()) << " due to " << *outsideuse << "\n";

    }
  } else {
    if (called)
      llvm::errs() << " failed to replace function " << (called->getName()) << " due to " << *iter << "\n";
    else
      llvm::errs() << " failed to replace function " << (*op->getCalledValue()) << " due to " << *iter << "\n";
  }
  }

  Value* tape = nullptr;
  CallInst* augmentcall = nullptr;
  Instruction* cachereplace = nullptr;

  bool constval = gutils->isConstantValue(op);

  NewFnTypeInfo nextTypeInfo(called);
  int argnum = 0;

  if (called) {
      std::map<Value*, std::set<int64_t>> intseen;

      for(auto &arg : called->args()) {
        nextTypeInfo.first.insert(std::pair<Argument*, ValueData>(&arg, TR.query(gutils->getOriginal(op->getArgOperand(argnum)))));
        nextTypeInfo.knownValues.insert(std::pair<Argument*, std::set<int64_t>>(&arg, TR.isConstantInt(gutils->getOriginal(op->getArgOperand(argnum)))));

        argnum++;
      }
      nextTypeInfo.second = TR.query(gutils->getOriginal(op));
  }

  //TODO consider what to do if called == nullptr for augmentation
  //llvm::Optional<std::map<std::pair<Instruction*, std::string>, unsigned>> sub_index_map;
  unsigned tapeIdx = 0xDEADBEEF;
  unsigned returnIdx = 0xDEADBEEF;
  unsigned differetIdx = 0xDEADBEEF;

  if (modifyPrimal) {

    Value* newcalled = nullptr;
    const AugmentedReturn* fnandtapetype = nullptr;

    bool subdifferentialreturn = (!gutils->isConstantValue(op));// && augmentedsubdifferet;

    if (!called) {
        IRBuilder<> pre(op);
        newcalled = gutils->invertPointerM(op->getCalledValue(), pre);

        auto ft = cast<FunctionType>(cast<PointerType>(op->getCalledValue()->getType())->getElementType());
        auto res = getDefaultFunctionTypeForAugmentation(ft, /*returnUsed*/true, /*subdifferentialreturn*/true);
        auto fptype = PointerType::getUnqual(FunctionType::get(StructType::get(newcalled->getContext(), res.second), res.first, ft->isVarArg()));
        newcalled = pre.CreatePointerCast(newcalled, PointerType::getUnqual(fptype));
        newcalled = pre.CreateLoad(newcalled);
        tapeIdx = 0;

        if (!ft->getReturnType()->isVoidTy() && !ft->getReturnType()->isFPOrFPVectorTy()) {
            returnIdx = 1;
            differetIdx = 2;
        }

    } else {
        if (topLevel)
            subdata = &CreateAugmentedPrimal(cast<Function>(called), subconstant_args, gutils->TLI, TR.analysis, gutils->AA, /*differentialReturns*/subdifferentialreturn, /*return is used*/augmentedsubretused, nextTypeInfo, uncacheable_args, false);
        if (!subdata) {
            llvm::errs() << *gutils->oldFunc->getParent() << "\n";
            llvm::errs() << *gutils->oldFunc << "\n";
            llvm::errs() << *gutils->newFunc << "\n";
            llvm::errs() << *called << "\n";
        }
        assert(subdata);
        fnandtapetype = subdata;
        newcalled = subdata->fn;

        auto found = subdata->returns.find(AugmentedStruct::DifferentialReturn);
        if (found != subdata->returns.end()) {
            differetIdx = found->second;
        }

        found = subdata->returns.find(AugmentedStruct::Return);
        if (found != subdata->returns.end()) {
            returnIdx = found->second;
        }

        found = subdata->returns.find(AugmentedStruct::Tape);
        if (found != subdata->returns.end()) {
            tapeIdx = found->second;
        }

    }
        //sub_index_map = fnandtapetype.tapeIndices;

        //llvm::errs() << "seeing sub_index_map of " << sub_index_map->size() << " in ap " << cast<Function>(called)->getName() << "\n";
        if (topLevel) {

          assert(newcalled);
          FunctionType* FT = cast<FunctionType>(cast<PointerType>(newcalled->getType())->getElementType());
          if (false) {
        badaugmentedfn:;
                auto NC = dyn_cast<Function>(newcalled);
                llvm::errs() << *gutils->oldFunc << "\n";
                llvm::errs() << *gutils->newFunc << "\n";
                if (NC)
                llvm::errs() << " trying to call " << NC->getName() << " " << *FT << "\n";
                else
                llvm::errs() << " trying to call " << *newcalled << " " << *FT << "\n";

                for(unsigned i=0; i<pre_args.size(); i++) {
                    assert(pre_args[i]);
                    assert(pre_args[i]->getType());
                    llvm::errs() << "args[" << i << "] = " << *pre_args[i] << " FT:" << *FT->getParamType(i) << "\n";
                }
                assert(0 && "calling with wrong number of arguments");
                exit(1);

          }

          if (pre_args.size() != FT->getNumParams()) goto badaugmentedfn;

          for(unsigned i=0; i<pre_args.size(); i++) {
            if (pre_args[i]->getType() != FT->getParamType(i)) goto badaugmentedfn;
          }

          augmentcall = BuilderZ.CreateCall(newcalled, pre_args);
          augmentcall->setCallingConv(op->getCallingConv());
          augmentcall->setDebugLoc(op->getDebugLoc());

          gutils->originalInstructions.insert(augmentcall);
          gutils->nonconstant.insert(augmentcall);
          augmentcall->setMetadata("enzyme_activity_inst", MDNode::get(augmentcall->getContext(), {MDString::get(augmentcall->getContext(), "active")}));

          if (!constval) {
            gutils->nonconstant_values.insert(augmentcall);
          }
          augmentcall->setMetadata("enzyme_activity_value", MDNode::get(augmentcall->getContext(), {MDString::get(augmentcall->getContext(), constval ? "const" : "active")}));

          augmentcall->setName(op->getName()+"_augmented");

          tape = BuilderZ.CreateExtractValue(augmentcall, {tapeIdx});
          if (tape->getType()->isEmptyTy()) {
            auto tt = tape->getType();
            gutils->erase(cast<Instruction>(tape));
            tape = UndefValue::get(tt);
          }
        } else {
          tape = gutils->addMalloc(BuilderZ, tape, getIndex(gutils->getOriginal(op), CacheType::Tape) );

          if (!topLevel && subretused) {
            cachereplace = BuilderZ.CreatePHI(op->getType(), 1);
            cachereplace = gutils->addMalloc(BuilderZ, cachereplace, getIndex(gutils->getOriginal(op), CacheType::Self) );
            cachereplace->setMetadata("enzyme_activity_value", MDNode::get(cachereplace->getContext(), {MDString::get(cachereplace->getContext(), constval ? "const" : "active")}));
          }
        }

        //llvm::errs() << "considering augmenting: " << *op << "\n";
        if( gutils->invertedPointers.count(op) ) {

            auto placeholder = cast<PHINode>(gutils->invertedPointers[op]);
            //llvm::errs() << " +  considering placeholder: " << *placeholder << "\n";

            bool subcheck = subdifferentialreturn && !op->getType()->isFPOrFPVectorTy() && !gutils->isConstantValue(op);

            //! We only need the shadow pointer if it is used in a non return setting
                    bool hasNonReturnUse = false;//outermostAugmentation;
                    for(auto use : gutils->getOriginal(op)->users()) {
                        if (!isa<ReturnInst>(use)) { // || returnuses.find(cast<Instruction>(use)) == returnuses.end()) {
                            hasNonReturnUse = true;
                            //llvm::errs() << "shouldCache for " << *op << " use " << *use << "\n";
                        }
                    }

            if( subcheck && hasNonReturnUse) {
                Value* newip = nullptr;
                if (topLevel) {
                    newip = BuilderZ.CreateExtractValue(augmentcall, {differetIdx}, op->getName()+"'ac");
                    assert(newip->getType() == op->getType());
                    placeholder->replaceAllUsesWith(newip);
                } else {
                    newip = gutils->addMalloc(BuilderZ, placeholder, getIndex(gutils->getOriginal(op), CacheType::Shadow) );
                }

                gutils->invertedPointers[op] = newip;

                if (topLevel) {
                    gutils->erase(placeholder);
                } else {
                    /* don't need to erase if not toplevel since that is handled by addMalloc */
                }
            } else {
                gutils->invertedPointers.erase(op);
                gutils->erase(placeholder);
            }
        }

        if (fnandtapetype && fnandtapetype->tapeType) {
          auto tapep = BuilderZ.CreatePointerCast(tape, PointerType::getUnqual(fnandtapetype->tapeType));
          auto truetape = BuilderZ.CreateLoad(tapep);
          truetape->setMetadata("enzyme_mustcache", MDNode::get(truetape->getContext(), {}));

          CallInst* ci = cast<CallInst>(CallInst::CreateFree(tape, &*BuilderZ.GetInsertPoint()));
          ci->addAttribute(AttributeList::FirstArgIndex, Attribute::NonNull);
          tape = truetape;
        }

        if (fnandtapetype) {
        if (!tape->getType()->isStructTy()) {
          llvm::errs() << "gutils->oldFunc: " << *gutils->oldFunc << "\n";
          llvm::errs() << "gutils->newFunc: " << *gutils->newFunc << "\n";
          llvm::errs() << "tape: " << *tape << "\n";
        }
        assert(tape->getType()->isStructTy());
        }

  } else {
      if( gutils->invertedPointers.count(op) ) {
        auto placeholder = cast<PHINode>(gutils->invertedPointers[op]);
        gutils->invertedPointers.erase(op);
        gutils->erase(placeholder);
      }
    if (!topLevel && subretused && !op->doesNotAccessMemory()) {
      assert(!replaceFunction);
      cachereplace = IRBuilder<>(op).CreatePHI(op->getType(), 1);
      cachereplace = gutils->addMalloc(BuilderZ, cachereplace, getIndex(gutils->getOriginal(op), CacheType::Self) );
      cachereplace->setMetadata("enzyme_activity_value", MDNode::get(cachereplace->getContext(), {MDString::get(cachereplace->getContext(), constval ? "const" : "active")}));
    }
  }

  bool retUsed = replaceFunction && subretused;
  Value* newcalled = nullptr;

  bool subdiffereturn = (!gutils->isConstantValue(op));
  bool subdretptr = (!gutils->isConstantValue(op)) && ( op->getType()->isPointerTy() || op->getType()->isIntOrIntVectorTy()) && replaceFunction;
  //llvm::errs() << "subdifferet:" << subdiffereturn << " " << *op << "\n";
  bool subtopLevel = replaceFunction || !modifyPrimal;
  if (called) {
    newcalled = CreatePrimalAndGradient(cast<Function>(called), subconstant_args, gutils->TLI, TR.analysis, gutils->AA, /*returnValue*/retUsed, /*subdiffereturn*/subdiffereturn, /*subdretptr*/subdretptr, /*topLevel*/subtopLevel, tape ? tape->getType() : nullptr, nextTypeInfo, uncacheable_args, subdata);//, LI, DT);
  } else {

    assert(!subtopLevel);

    newcalled = gutils->invertPointerM(op->getCalledValue(), Builder2);

    auto ft = cast<FunctionType>(cast<PointerType>(op->getCalledValue()->getType())->getElementType());
    auto res = getDefaultFunctionTypeForGradient(ft, subdiffereturn);
    //TODO Note there is empty tape added here, replace with generic
    res.first.push_back(Type::getInt8PtrTy(newcalled->getContext()));
    auto fptype = PointerType::getUnqual(FunctionType::get(StructType::get(newcalled->getContext(), res.second), res.first, ft->isVarArg()));
    newcalled = Builder2.CreatePointerCast(newcalled, PointerType::getUnqual(fptype));
    newcalled = Builder2.CreateLoad(Builder2.CreateConstGEP1_64(newcalled, 1));
  }

  if (subdiffereturn && op->getType()->isFPOrFPVectorTy()) {
    args.push_back(gutils->diffe(op, Builder2));
  }

  if (tape) {
    auto ntape = gutils->lookupM(tape, Builder2);
    assert(ntape);
    assert(ntape->getType());
    args.push_back(ntape);
  }

  assert(newcalled);
  //if (auto NC = dyn_cast<Function>(newcalled)) {
  FunctionType* FT = cast<FunctionType>(cast<PointerType>(newcalled->getType())->getElementType());

  if (false) {
badfn:;
        auto NC = dyn_cast<Function>(newcalled);
        llvm::errs() << *gutils->oldFunc << "\n";
        llvm::errs() << *gutils->newFunc << "\n";
        if (NC)
        llvm::errs() << " trying to call " << NC->getName() << " " << *FT << "\n";
        else
        llvm::errs() << " trying to call " << *newcalled << " " << *FT << "\n";

        for(unsigned i=0; i<args.size(); i++) {
            assert(args[i]);
            assert(args[i]->getType());
            llvm::errs() << "args[" << i << "] = " << *args[i] << " FT:" << *FT->getParamType(i) << "\n";
        }
        assert(0 && "calling with wrong number of arguments");
        exit(1);

  }

  if (args.size() != FT->getNumParams()) goto badfn;

  for(unsigned i=0; i<args.size(); i++) {
    if (args[i]->getType() != FT->getParamType(i)) goto badfn;
  }

  CallInst* diffes = Builder2.CreateCall(newcalled, args);
  diffes->setCallingConv(op->getCallingConv());
  diffes->setDebugLoc(op->getDebugLoc());

  unsigned structidx = retUsed ? 1 : 0;
  if (subdretptr) structidx++;

  for(unsigned i=0;i<op->getNumArgOperands(); i++) {
    if (argsInverted[i] == DIFFE_TYPE::OUT_DIFF) {
      Value* diffeadd = Builder2.CreateExtractValue(diffes, {structidx});
      structidx++;
      gutils->addToDiffe(op->getArgOperand(i), diffeadd, Builder2, TR.intType(gutils->getOriginal(op->getArgOperand(i)), false).isFloat());
    }
  }

  assert(cast<StructType>(diffes->getType())->getNumElements() == structidx);

  //TODO this shouldn't matter because this can't use itself, but setting null should be done before other sets but after load of diffe
  if (subretused && !gutils->isConstantValue(op))
    gutils->setDiffe(op, Constant::getNullValue(op->getType()), Builder2);

  gutils->originalInstructions.insert(diffes);
  gutils->nonconstant.insert(diffes);

  diffes->setMetadata("enzyme_activity_inst", MDNode::get(diffes->getContext(), {MDString::get(diffes->getContext(), "active")}));

  if (!gutils->isConstantValue(op)) {
    gutils->nonconstant_values.insert(diffes);
  }
  diffes->setMetadata("enzyme_activity_value", MDNode::get(diffes->getContext(), {MDString::get(diffes->getContext(), gutils->isConstantValue(op) ? "const" : "active")}));

  if (replaceFunction) {

    //if a function is replaced for joint forward/reverse, handle inverted pointers
    if (gutils->invertedPointers.count(op)) {
        auto placeholder = cast<PHINode>(gutils->invertedPointers[op]);
        gutils->invertedPointers.erase(op);
        if (subdretptr) {
            dumpMap(gutils->invertedPointers);
            auto dretval = cast<Instruction>(Builder2.CreateExtractValue(diffes, {1}));
            /* todo handle this case later */
            assert(!subretused);
            gutils->invertedPointers[op] = dretval;
        }
        gutils->erase(placeholder);
    }

    ValueToValueMapTy mapp;
    if (subretused) {
      auto retval = cast<Instruction>(Builder2.CreateExtractValue(diffes, {0}));
      gutils->originalInstructions.insert(retval);
      gutils->nonconstant.insert(retval);
      retval->setMetadata("enzyme_activity_inst", MDNode::get(retval->getContext(), {MDString::get(retval->getContext(), "const")}));
      if (!gutils->isConstantValue(op)) {
        gutils->nonconstant_values.insert(retval);
      }
      retval->setMetadata("enzyme_activity_value", MDNode::get(retval->getContext(), {MDString::get(retval->getContext(), gutils->isConstantValue(op) ? "const" : "active")}));
      op->replaceAllUsesWith(retval);
      mapp[op] = retval;
    }

    for (auto &a : *op->getParent()) {
      if (&a != op) {
        mapp[&a] = &a;
      }
    }

    for (auto &a : *gutils->reverseBlocks[op->getParent()]) {
      mapp[&a] = &a;
    }

    std::reverse(postCreate.begin(), postCreate.end());
    for(auto a : postCreate) {
      for(unsigned i=0; i<a->getNumOperands(); i++) {
        a->setOperand(i, gutils->unwrapM(a->getOperand(i), Builder2, mapp, true));
      }
      llvm::errs() << "moving instruction for postcreate: " << *a << "\n";
      a->moveBefore(*Builder2.GetInsertBlock(), Builder2.GetInsertPoint());
    }

    gutils->erase(op);

    return;
  }

  if (augmentcall || cachereplace) {

    if (subretused) {
      Value* dcall = nullptr;
      if (augmentcall) {
        dcall = BuilderZ.CreateExtractValue(augmentcall, {returnIdx});
        assert(dcall->getType() == op->getType());
        if (auto dinst = dyn_cast<Instruction>(dcall)) {
          dinst->setMetadata("enzyme_activity_value", MDNode::get(dcall->getContext(), {MDString::get(dcall->getContext(), constval ? "const" : "active")}));
        }
      }
      if (cachereplace) {
        assert(dcall == nullptr);
        dcall = cachereplace;
      }

      if (Instruction* inst = dyn_cast<Instruction>(dcall))
          gutils->originalInstructions.insert(inst);
      gutils->nonconstant.insert(dcall);
      if (!gutils->isConstantValue(op))
        gutils->nonconstant_values.insert(dcall);

    if (!gutils->isConstantValue(op)) {
      if (!op->getType()->isFPOrFPVectorTy() && TR.query(gutils->getOriginal(op))[{}].isPossiblePointer()) {
        gutils->invertedPointers[dcall] = gutils->invertedPointers[op];
        gutils->invertedPointers.erase(op);
      } else {
        gutils->differentials[dcall] = gutils->differentials[op];
        gutils->differentials.erase(op);
      }
    }
    op->replaceAllUsesWith(dcall);
    auto name = op->getName();
    op->setName("");
    if (dcall) {
      dcall->setName(name);
    }
  }

  gutils->erase(op);

  if (augmentcall)
    gutils->replaceableCalls.insert(augmentcall);
  } else {
    gutils->replaceableCalls.insert(op);
  }
}

Function* CreatePrimalAndGradient(Function* todiff, const std::set<unsigned>& constant_args, TargetLibraryInfo &TLI,
                                  TypeAnalysis &TA, AAResults &global_AA, bool returnUsed, bool differentialReturn, bool dretPtr, bool topLevel, llvm::Type* additionalArg,
                                  const NewFnTypeInfo& oldTypeInfo, const std::map<Argument*, bool> _uncacheable_args,
                                  const AugmentedReturn* augmenteddata) {
  //if (additionalArg && !additionalArg->isStructTy()) {
  //    llvm::errs() << *todiff << "\n";
  //    llvm::errs() << "addl arg: " << *additionalArg << "\n";
  //}
  if (additionalArg) assert(additionalArg->isStructTy() || (additionalArg == Type::getInt8PtrTy(additionalArg->getContext()) )  );
  if (differentialReturn) assert(!todiff->getReturnType()->isVoidTy());
  static std::map<std::tuple<Function*,std::set<unsigned>/*constant_args*/, std::map<Argument*, bool>/*uncacheable_args*/, bool/*retval*/, bool/*differentialReturn*/, bool/*dretptr*/, bool/*topLevel*/, llvm::Type*, const NewFnTypeInfo>, Function*> cachedfunctions;
  auto tup = std::make_tuple(todiff, std::set<unsigned>(constant_args.begin(), constant_args.end()), std::map<Argument*, bool>(_uncacheable_args.begin(), _uncacheable_args.end()), returnUsed, differentialReturn, dretPtr, topLevel, additionalArg, oldTypeInfo);
  if (cachedfunctions.find(tup) != cachedfunctions.end()) {
    return cachedfunctions.find(tup)->second;
  }

  //Whether we shuold actually return the value
  bool returnValue = returnUsed && topLevel;
  //llvm::errs() << " returnValue: " << returnValue <<  "toplevel: " << topLevel << " func: " << todiff->getName() << "\n";

  bool hasTape = false;

  if (constant_args.size() == 0 && !topLevel && !returnValue && hasMetadata(todiff, "enzyme_gradient")) {

      auto md = todiff->getMetadata("enzyme_gradient");
      if (!isa<MDTuple>(md)) {
          llvm::errs() << *todiff << "\n";
          llvm::errs() << *md << "\n";
          report_fatal_error("unknown gradient for noninvertible function -- metadata incorrect");
      }
      auto md2 = cast<MDTuple>(md);
      assert(md2->getNumOperands() == 1);
      auto gvemd = cast<ConstantAsMetadata>(md2->getOperand(0));
      auto foundcalled = cast<Function>(gvemd->getValue());

      auto res = getDefaultFunctionTypeForGradient(todiff->getFunctionType(), /*differentialReturn*/differentialReturn);


      if (foundcalled->arg_size() == res.first.size() + 1 /*tape*/) {
        auto lastarg = foundcalled->arg_end();
        lastarg--;
        res.first.push_back(lastarg->getType());
        hasTape = true;
      } else if (foundcalled->arg_size() == res.first.size()) {
        res.first.push_back(StructType::get(todiff->getContext(), {}));
      } else {
          llvm::errs() << "expected args: [";
          for(auto a : res.first) {
                llvm::errs() << *a << " ";
          }
          llvm::errs() << "]\n";
          llvm::errs() << *foundcalled << "\n";
          assert(0 && "bad type for custom gradient");
      }

      auto st = dyn_cast<StructType>(foundcalled->getReturnType());
      bool wrongRet = st == nullptr;
      if (wrongRet || !hasTape) {
        FunctionType *FTy = FunctionType::get(StructType::get(todiff->getContext(), {res.second}), res.first, todiff->getFunctionType()->isVarArg());
        Function *NewF = Function::Create(FTy, Function::LinkageTypes::InternalLinkage, "fixgradient_"+todiff->getName(), todiff->getParent());
        NewF->setAttributes(foundcalled->getAttributes());
        if (NewF->hasFnAttribute(Attribute::NoInline)) {
            NewF->removeFnAttr(Attribute::NoInline);
        }
          for (Argument &Arg : NewF->args()) {
              if (Arg.hasAttribute(Attribute::Returned))
                  Arg.removeAttr(Attribute::Returned);
              if (Arg.hasAttribute(Attribute::StructRet))
                  Arg.removeAttr(Attribute::StructRet);
          }

        BasicBlock *BB = BasicBlock::Create(NewF->getContext(), "entry", NewF);
        IRBuilder <>bb(BB);
        SmallVector<Value*,4> args;
        for(auto &a : NewF->args()) args.push_back(&a);
        if (!hasTape) {
            args.pop_back();
        }
        llvm::errs() << *NewF << "\n";
        llvm::errs() << *foundcalled << "\n";
        auto cal = bb.CreateCall(foundcalled, args);
        cal->setCallingConv(foundcalled->getCallingConv());
        Value* val = cal;
        if (wrongRet) {
            auto ut = UndefValue::get(NewF->getReturnType());
            if (val->getType()->isEmptyTy() && res.second.size() == 0) {
                val = ut;
            } else if (res.second.size() == 1 && res.second[0] == val->getType()) {
                val = bb.CreateInsertValue(ut, cal, {0u});
            } else {
                llvm::errs() << *foundcalled << "\n";
                assert(0 && "illegal type for reverse");
            }
        }
        bb.CreateRet(val);
        foundcalled = NewF;
      }
      return insert_or_assign(cachedfunctions, tup, foundcalled)->second;
  }

  assert(!todiff->empty());
  auto M = todiff->getParent();

  auto& Context = M->getContext();
  AAResults AA(TLI);
  //AA.addAAResult(global_AA);
  DiffeGradientUtils *gutils = DiffeGradientUtils::CreateFromClone(todiff, TLI, TA, AA, constant_args, returnValue ? ( dretPtr ? ReturnType::ArgsWithTwoReturns: ReturnType::ArgsWithReturn ) : ReturnType::Args, differentialReturn, additionalArg);
  insert_or_assign(cachedfunctions, tup, gutils->newFunc);

  const SmallPtrSet<BasicBlock*, 4> guaranteedUnreachable = getGuaranteedUnreachable(gutils->oldFunc);

  SmallPtrSet<Value*, 4> assumeTrue;
  SmallPtrSet<Value*, 4> assumeFalse;

  if (!topLevel) {
    //TODO also can consider switch instance as well
    // TODO can also insert to topLevel as well [note this requires putting the intrinsic at the correct location]
    for(auto& BB : *gutils->oldFunc) {
      std::vector<BasicBlock*> unreachables;
      std::vector<BasicBlock*> reachables;
      for(auto Succ : successors(&BB)) {
        if (guaranteedUnreachable.find(Succ) != guaranteedUnreachable.end()) {
          unreachables.push_back(Succ);
        } else {
          reachables.push_back(Succ);
        }
      }

      if (unreachables.size() == 0 || reachables.size() == 0) continue;

      if (auto bi = dyn_cast<BranchInst>(BB.getTerminator())) {
        IRBuilder<> B(&gutils->newFunc->getEntryBlock().front());

        if (auto inst = dyn_cast<Instruction>(bi->getCondition())) {
          B.SetInsertPoint(gutils->getNewFromOriginal(inst)->getNextNode());
        }

        Value* vals[1] = { gutils->getNewFromOriginal(bi->getCondition()) };
        if (bi->getSuccessor(0) == unreachables[0]) {
          assumeFalse.insert(vals[0]);
          vals[0] = B.CreateNot(vals[0]);
        } else {
          assumeTrue.insert(vals[0]);
        }
        B.CreateCall(Intrinsic::getDeclaration(M, Intrinsic::assume), vals);
      }
    }
  }

  gutils->forceContexts();

  NewFnTypeInfo typeInfo(gutils->oldFunc);
  {
    auto toarg = todiff->arg_begin();
    auto olarg = gutils->oldFunc->arg_begin();
    for(; toarg != todiff->arg_end(); toarg++, olarg++) {

      {
      auto fd = oldTypeInfo.first.find(toarg);
      assert(fd != oldTypeInfo.first.end());
      typeInfo.first.insert(std::pair<Argument*, ValueData>(olarg, fd->second));
      }

      {
      auto cfd = oldTypeInfo.knownValues.find(toarg);
      assert(cfd != oldTypeInfo.knownValues.end());
      typeInfo.knownValues.insert(std::pair<Argument*, std::set<int64_t>>(olarg, cfd->second));
      }
    }
    typeInfo.second = oldTypeInfo.second;
  }

  TypeResults TR = TA.analyzeFunction(typeInfo);
  assert(TR.info.function == gutils->oldFunc);

  gutils->forceActiveDetection(AA, TR);
  gutils->forceAugmentedReturns(TR, guaranteedUnreachable);

  std::map<std::pair<Instruction*, CacheType>,unsigned> mapping;
  if (augmenteddata) mapping = augmenteddata->tapeIndices;

  auto getIndex = [&](Instruction* I, CacheType u)-> unsigned {
    return gutils->getIndex( std::make_pair(I, u), mapping);
  };

  // Convert uncacheable args from the input function to the preprocessed function
  std::map<Argument*, bool> _uncacheable_argsPP;
  {
    auto in_arg = todiff->arg_begin();
    auto pp_arg = gutils->oldFunc->arg_begin();
    for(; pp_arg != gutils->oldFunc->arg_end(); ) {
        _uncacheable_argsPP[pp_arg] = _uncacheable_args.find(in_arg)->second;
        pp_arg++;
        in_arg++;
    }
  }

  const std::map<CallInst*, const std::map<Argument*, bool> > uncacheable_args_map = (augmenteddata) ? augmenteddata->uncacheable_args_map :
      compute_uncacheable_args_for_callsites(gutils->oldFunc, gutils->DT, TLI, AA, gutils, _uncacheable_argsPP);

  const std::map<Instruction*, bool> can_modref_map =  augmenteddata ? augmenteddata->can_modref_map : compute_uncacheable_load_map(gutils, AA, TLI, _uncacheable_argsPP);
  /*
    for (auto &iter : can_modref_map_mutable) {
      if (iter.second) {
        //llvm::errs() << "isneeded: " << is_needed << " gradient can_modref_map: " << *iter.first << " fn: " << gutils->oldFunc->getName() << "\n";
      } else {
        //llvm::errs() << "gradient can_modref_map: " << *iter.first << "\n";
      }
    }
  */

  gutils->can_modref_map = &can_modref_map;

  Value* additionalValue = nullptr;
  if (additionalArg) {
    auto v = gutils->newFunc->arg_end();
    v--;
    additionalValue = v;
    assert(!topLevel);
    assert(augmenteddata);

    if (!additionalValue->getType()->isStructTy()) {
        assert(augmenteddata->tapeType);
        IRBuilder<> BuilderZ(gutils->inversionAllocs);
        auto tapep = BuilderZ.CreatePointerCast(additionalValue, PointerType::getUnqual(augmenteddata->tapeType));
        LoadInst* truetape = BuilderZ.CreateLoad(tapep);
        truetape->setMetadata("enzyme_mustcache", MDNode::get(truetape->getContext(), {}));

        CallInst* ci = cast<CallInst>(CallInst::CreateFree(additionalValue, truetape));//&*BuilderZ.GetInsertPoint()));
        ci->moveAfter(truetape);
        ci->addAttribute(AttributeList::FirstArgIndex, Attribute::NonNull);
        additionalValue = truetape;
    }

    if (!additionalValue->getType()->isStructTy()) {
        llvm::errs() << *gutils->oldFunc << "\n";
        llvm::errs() << *gutils->newFunc << "\n";
        llvm::errs() << "el incorrect tape type: " << *additionalValue << "\n";
    }
    assert(additionalValue->getType()->isStructTy());
    //TODO here finish up making recursive structs simply pass in i8*
    //blahblahblah
    gutils->setTape(additionalValue);
  }

  Argument* differetval = nullptr;
  if (differentialReturn && todiff->getReturnType()->isFPOrFPVectorTy()) {
    auto endarg = gutils->newFunc->arg_end();
    endarg--;
    if (additionalArg) endarg--;
    differetval = endarg;
    if (differetval->getType() != todiff->getReturnType()) {
        llvm::errs() << *gutils->oldFunc << "\n";
        llvm::errs() << *gutils->newFunc << "\n";
    }
    assert(differetval->getType() == todiff->getReturnType());
  }

  // Explicitly handle all returns first to ensure that return instructions know if they are used or not before
  //   processessing instructions
  std::map<ReturnInst*,StoreInst*> replacedReturns;
  llvm::AllocaInst* retAlloca = nullptr;
  llvm::AllocaInst* dretAlloca = nullptr;
  if (returnValue) {
    retAlloca = IRBuilder<>(&gutils->newFunc->getEntryBlock().front()).CreateAlloca(todiff->getReturnType(), nullptr, "toreturn");
    if (dretPtr && !todiff->getReturnType()->isFPOrFPVectorTy() && !topLevel) {
        dretAlloca = IRBuilder<>(&gutils->newFunc->getEntryBlock().front()).CreateAlloca(todiff->getReturnType(), nullptr, "dtoreturn");
    }

  }

  //! Ficticious values with TBAA to use for constant detection algos until everything is made fully ahead of time
  //   Note that we need to delete the tbaa tags from these values once we finish / before verification
  std::vector<Instruction*> fakeTBAA;

  for(BasicBlock* BB: gutils->originalBlocks) {
    if(ReturnInst* op = dyn_cast<ReturnInst>(BB->getTerminator())) {
      Value* retval = op->getReturnValue();
      IRBuilder<> rb(op);
      rb.setFastMathFlags(getFast());

      if (retAlloca) {
        StoreInst* si = rb.CreateStore(retval, retAlloca);
        replacedReturns[cast<ReturnInst>(gutils->getOriginal(op))] = si;

        if (dretAlloca && !gutils->isConstantValue(retval)) {
            rb.CreateStore(gutils->invertPointerM(retval, rb), dretAlloca);
        }
      } else {
        /*
         TODO should do DCE ideally
        if (auto inst = dyn_cast<Instruction>(retval)) {
            SmallSetVector<Instruction *, 16> WorkList;
            DCEInstruction(inst, WorkList, TLI);
            while (!WorkList.empty()) {
                Instruction *I = WorkList.pop_back_val();
                MadeChange |= DCEInstruction(I, WorkList, TLI);
            }
        }
        */
      }

      //returns nonvoid value
      if (retval != nullptr) {

          //differential float return
          if (differentialReturn && todiff->getReturnType()->isFPOrFPVectorTy() && !gutils->isConstantValue(retval)) {
            IRBuilder <>reverseB(gutils->reverseBlocks[BB]);
            gutils->setDiffe(retval, differetval, reverseB);
          }

      //returns void, should not have a return allocation
      } else {
        assert (retAlloca == nullptr);
      }

      rb.CreateBr(gutils->reverseBlocks[BB]);
      gutils->erase(op);
    }
  }

  for(BasicBlock* BB: gutils->originalBlocks) {
    // Don't create derivatives for code that results in termination
    BasicBlock* oBB = gutils->getOriginal(BB);
    if (guaranteedUnreachable.find(oBB) != guaranteedUnreachable.end()) continue;

    auto BB2 = gutils->reverseBlocks[BB];
    assert(BB2);

    IRBuilder<> Builder2(BB2);
    //if (BB2->size() > 0) {
    //    Builder2.SetInsertPoint(BB2->getFirstNonPHI());
    //}
    Builder2.setFastMathFlags(getFast());

    std::function<Value*(Value*)> lookup = [&](Value* val) -> Value* {
      return gutils->lookupM(val, Builder2);
    };

    auto diffe = [&Builder2,&gutils](Value* val) -> Value* {
        return gutils->diffe(val, Builder2);
    };

    auto addToDiffe = [&Builder2,&gutils](Value* val, Value* dif, Type* T) -> void {
      gutils->addToDiffe(val, dif, Builder2, T);
    };

    auto setDiffe = [&](Value* val, Value* toset) -> void {
      gutils->setDiffe(val, toset, Builder2);
    };

  auto term = BB->getTerminator();
  assert(term);
  if (isa<ReturnInst>(term) || isa<BranchInst>(term) || isa<SwitchInst>(term)) {

  } else {
    assert(BB);
    assert(BB->getParent());
    assert(term);
    llvm::errs() << *BB->getParent() << "\n";
    llvm::errs() << "unknown terminator instance " << *term << "\n";
    assert(0 && "unknown terminator inst");
  }


  BasicBlock::reverse_iterator I = oBB->rbegin(), E = oBB->rend();
  I++;
  for (; I != E; I++) {
    Instruction* inst = gutils->getNewFromOriginal(&*I);
    assert(inst);

    assert(TR.info.function == gutils->oldFunc);
    DerivativeMaker<const AugmentedReturn*> maker( topLevel ? DerivativeMode::Both : DerivativeMode::Reverse, gutils, TR, getIndex, uncacheable_args_map, /*returnuses*/nullptr, augmenteddata, &fakeTBAA);
    //maker.visit(*inst);

    if (isa<BinaryOperator>(inst)) {
      maker.visit(*inst);
      Builder2.SetInsertPoint(BB2);
    } else if(auto op = dyn_cast<CallInst>(inst)) {
      switch(getIntrinsicForCallSite(op, &TLI)) {
        case Intrinsic::not_intrinsic:
            /*
             //Note that for some reason pow test requires ffast-math and such still because otherwise pow isn't marked as readonly and thus
             // the above call doesn't work
            llvm::errs() << " op: " << *op << "\n";
            llvm::errs() << " locallinkage: " << op->getCalledFunction()->hasLocalLinkage() << "\n";
            llvm::errs() << " onlyreadsmemory: " << op->onlyReadsMemory() << "\n";
            LibFunc Func;
            TLI.getLibFunc(*op->getCalledFunction(), Func);
            llvm::errs() << "g TLI libfunc: " << Func << " pow: " << LibFunc_pow << "\n";
            */
            goto realcall;
        default: {
          maker.visit(*inst);
          Builder2.SetInsertPoint(BB2);
          break;
        }
      }
      continue;
realcall:
      const AugmentedReturn* subdata = nullptr;
      //llvm::errs() << " consdering op: " << *op << " toplevel" << topLevel << " ad: " << augmenteddata << "\n";
      if (!topLevel) {
        if (augmenteddata) {
            auto fd = augmenteddata->subaugmentations.find(cast<CallInst>(gutils->getOriginal(op)));
            if (fd != augmenteddata->subaugmentations.end()) {
                subdata = fd->second;
            }
        }
      }
      auto orig = gutils->getOriginal(op);

      if (uncacheable_args_map.find(orig) == uncacheable_args_map.end()) {
          llvm::errs() << "op: " << *op << "(" << op->getParent()->getParent()->getName() << ") " << " orig:" << *orig << "(" << orig->getParent()->getParent()->getName() << ")\n";
          llvm::errs() << "uncacheable_args_map:\n";
          for(auto a : uncacheable_args_map) {
            llvm::errs() << " + " << *a.first << "(" << a.first->getParent()->getParent()->getName() << ")\n";
          }
      }
      assert(uncacheable_args_map.find(orig) != uncacheable_args_map.end());
      handleGradientCallInst(TR, Builder2, op, gutils, topLevel, replacedReturns, dretAlloca, uncacheable_args_map.find(orig)->second, getIndex, returnUsed, subdata); //topLevel ? augmenteddata->subaugmentations[cast<CallInst>(gutils->getOriginal(op))] : nullptr);
    } else if(auto op = dyn_cast_or_null<SelectInst>(inst)) {
      if (gutils->isConstantValue(inst)) continue;
      if (op->getType()->isPointerTy()) continue;

      Value* dif1 = nullptr;
      Value* dif2 = nullptr;

      if (!gutils->isConstantValue(op->getOperand(1)))
        dif1 = Builder2.CreateSelect(lookup(op->getOperand(0)), diffe(inst), Constant::getNullValue(op->getOperand(1)->getType()), "diffe"+op->getOperand(1)->getName());
      if (!gutils->isConstantValue(op->getOperand(2)))
        dif2 = Builder2.CreateSelect(lookup(op->getOperand(0)), Constant::getNullValue(op->getOperand(2)->getType()), diffe(inst), "diffe"+op->getOperand(2)->getName());

      setDiffe(inst, Constant::getNullValue(inst->getType()));
      if (dif1) addToDiffe(op->getOperand(1), dif1, TR.intType(gutils->getOriginal(op->getOperand(1)), false).isFloat());
      if (dif2) addToDiffe(op->getOperand(2), dif2, TR.intType(gutils->getOriginal(op->getOperand(2)), false).isFloat());
    } else if(isa<LoadInst>(inst) || isa<StoreInst>(inst)) {
      maker.visit(*inst);
      Builder2.SetInsertPoint(BB2);
    } else if(auto op = dyn_cast<ExtractValueInst>(inst)) {
      if (gutils->isConstantValue(inst)) continue;
      if (op->getType()->isPointerTy()) continue;

      auto prediff = diffe(inst);
      //todo const
      if (!gutils->isConstantValue(op->getOperand(0))) {
        SmallVector<Value*,4> sv;
        for(auto i : op->getIndices())
            sv.push_back(ConstantInt::get(Type::getInt32Ty(Context), i));
        gutils->addToDiffeIndexed(op->getOperand(0), prediff, sv, Builder2);
      }
      setDiffe(inst, Constant::getNullValue(inst->getType()));
    } else if(auto op = dyn_cast<InsertValueInst>(inst)) {
      if (gutils->isConstantValue(inst)) continue;
      auto st = cast<StructType>(op->getType());
      bool hasNonPointer = false;
      for(unsigned i=0; i<st->getNumElements(); i++) {
        if (!st->getElementType(i)->isPointerTy()) {
           hasNonPointer = true;
        }
      }
      if (!hasNonPointer) continue;

      if (!gutils->isConstantValue(op->getInsertedValueOperand()) && !op->getInsertedValueOperand()->getType()->isPointerTy()) {
        auto prediff = gutils->diffe(inst, Builder2);
        auto dindex = Builder2.CreateExtractValue(prediff, op->getIndices());
        gutils->addToDiffe(op->getOperand(1), dindex, Builder2, TR.intType(gutils->getOriginal(op->getOperand(1)), false).isFloat());
      }

      if (!gutils->isConstantValue(op->getAggregateOperand()) && !op->getAggregateOperand()->getType()->isPointerTy()) {
        auto prediff = gutils->diffe(inst, Builder2);
        auto dindex = Builder2.CreateInsertValue(prediff, Constant::getNullValue(op->getInsertedValueOperand()->getType()), op->getIndices());
        gutils->addToDiffe(op->getAggregateOperand(), dindex, Builder2, TR.intType(gutils->getOriginal(op->getAggregateOperand()), false).isFloat());
      }

      gutils->setDiffe(inst, Constant::getNullValue(inst->getType()), Builder2);
    } else if (auto op = dyn_cast<ShuffleVectorInst>(inst)) {
      if (gutils->isConstantValue(inst)) continue;

      auto loaded = diffe(inst);
      size_t l1 = cast<VectorType>(op->getOperand(0)->getType())->getNumElements();
      uint64_t instidx = 0;
      for( size_t idx : op->getShuffleMask()) {
        auto opnum = (idx < l1) ? 0 : 1;
        auto opidx = (idx < l1) ? idx : (idx-l1);
        SmallVector<Value*,4> sv;
        sv.push_back(ConstantInt::get(Type::getInt32Ty(Context), opidx));
        if (!gutils->isConstantValue(op->getOperand(opnum)))
          gutils->addToDiffeIndexed(op->getOperand(opnum), Builder2.CreateExtractElement(loaded, instidx), sv, Builder2);
        instidx++;
      }
      setDiffe(inst, Constant::getNullValue(inst->getType()));
    } else if(auto op = dyn_cast<ExtractElementInst>(inst)) {
      if (gutils->isConstantValue(inst)) continue;

      if (!gutils->isConstantValue(op->getVectorOperand())) {
        SmallVector<Value*,4> sv;
        sv.push_back(op->getIndexOperand());
        gutils->addToDiffeIndexed(op->getVectorOperand(), diffe(inst), sv, Builder2);
      }
      setDiffe(inst, Constant::getNullValue(inst->getType()));
    } else if(auto op = dyn_cast<InsertElementInst>(inst)) {
      if (gutils->isConstantValue(inst)) continue;

      auto dif1 = diffe(inst);

      if (!gutils->isConstantValue(op->getOperand(0)))
        addToDiffe(op->getOperand(0), Builder2.CreateInsertElement(dif1, Constant::getNullValue(op->getOperand(1)->getType()), lookup(op->getOperand(2))), TR.intType(gutils->getOriginal(op->getOperand(0)), false).isFloat());

      if (!gutils->isConstantValue(op->getOperand(1)))
        addToDiffe(op->getOperand(1), Builder2.CreateExtractElement(dif1, lookup(op->getOperand(2))), TR.intType(gutils->getOriginal(op->getOperand(1)), false).isFloat());

      setDiffe(inst, Constant::getNullValue(inst->getType()));
    } else if(auto op = dyn_cast<CastInst>(inst)) {
      if (gutils->isConstantValue(inst)) continue;
      if (op->getType()->isPointerTy() || op->getOpcode() == CastInst::CastOps::PtrToInt) continue;

      if (!gutils->isConstantValue(op->getOperand(0))) {
        if (op->getOpcode()==CastInst::CastOps::FPTrunc || op->getOpcode()==CastInst::CastOps::FPExt) {
          addToDiffe(op->getOperand(0), Builder2.CreateFPCast(diffe(inst), op->getOperand(0)->getType()), TR.intType(gutils->getOriginal(op->getOperand(0)), false).isFloat());
        } else if (op->getOpcode()==CastInst::CastOps::BitCast) {
          addToDiffe(op->getOperand(0), Builder2.CreateBitCast(diffe(inst), op->getOperand(0)->getType()), TR.intType(gutils->getOriginal(op->getOperand(0)), false).isFloat());
        } else if (op->getOpcode()==CastInst::CastOps::Trunc) {
          //TODO CHECK THIS
          auto trunced = Builder2.CreateZExt(diffe(inst), op->getOperand(0)->getType());
          addToDiffe(op->getOperand(0), trunced, TR.intType(gutils->getOriginal(op->getOperand(0)), false).isFloat());
        } else {
            llvm::errs() << *inst->getParent()->getParent() << "\n" << *inst->getParent() << "\n";
            llvm::errs() << "cannot handle above cast " << *inst << "\n";
            report_fatal_error("unknown instruction");
        }
      }
      setDiffe(inst, Constant::getNullValue(inst->getType()));
    } else if(isa<CmpInst>(inst) || isa<PHINode>(inst) || isa<BranchInst>(inst) || isa<SwitchInst>(inst) || isa<AllocaInst>(inst) || isa<CastInst>(inst) || isa<GetElementPtrInst>(inst)) {
        continue;
    } else {
      assert(inst);
      assert(inst->getParent());
      assert(inst->getParent()->getParent());
      llvm::errs() << *inst->getParent()->getParent() << "\n" << *inst->getParent() << "\n";
      llvm::errs() << "cannot handle above inst " << *inst << "\n";
      report_fatal_error("unknown instruction");
    }
  }

    createInvertedTerminator(TR, gutils, BB, retAlloca, dretAlloca, 0 + (additionalArg ? 1 : 0) + (differentialReturn && todiff->getReturnType()->isFPOrFPVectorTy() ? 1 : 0));

  }

  if (!topLevel)
    gutils->eraseStructuralStoresAndCalls();

  for(auto inst: fakeTBAA) {
      inst->setMetadata(LLVMContext::MD_tbaa, nullptr);
  }

  for(auto val : assumeTrue) {
    bool changed;
    do {
      changed = false;
      for(auto &use : val->uses()) {
        if (auto user = dyn_cast<IntrinsicInst>(use.getUser())) {
          if (user->getIntrinsicID() == Intrinsic::assume) continue;
        }
        use.set(ConstantInt::getTrue(val->getContext()));
        changed = true;
        break;
      }
    }while (!changed);
  }

  for(auto val : assumeFalse) {
    bool changed;
    do {
      changed = false;
      for(auto &use : val->uses()) {
        if (auto notu = dyn_cast<BinaryOperator>(use.getUser())) {
          if (notu->getNumUses() == 1 && notu->getOpcode() == BinaryOperator::Xor && notu->getOperand(0) == val && isa<ConstantInt>(notu->getOperand(1)) && cast<ConstantInt>(notu->getOperand(1))->isOne()) {
            if (auto user = dyn_cast<IntrinsicInst>(*notu->user_begin())) {
              if (user->getIntrinsicID() == Intrinsic::assume) {
                continue;
              }
            }
          }
        }
        use.set(ConstantInt::getFalse(val->getContext()));
        changed = true;
        break;
      }
    } while (!changed);
  }

  while(gutils->inversionAllocs->size() > 0) {
    gutils->inversionAllocs->back().moveBefore(gutils->newFunc->getEntryBlock().getFirstNonPHIOrDbgOrLifetime());
  }

  (IRBuilder <>(gutils->inversionAllocs)).CreateUnreachable();
  DeleteDeadBlock(gutils->inversionAllocs);
  for(auto BBs : gutils->reverseBlocks) {
    if (pred_begin(BBs.second) == pred_end(BBs.second)) {
        (IRBuilder <>(BBs.second)).CreateUnreachable();
        DeleteDeadBlock(BBs.second);
    }
  }

  for (Argument &Arg : gutils->newFunc->args()) {
      if (Arg.hasAttribute(Attribute::Returned))
          Arg.removeAttr(Attribute::Returned);
      if (Arg.hasAttribute(Attribute::StructRet))
          Arg.removeAttr(Attribute::StructRet);
  }
  if (gutils->newFunc->hasFnAttribute(Attribute::OptimizeNone))
    gutils->newFunc->removeFnAttr(Attribute::OptimizeNone);

  if (auto bytes = gutils->newFunc->getDereferenceableBytes(llvm::AttributeList::ReturnIndex)) {
    AttrBuilder ab;
    ab.addDereferenceableAttr(bytes);
    gutils->newFunc->removeAttributes(llvm::AttributeList::ReturnIndex, ab);
  }
  if (gutils->newFunc->hasAttribute(llvm::AttributeList::ReturnIndex, llvm::Attribute::NoAlias)) {
    gutils->newFunc->removeAttribute(llvm::AttributeList::ReturnIndex, llvm::Attribute::NoAlias);
  }
  if (gutils->newFunc->hasAttribute(llvm::AttributeList::ReturnIndex, llvm::Attribute::ZExt)) {
    gutils->newFunc->removeAttribute(llvm::AttributeList::ReturnIndex, llvm::Attribute::ZExt);
  }

  if (llvm::verifyFunction(*gutils->newFunc, &llvm::errs())) {
      llvm::errs() << *gutils->oldFunc << "\n";
      llvm::errs() << *gutils->newFunc << "\n";
      report_fatal_error("function failed verification (4)");
  }

  gutils->cleanupActiveDetection();

  optimizeIntermediate(gutils, topLevel, gutils->newFunc);

  auto nf = gutils->newFunc;
  delete gutils;

  return nf;
}
