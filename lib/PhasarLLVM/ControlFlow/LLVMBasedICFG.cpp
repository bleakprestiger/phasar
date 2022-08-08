/******************************************************************************
 * Copyright (c) 2017 Philipp Schubert.
 * All rights reserved. This program and the accompanying materials are made
 * available under the terms of LICENSE.txt.
 *
 * Contributors:
 *     Philipp Schubert and others
 *****************************************************************************/

/*
 * LLVMBasedInterproceduralICFG.cpp
 *
 *  Created on: 09.09.2016
 *      Author: pdschbrt
 */

#include "phasar/PhasarLLVM/ControlFlow/LLVMBasedICFG.h"
#include "phasar/DB/ProjectIRDB.h"
#include "phasar/PhasarLLVM/ControlFlow/CFGBase.h"
#include "phasar/PhasarLLVM/ControlFlow/LLVMBasedCFG.h"
#include "phasar/PhasarLLVM/ControlFlow/Resolver/CallGraphAnalysisType.h"
#include "phasar/PhasarLLVM/ControlFlow/Resolver/Resolver.h"
#include "phasar/PhasarLLVM/Pointer/LLVMPointsToInfo.h"
#include "phasar/PhasarLLVM/Pointer/LLVMPointsToSet.h"
#include "phasar/PhasarLLVM/TypeHierarchy/LLVMTypeHierarchy.h"
#include "phasar/PhasarLLVM/Utils/LLVMBasedContainerConfig.h"
#include "phasar/Utils/LLVMShorthands.h"
#include "phasar/Utils/MaybeUniquePtr.h"
#include "phasar/Utils/PAMMMacros.h"
#include "phasar/Utils/Soundness.h"
#include "phasar/Utils/TypeTraits.h"
#include "phasar/Utils/Utilities.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include "boost/graph/graphviz.hpp"

#include <utility>

namespace psr {
struct LLVMBasedICFG::Builder {
  ProjectIRDB *IRDB = nullptr;
  LLVMTypeHierarchy *TH = nullptr;
  MaybeUniquePtr<LLVMPointsToInfo> PT{};
  std::unique_ptr<Resolver> Res = nullptr;
  llvm::DenseSet<const llvm::Function *> VisitedFunctions{};
  llvm::SmallVector<llvm::Function *, 1> UserEntryPoints{};
  llvm::DenseMap<const llvm::Function *, vertex_t> *FunctionVertexMap = nullptr;

  llvm::Function *GlobalCleanupFn = nullptr;

  llvm::SmallDenseMap<const llvm::Module *, llvm::Function *>
      GlobalRegisteredDtorsCaller{};

  // The worklist for direct callee resolution.
  llvm::SmallVector<const llvm::Function *, 0> FunctionWL{};

  // Map indirect calls to the number of possible targets found for it. Fixpoint
  // is not reached when more targets are found.
  llvm::DenseMap<const llvm::Instruction *, unsigned> IndirectCalls{};

  void initEntryPoints(llvm::ArrayRef<std::string> EntryPoints);
  void initGlobalsAndWorkList(LLVMBasedICFG *ICFG, bool IncludeGlobals);
  bidigraph_t buildCallGraph(Soundness S);

