/******************************************************************************
 * Copyright (c) 2020 Philipp Schubert.
 * All rights reserved. This program and the accompanying materials are made
 * available under the terms of LICENSE.txt.
 *
 * Contributors:
 *     Philipp Schubert and others
 *****************************************************************************/

#ifndef PHASAR_PHASARLLVM_IFDSIDE_LLVMFLOWFUNCTIONS_H
#define PHASAR_PHASARLLVM_IFDSIDE_LLVMFLOWFUNCTIONS_H

#include "phasar/PhasarLLVM/DataFlowSolver/IfdsIde/FlowFunctions.h"
#include "phasar/PhasarLLVM/DataFlowSolver/IfdsIde/LLVMZeroValue.h"
#include "phasar/Utils/LLVMShorthands.h"

#include "llvm/IR/AbstractCallSite.h"
#include "llvm/IR/Instructions.h"

#include <functional>
#include <memory>
#include <set>
#include <vector>

namespace llvm {
class Value;
class Use;
class Function;
class Instruction;
} // namespace llvm

namespace psr {

/**
 * A flow function that can be wrapped around another flow function
 * in order to kill unnecessary temporary values that are no longer
 * in use, but otherwise would be still propagated through the exploded
 * super-graph.
 * @brief Automatically kills temporary loads that are no longer in use.
 */
class AutoKillTMPs : public FlowFunction<const llvm::Value *> {
protected:
  FlowFunctionPtrType delegate;
  const llvm::Instruction *inst;

public:
  AutoKillTMPs(FlowFunctionPtrType FF, const llvm::Instruction *In)
      : delegate(std::move(FF)), inst(In) {}
  virtual ~AutoKillTMPs() = default;

  container_type computeTargets(const llvm::Value *Source) override {
    container_type Result = delegate->computeTargets(Source);
    for (const llvm::Use &U : inst->operands()) {
      if (llvm::isa<llvm::LoadInst>(U)) {
        Result.erase(U);
      }
    }
    return Result;
  }
};

//===----------------------------------------------------------------------===//
// Mapping functions

/**
 * A predicate can be used to specify additional requirements for the
 * propagation.
 * @brief Propagates all non pointer parameters alongside the call site.
 */
template <typename Container = std::set<const llvm::Value *>>
class MapFactsAlongsideCallSite
    : public FlowFunction<const llvm::Value *, Container> {
  using typename FlowFunction<const llvm::Value *, Container>::container_type;

protected:
  const llvm::CallBase *CB;
  std::function<bool(const llvm::CallBase *, const llvm::Value *)> Predicate;

public:
  MapFactsAlongsideCallSite(
      const llvm::CallBase *CB,
      std::function<bool(const llvm::CallBase *, const llvm::Value *)>
          Predicate =
              [](const llvm::CallBase *CB, const llvm::Value *V) {
                // Checks if a values is involved in a call, i.e. may be
                // modified by a callee, in which case its flow is controlled by
                // getCallFlowFunction() and getRetFlowFunction().
                bool Involved = false;
                for (auto &Arg : CB->args()) {
                  if (Arg == V && V->getType()->isPointerTy()) {
                    Involved = true;
                  }
                }
                return Involved;
              })
      : CB(CB), Predicate(std::move(Predicate)){};
  virtual ~MapFactsAlongsideCallSite() = default;

  container_type computeTargets(const llvm::Value *Source) override {
    // always propagate the zero fact
    if (!LLVMZeroValue::getInstance()->isLLVMZeroValue(Source)) {
      return {Source};
    }
    // propagate if predicate does not hold, i.e. fact is not involved in the
    // call
    if (!Predicate(CB, Source)) {
      return {Source};
    }
    // Pass ZeroValue as is
    if (LLVMZeroValue::getInstance()->isLLVMZeroValue(Source)) {
      return {Source};
    }
    // otherwise kill fact
    return {};
  }
};

/**
 * A predicate can be used to specifiy additonal requirements for mapping
 * actual parameter into formal parameter.
 * @brief Generates all valid formal parameter in the callee context.
 */
template <typename Container = std::set<const llvm::Value *>>
class MapFactsToCallee : public FlowFunction<const llvm::Value *, Container> {
  using typename FlowFunction<const llvm::Value *, Container>::container_type;

protected:
  const llvm::Function *DestFun;
  std::vector<const llvm::Value *> Actuals{};
  std::vector<const llvm::Value *> Formals{};
  std::function<bool(const llvm::Value *)> Predicate;

public:
  MapFactsToCallee(
      const llvm::CallBase *CB, const llvm::Function *DestFun,
      std::function<bool(const llvm::Value *)> Predicate =
          [](const llvm::Value *) { return true; })
      : DestFun(DestFun), Predicate(std::move(Predicate)) {
    // Set up the actual parameters
    for (const auto &Actual : CB->args()) {
      Actuals.push_back(Actual);
    }
    // Set up the formal parameters
    for (const auto &Formal : DestFun->args()) {
      Formals.push_back(&Formal);
    }
  }

  ~MapFactsToCallee() override = default;

  container_type computeTargets(const llvm::Value *Source) override {
    // If DestFun is a declaration we cannot follow this call, we thus need to
    // kill everything
    if (DestFun->isDeclaration()) {
      return {};
    }
    if (!LLVMZeroValue::getInstance()->isLLVMZeroValue(Source)) {
      container_type Res;
      // Handle C-style varargs functions
      if (DestFun->isVarArg()) {
        // Map actual parameters to corresponding formal parameters.
        for (unsigned Idx = 0; Idx < Actuals.size(); ++Idx) {
          if (Source == Actuals[Idx] && Predicate(Actuals[Idx])) {
            if (Idx >= DestFun->arg_size()) {
              // Over-approximate by trying to add the
              //   alloca [1 x %struct.__va_list_tag], align 16
              // to the results
              // find the allocated %struct.__va_list_tag and generate it
              for (const auto &BB : *DestFun) {
                for (const auto &I : BB) {
                  if (const auto *Alloc =
                          llvm::dyn_cast<llvm::AllocaInst>(&I)) {
                    if (Alloc->getAllocatedType()->isArrayTy() &&
                        Alloc->getAllocatedType()->getArrayNumElements() > 0 &&
                        Alloc->getAllocatedType()
                            ->getArrayElementType()
                            ->isStructTy() &&
                        Alloc->getAllocatedType()
                                ->getArrayElementType()
                                ->getStructName() == "struct.__va_list_tag") {
                      Res.insert(Alloc);
                    }
                  }
                }
              }
            } else {
              assert(Idx < Formals.size() &&
                     "Out of bound access to formal parameters!");
              Res.insert(Formals[Idx]); // corresponding formal
            }
          }
        }
        return Res;
      } else {
        // Handle ordinary case
        // Map actual parameters to corresponding formal parameters.
        for (unsigned Idx = 0; Idx < Actuals.size(); ++Idx) {
          if (Source == Actuals[Idx] && Predicate(Actuals[Idx])) {
            assert(Idx < Formals.size() &&
                   "Out of bound access to formal parameters!");
            Res.insert(Formals[Idx]); // corresponding formal
          }
        }
        return Res;
      }
    } else {
      // Pass ZeroValue as is
      return {Source};
    }
  }
};

/**
 * Predicates can be used to specify additional requirements for mapping
 * actual parameters into formal parameters and the return value.
 * @note Currently, the return value predicate only allows checks regarding
 * the callee method.
 * @brief Generates all valid actual parameters and the return value in the
 * caller context.
 */
template <typename Container = std::set<const llvm::Value *>>
class MapFactsToCaller : public FlowFunction<const llvm::Value *, Container> {
  using typename FlowFunction<const llvm::Value *, Container>::container_type;

private:
  const llvm::CallBase *CB;
  const llvm::Function *CalleeFun;
  const llvm::ReturnInst *ExitSite;
  std::vector<const llvm::Value *> Actuals;
  std::vector<const llvm::Value *> Formals;
  std::function<bool(const llvm::Value *)> ParamPredicate;
  std::function<bool(const llvm::Function *)> ReturnPredicate;

public:
  MapFactsToCaller(
      const llvm::CallBase *CB, const llvm::Function *CalleeFun,
      const llvm::Instruction *ExitSite,
      std::function<bool(const llvm::Value *)> ParamPredicate =
          [](const llvm::Value *) { return true; },
      std::function<bool(const llvm::Function *)> ReturnPredicate =
          [](const llvm::Function *) { return true; })
      : CB(CB), CalleeFun(CalleeFun),
        ExitSite(llvm::dyn_cast<llvm::ReturnInst>(ExitSite)),
        ParamPredicate(std::move(ParamPredicate)),
        ReturnPredicate(std::move(ReturnPredicate)) {
    assert(ExitSite && "Should not be null");
    // Set up the actual parameters
    for (const auto &Actual : CB->args()) {
      Actuals.push_back(Actual);
    }
    // Set up the formal parameters
    for (const auto &Formal : CalleeFun->args()) {
      Formals.push_back(&Formal);
    }
  }