  /// \returns FixPointReached
  bool processFunction(bidigraph_t &Callgraph, const llvm::Function *F);
  /// \returns FoundNewTargets
  bool constructDynamicCall(bidigraph_t &CallGraph,
                            const llvm::Instruction *CS);
};

void LLVMBasedICFG::Builder::initEntryPoints(
    llvm::ArrayRef<std::string> EntryPoints) {
  if (EntryPoints.size() == 1 && EntryPoints.front() == "__ALL__") {
    // Handle the special case in which a user wishes to treat all functions as
    // entry points.
    for (const auto *Fun : IRDB->getAllFunctions()) {
      if (!Fun->isDeclaration() && Fun->hasName()) {
        UserEntryPoints.push_back(IRDB->getFunctionDefinition(Fun->getName()));
      }
    }
  } else {
    UserEntryPoints.reserve(EntryPoints.size());
    for (const auto &EntryPoint : EntryPoints) {
      auto *F = IRDB->getFunctionDefinition(EntryPoint);
      if (F == nullptr) {
        PHASAR_LOG_LEVEL(WARNING,
                         "Could not retrieve function for entry point '"
                             << EntryPoint << "'");
        continue;
      }
      UserEntryPoints.push_back(F);
    }
  }
}

void LLVMBasedICFG::Builder::initGlobalsAndWorkList(LLVMBasedICFG *ICFG,
                                                    bool IncludeGlobals) {
  FunctionWL.reserve(IRDB->getAllFunctions().size());
  if (IncludeGlobals) {
    assert(IRDB->getNumberOfModules() == 1 &&
           "IncludeGlobals is currently only supported for WPA");
    const auto *GlobCtor = ICFG->buildCRuntimeGlobalCtorsDtorsModel(
        *IRDB->getWPAModule(), UserEntryPoints);
    FunctionWL.push_back(GlobCtor);
  } else {
    FunctionWL.insert(FunctionWL.end(), UserEntryPoints.begin(),
                      UserEntryPoints.end());
  }
}

auto LLVMBasedICFG::Builder::buildCallGraph(Soundness /*S*/) -> bidigraph_t {
  PHASAR_LOG_LEVEL(INFO, "Starting CallGraphAnalysisType: " << Res->str());
  VisitedFunctions.reserve(IRDB->getAllFunctions().size());

  bool FixpointReached;
  bidigraph_t Ret{};

  do {
    FixpointReached = true;
    while (!FunctionWL.empty()) {
      const llvm::Function *F = FunctionWL.pop_back_val();
      FixpointReached &= processFunction(Ret, F);
    }

    /// XXX This can probably be does more efficiently.
    /// However, we cannot just work on the IndirectCalls-delta as we are
    /// mutating the points-to-info on the fly
    for (auto [CS, _] : IndirectCalls) {
      FixpointReached &= !constructDynamicCall(Ret, CS);
    }

  } while (!FixpointReached);
  for (const auto &[IndirectCall, Targets] : IndirectCalls) {
    if (Targets == 0) {
      PHASAR_LOG_LEVEL(WARNING, "No callees found for callsite "
                                    << llvmIRToString(IndirectCall));
    }
  }
  REG_COUNTER("CG Vertices", boost::num_vertices(ret),
              PAMM_SEVERITY_LEVEL::Full);
  REG_COUNTER("CG Edges", boost::num_edges(ret), PAMM_SEVERITY_LEVEL::Full);
  PHASAR_LOG_LEVEL(INFO, "Call graph has been constructed");
  return Ret;
}

bool LLVMBasedICFG::Builder::processFunction(bidigraph_t &CallGraph,
                                             const llvm::Function *F) {
  PHASAR_LOG_LEVEL(DEBUG, "Walking in function: " << F->getName());
  if (F->isDeclaration() || !VisitedFunctions.insert(F).second) {
    PHASAR_LOG_LEVEL(DEBUG, "Function already visited or only declaration: "
                                << F->getName());
    return true;
  }

  assert(FunctionVertexMap != nullptr);
  assert(Res != nullptr);

  // add a node for function F to the call graph (if not present already)

  auto [VtxIt, VtxInserted] = FunctionVertexMap->try_emplace(F, vertex_t{});

  if (VtxInserted) {
    VtxIt->second = boost::add_vertex(VertexProperties(F), CallGraph);
  }

  vertex_t ThisFunctionVertexDescriptor = VtxIt->second;

  bool FixpointReached = true;

  // iterate all instructions of the current function
  Resolver::FunctionSetTy PossibleTargets;
  for (const auto &I : llvm::instructions(F)) {
    if (const auto *CS = llvm::dyn_cast<llvm::CallBase>(&I)) {
      Res->preCall(&I);

      // check if function call can be resolved statically
      if (CS->getCalledFunction() != nullptr) {
        PossibleTargets.insert(CS->getCalledFunction());
        PHASAR_LOG_LEVEL(DEBUG, "Found static call-site: "
                                    << "  " << llvmIRToString(CS));
      } else {
        // still try to resolve the called function statically
        const llvm::Value *SV = CS->getCalledOperand()->stripPointerCasts();
        const llvm::Function *ValueFunction =
            !SV->hasName() ? nullptr : IRDB->getFunction(SV->getName());
        if (ValueFunction) {
          PossibleTargets.insert(ValueFunction);
          PHASAR_LOG_LEVEL(DEBUG,
                           "Found static call-site: " << llvmIRToString(CS));
        } else {
          if (llvm::isa<llvm::InlineAsm>(SV)) {
            continue;
          }
          // the function call must be resolved dynamically
          PHASAR_LOG_LEVEL(DEBUG, "Found dynamic call-site: "
                                      << "  " << llvmIRToString(CS));
          IndirectCalls[CS] = 0;

          FixpointReached = false;
          continue;
        }
      }

      PHASAR_LOG_LEVEL(DEBUG, "Found " << PossibleTargets.size()
                                       << " possible target(s)");

      Res->handlePossibleTargets(CS, PossibleTargets);
      // Insert possible target inside the graph and add the link with
      // the current function
      for (const auto &PossibleTarget : PossibleTargets) {
        auto [TgtIt, TgtInserted] =
            FunctionVertexMap->try_emplace(PossibleTarget, vertex_t{});

        if (TgtInserted) {
          TgtIt->second =
              boost::add_vertex(VertexProperties(PossibleTarget), CallGraph);
        }

        vertex_t TargetVertex = TgtIt->second;

        boost::add_edge(ThisFunctionVertexDescriptor, TargetVertex,
                        EdgeProperties(CS), CallGraph);

        FunctionWL.push_back(PossibleTarget);
      }

      Res->postCall(&I);
    } else {
      Res->otherInst(&I);
    }
    PossibleTargets.clear();
  }

  return FixpointReached;
}

static bool internalIsVirtualFunctionCall(const llvm::Instruction *Inst,
                                          const LLVMTypeHierarchy &TH) {
  assert(Inst != nullptr);
  const auto *CallSite = llvm::dyn_cast<llvm::CallBase>(Inst);
  if (!CallSite) {
    return false;
  }
  // check potential receiver type
  const auto *RecType = getReceiverType(CallSite);
  if (!RecType) {
    return false;
  }
  if (!TH.hasType(RecType)) {
    return false;
  }
  if (!TH.hasVFTable(RecType)) {
    return false;
  }
  return getVFTIndex(CallSite) >= 0;
}

bool LLVMBasedICFG::Builder::constructDynamicCall(bidigraph_t &CallGraph,
                                                  const llvm::Instruction *CS) {
  bool NewTargetsFound = false;
  // Find vertex of calling function.
  vertex_t ThisFunctionVertexDescriptor;
  auto FvmItr = FunctionVertexMap->find(CS->getFunction());

  if (FvmItr == FunctionVertexMap->end()) {
    PHASAR_LOG_LEVEL(
        ERROR, "constructDynamicCall: Did not find vertex of calling function "
                   << CS->getFunction()->getName() << " at callsite "
                   << llvmIRToString(CS));
    std::terminate();
  }

  ThisFunctionVertexDescriptor = FvmItr->second;

  if (const auto *CallSite = llvm::dyn_cast<llvm::CallBase>(CS)) {
    Res->preCall(CallSite);

    // the function call must be resolved dynamically
    PHASAR_LOG_LEVEL(DEBUG, "Looking into dynamic call-site: ");
    PHASAR_LOG_LEVEL(DEBUG, "  " << llvmIRToString(CS));
    // call the resolve routine

    assert(TH != nullptr);
    auto PossibleTargets = internalIsVirtualFunctionCall(CallSite, *TH)
                               ? Res->resolveVirtualCall(CallSite)
                               : Res->resolveFunctionPointer(CallSite);

    assert(IndirectCalls.count(CallSite));

    auto &NumIndCalls = IndirectCalls[CallSite];

    if (NumIndCalls < PossibleTargets.size()) {
      PHASAR_LOG_LEVEL(DEBUG, "Found " << PossibleTargets.size() - NumIndCalls
                                       << " new possible target(s)");
      NumIndCalls = PossibleTargets.size();
      NewTargetsFound = true;
    }
    if (!NewTargetsFound) {
      return NewTargetsFound;
    }
    // Throw out already found targets
    for (const auto &OE : boost::make_iterator_range(
             boost::out_edges(ThisFunctionVertexDescriptor, CallGraph))) {
      if (CallGraph[OE].CS == CallSite) {
        PossibleTargets.erase(CallGraph[boost::target(OE, CallGraph)].F);
      }
    }
    Res->handlePossibleTargets(CallSite, PossibleTargets);
    // Insert possible target inside the graph and add the link with
    // the current function
    for (const auto &PossibleTarget : PossibleTargets) {
      auto [TgtIt, TgtInserted] =
          FunctionVertexMap->try_emplace(PossibleTarget, vertex_t{});

      if (TgtInserted) {
        TgtIt->second =
            boost::add_vertex(VertexProperties(PossibleTarget), CallGraph);
      }
      vertex_t TargetVertex = TgtIt->second;

      boost::add_edge(ThisFunctionVertexDescriptor, TargetVertex,
                      EdgeProperties(CallSite), CallGraph);
      FunctionWL.push_back(PossibleTarget);
    }

    Res->postCall(CallSite);
  } else {
    Res->otherInst(CS);
  }

  return NewTargetsFound;
}

LLVMBasedICFG::LLVMBasedICFG(ProjectIRDB *IRDB, CallGraphAnalysisType CGType,
                             llvm::ArrayRef<std::string> EntryPoints,
                             LLVMTypeHierarchy *TH, LLVMPointsToInfo *PT,
                             Soundness S, bool IncludeGlobals)
    : TH(TH) {
  assert(IRDB != nullptr);
  this->IRDB = IRDB;

  Builder B{IRDB, TH, PT};
  if (!TH && CGType != CallGraphAnalysisType::NORESOLVE) {
    this->TH = std::make_unique<LLVMTypeHierarchy>(*IRDB);
    B.TH = this->TH.get();
  }
  if (!PT && CGType == CallGraphAnalysisType::OTF) {
    B.PT = std::make_unique<LLVMPointsToSet>(*IRDB);
  }
  B.FunctionVertexMap = &this->FunctionVertexMap;

  B.Res = Resolver::create(CGType, IRDB, B.TH, this, B.PT.get());
  B.initEntryPoints(EntryPoints);
  B.initGlobalsAndWorkList(this, IncludeGlobals);

  CallGraph = B.buildCallGraph(S);
}

LLVMBasedICFG::~LLVMBasedICFG() = default;

[[nodiscard]] FunctionRange LLVMBasedICFG::getAllFunctionsImpl() const {
  /// With the new LLVMProjectIRDB, this will be easier...
  return llvm::map_range(
      static_cast<const llvm::Module &>(*IRDB->getWPAModule()),
      Ref2PointerConverter<llvm::Function>{});
}

[[nodiscard]] auto LLVMBasedICFG::getFunctionImpl(llvm::StringRef Fun) const
    -> f_t {
  return IRDB->getFunction(Fun);
}

[[nodiscard]] bool LLVMBasedICFG::isIndirectFunctionCallImpl(n_t Inst) const {
  const auto *CallSite = llvm::dyn_cast<llvm::CallBase>(Inst);
  return CallSite && CallSite->isIndirectCall();
}

[[nodiscard]] bool LLVMBasedICFG::isVirtualFunctionCallImpl(n_t Inst) const {
  return internalIsVirtualFunctionCall(Inst, *TH);
}

[[nodiscard]] auto LLVMBasedICFG::allNonCallStartNodesImpl() const
    -> std::vector<n_t> {
  std::vector<n_t> NonCallStartNodes;
  /// NOTE: Gets more performant once we have the new LLVMProjectIRDB
  NonCallStartNodes.reserve(2 * IRDB->getAllFunctions().size());
  for (const auto *F : IRDB->getAllFunctions()) {
    for (const auto &I : llvm::instructions(F)) {
      if (!llvm::isa<llvm::CallBase>(&I) && !isStartPoint(&I)) {
        NonCallStartNodes.push_back(&I);
      }
    }
  }
  return NonCallStartNodes;
}
[[nodiscard]] auto LLVMBasedICFG::getCalleesOfCallAtImpl(n_t Inst) const
    -> llvm::SmallVector<f_t> {
  if (!llvm::isa<llvm::CallBase>(Inst)) {
    return {};
  }

  auto MapEntry = FunctionVertexMap.find(Inst->getFunction());
  if (MapEntry == FunctionVertexMap.end()) {
    return {};
  }

  llvm::SmallVector<f_t> Callees;

  for (auto EdgeDesc : boost::make_iterator_range(
           boost::out_edges(MapEntry->second, CallGraph))) {
    auto Edge = CallGraph[EdgeDesc];
    if (Inst == Edge.CS) {
      auto Target = boost::target(EdgeDesc, CallGraph);
      Callees.push_back(CallGraph[Target].F);
    }
  }

  return Callees;
}
/// TODO: Return a map_iterator on the in_edge_iterator -- How to deal with
/// not-contaied funs? assert them out?
[[nodiscard]] auto LLVMBasedICFG::getCallersOfImpl(f_t Fun) const
    -> llvm::SmallVector<n_t> {
  llvm::SmallVector<n_t> CallersOf;
  auto MapEntry = FunctionVertexMap.find(Fun);
  if (MapEntry == FunctionVertexMap.end()) {
    return CallersOf;
  }

  for (auto EdgeDesc : boost::make_iterator_range(
           boost::in_edges(MapEntry->second, CallGraph))) {
    auto Edge = CallGraph[EdgeDesc];
    CallersOf.push_back(Edge.CS);
  }
  return CallersOf;
}

[[nodiscard]] auto LLVMBasedICFG::getCallsFromWithinImpl(f_t Fun) const
    -> llvm::SmallVector<n_t> {
  llvm::SmallVector<n_t> CallSites;
  for (const auto &I : llvm::instructions(Fun)) {
    if (llvm::isa<llvm::CallBase>(I)) {
      CallSites.push_back(&I);
    }
  }
  return CallSites;
}

[[nodiscard]] auto LLVMBasedICFG::getReturnSitesOfCallAtImpl(n_t Inst) const
    -> llvm::SmallVector<n_t, 2> {
  /// Currently, we don't distinguish normal-dest and unwind-dest, so we can
  /// just use getSuccsOf

  return getSuccsOf(Inst);
}

LLVMBasedICFG::VertexProperties::VertexProperties(
    const llvm::Function *F) noexcept
    : F(F) {}
[[nodiscard]] llvm::StringRef
LLVMBasedICFG::VertexProperties::getFunctionName() const noexcept {
  return F->getName();
}

LLVMBasedICFG::EdgeProperties::EdgeProperties(
    const llvm::Instruction *I) noexcept
    : CS(I) {}

std::string LLVMBasedICFG::EdgeProperties::getCallSiteAsString() const {
  return llvmIRToStableString(CS);
}

void LLVMBasedICFG::printImpl(llvm::raw_ostream &OS) const {
  OS << "digraph CallGraph{\n";
  scope_exit CloseBrace = [&OS] { OS << "}\n"; };

  for (auto Vtx : boost::make_iterator_range(boost::vertices(CallGraph))) {
    OS << Vtx << "[label=\"";
    OS.write_escaped(CallGraph[Vtx].getFunctionName());
    OS << "\"];\n";

    for (auto Succ :
         boost::make_iterator_range(boost::out_edges(Vtx, CallGraph))) {
      OS << Vtx << "->" << boost::target(Succ, CallGraph) << "[label=\"";
      OS.write_escaped(CallGraph[Succ].getCallSiteAsString());
      OS << "\"]\n;";
    }
    OS << '\n';
  }
}

[[nodiscard]] nlohmann::json LLVMBasedICFG::getAsJsonImpl() const {
  nlohmann::json J;

  // iterate all graph vertices
  for (auto Vtx : boost::make_iterator_range(boost::vertices(CallGraph))) {
    auto VtxFunName = CallGraph[Vtx].getFunctionName().str();
    J[PhasarConfig::JsonCallGraphID()][VtxFunName] = nlohmann::json::array();
    // iterate all out edges of vertex vi_v
    for (auto Succ :
         boost::make_iterator_range(boost::out_edges(Vtx, CallGraph))) {
      J[PhasarConfig::JsonCallGraphID()][VtxFunName].push_back(
          CallGraph[boost::target(Succ, CallGraph)].getFunctionName().str());
    }
  }
  return J;
}

auto LLVMBasedICFG::getAllVertexFunctions() const -> std::vector<f_t> {
  std::vector<f_t> Ret;
  Ret.reserve(FunctionVertexMap.size());
  for (auto [Fun, Vtx] : FunctionVertexMap) {
    Ret.push_back(Fun);
  }
  return Ret;
}

} // namespace psr