  ~MapFactsToCaller() override = default;

  // std::set<const llvm::Value *>
  container_type computeTargets(const llvm::Value *Source) override {
    assert(!CalleeFun->isDeclaration() &&
           "Cannot perform mapping to caller for function declaration");
    if (!LLVMZeroValue::getInstance()->isLLVMZeroValue(Source)) {
      container_type Res;

      // Handle C-style varargs functions
      if (CalleeFun->isVarArg()) {
        const llvm::Instruction *AllocVarArg;
        // Find the allocation of %struct.__va_list_tag
        for (const auto &BB : *CalleeFun) {
          for (const auto &I : BB) {
            if (const auto *Alloc = llvm::dyn_cast<llvm::AllocaInst>(&I)) {
              if (Alloc->getAllocatedType()->isArrayTy() &&
                  Alloc->getAllocatedType()->getArrayNumElements() > 0 &&
                  Alloc->getAllocatedType()
                      ->getArrayElementType()
                      ->isStructTy() &&
                  Alloc->getAllocatedType()
                          ->getArrayElementType()
                          ->getStructName() == "struct.__va_list_tag") {
                AllocVarArg = Alloc;
                // TODO break out this nested loop earlier (without goto ;-)
              }
            }
          }
        }
        // Generate the varargs things by using an over-approximation
        if (Source == AllocVarArg) {
          for (unsigned Idx = Formals.size(); Idx < Actuals.size(); ++Idx) {
            Res.insert(Actuals[Idx]);
          }
        }
      }
      // Handle ordinary case
      // Map formal parameter into corresponding actual parameter.
      for (unsigned Idx = 0; Idx < Formals.size(); ++Idx) {
        if (Source == Formals[Idx] && ParamPredicate(Formals[Idx])) {
          Res.insert(Actuals[Idx]); // corresponding actual
        }
      }
      // Collect return value facts
      if (Source == ExitSite->getReturnValue() && ReturnPredicate(CalleeFun)) {
        Res.insert(CB);
      }
      return Res;
    } else {
      // Pass ZeroValue as is
      return {Source};
    }
  }
};

//===----------------------------------------------------------------------===//
// Propagation flow functions

template <typename D> class PropagateLoad : public FlowFunction<D> {
protected:
  const llvm::LoadInst *Load;

public:
  PropagateLoad(const llvm::LoadInst *L) : Load(L) {}
  virtual ~PropagateLoad() = default;

  std::set<D> computeTargets(D source) override {
    if (source == Load->getPointerOperand()) {
      return {source, Load};
    }
    return {source};
  }
};

template <typename D> class PropagateStore : public FlowFunction<D> {
protected:
  const llvm::StoreInst *Store;

public:
  PropagateStore(const llvm::StoreInst *S) : Store(S) {}
  virtual ~PropagateStore() = default;

  std::set<D> computeTargets(D source) override {
    if (Store->getValueOperand() == source) {
      return {source, Store->getPointerOperand()};
    }
    return {source};
  }
};

//===----------------------------------------------------------------------===//
// Update flow functions

template <typename D> class StrongUpdateStore : public FlowFunction<D> {
protected:
  const llvm::StoreInst *Store;
  std::function<bool(D)> Predicate;

public:
  StrongUpdateStore(const llvm::StoreInst *S, std::function<bool(D)> P)
      : Store(S), Predicate(P) {}
  virtual ~StrongUpdateStore() = default;

  std::set<D> computeTargets(D source) override {
    if (source == Store->getPointerOperand()) {
      return {};
    } else if (Predicate(source)) {
      return {source, Store->getPointerOperand()};
    } else {
      return {source};
    }
  }
};

} // namespace psr

#endif // PHASAR_PHASARLLVM_IFDSIDE_LLVMFLOWFUNCTIONS_H
