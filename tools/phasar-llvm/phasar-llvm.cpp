/******************************************************************************
 * Copyright (c) 2017 Philipp Schubert.
 * All rights reserved. This program and the accompanying materials are made
 * available under the terms of LICENSE.txt.
 *
 * Contributors:
 *     Philipp Schubert and others
 *****************************************************************************/

#include "phasar/Config/Configuration.h"
#include "phasar/Controller/AnalysisController.h"
#include "phasar/PhasarLLVM/AnalysisStrategy/Strategies.h"
#include "phasar/PhasarLLVM/ControlFlow/Resolver/CallGraphAnalysisType.h"
#include "phasar/PhasarLLVM/Pointer/PointerAnalysisType.h"
#include "phasar/PhasarLLVM/Utils/DataFlowAnalysisType.h"
#include "phasar/Utils/IO.h"
#include "phasar/Utils/Logger.h"
#include "phasar/Utils/Soundness.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CommandLine.h"

#include "nlohmann/json.hpp"

#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

using namespace psr;

namespace cl = llvm::cl;

namespace {

cl::OptionCategory PsrCat("PhASAR");

#define PSR_OPTION_FLAG(NAME, CMDFLAG, DESC, ...)                              \
  cl::opt<bool> NAME(CMDFLAG, cl::desc(DESC), cl::cat(PsrCat), ##__VA_ARGS__)

#define PSR_SHORTLONG_OPTION_TYPE(NAME, TYPE, SHORTCMD, LONGCMD, DESC, ...)    \
  TYPE NAME(LONGCMD, cl::desc(DESC), cl::cat(PsrCat), ##__VA_ARGS__);          \
  cl::alias NAME##ShortAlias(SHORTCMD, cl::aliasopt(NAME),                     \
                             cl::desc(DESC " (alias for --" LONGCMD ")"),      \
                             cl::cat(PsrCat))

#define PSR_SHORTLONG_OPTION(NAME, TYPE, SHORTCMD, LONGCMD, DESC, ...)         \
  PSR_SHORTLONG_OPTION_TYPE(NAME, cl::opt<TYPE>, SHORTCMD, LONGCMD, DESC,      \
                            ##__VA_ARGS__)

// PSR_SHORTLONG_OPTION(ConfigOpt, std::string, "c", "config",
//                      "Path to the configuration file, options can be "
//                      "specified as 'parameter = option'");

PSR_SHORTLONG_OPTION(SilentOpt, bool, "s", "silent",
                     "Suppress any non-result output");
cl::alias QuietAlias("quiet", cl::aliasopt(SilentOpt),
                     cl::desc("Alias for --silent"), cl::cat(PsrCat));

PSR_SHORTLONG_OPTION_TYPE(ModuleOpt, cl::list<std::string>, "m", "module",
                          "Path to the LLVM IR module under analysis",
                          cl::OneOrMore);

PSR_SHORTLONG_OPTION_TYPE(
    EntryOpt, cl::list<std::string>, "E", "entry-points",
    "Set the entry point(s) to be used; use '__ALL__' to specify all available "
    "function definitions as entry points");

cl::list<DataFlowAnalysisType> DataFlowAnalysisOpt(
    "data-flow-analysis", cl::desc("Set the analyses to be run"),
    cl::values(
#define DATA_FLOW_ANALYSIS_TYPES(NAME, CMDFLAG, DESC)                          \
  clEnumValN(DataFlowAnalysisType::NAME, CMDFLAG, DESC),
#include "phasar/PhasarLLVM/Utils/DataFlowAnalysisType.def"
        clEnumValN(DataFlowAnalysisType::None, "none", "No-op")),
    cl::cat(PsrCat));
cl::alias DataFlowAnalysisAlias("D", cl::aliasopt(DataFlowAnalysisOpt),
                                cl::desc("Alias for --data-flow-analysis"),
                                cl::cat(PsrCat));

cl::opt<AnalysisStrategy>
    StrategyOpt("analysis-strategy", cl::desc("The analysis strategy"),
                cl::values(
#define ANALYSIS_STRATEGY_TYPES(NAME, CMDFLAG, DESC)                           \
  clEnumValN(AnalysisStrategy::NAME, CMDFLAG, DESC),
#include "phasar/PhasarLLVM/AnalysisStrategy/Strategies.def"
                    clEnumValN(AnalysisStrategy::None, "none", "none")),
                cl::init(AnalysisStrategy::WholeProgram), cl::cat(PsrCat),
                cl::Hidden);

cl::opt<std::string> AnalysisConfigOpt(
    "analysis-config",
    cl::desc("Set the analysis's configuration (if required)"),
    cl::cat(PsrCat));

cl::opt<PointerAnalysisType> PTATypeOpt(
    "pointer-analysis",
    cl::desc("Set the points-to analysis to be used (CFLSteens, "
             "CFLAnders).  CFLSteens is ~O(N) but inaccurate while "
             "CFLAnders O(N^3) but more accurate."),
    cl::values(
#define POINTER_ANALYSIS_TYPE(NAME, CMDFLAG, DESC)                             \
  clEnumValN(PointerAnalysisType::NAME, CMDFLAG, DESC),
#include "phasar/PhasarLLVM/Pointer/PointerAnalysisType.def"
        clEnumValN(PointerAnalysisType::Invalid, "invalid", "invalid")),
    cl::init(PointerAnalysisType::CFLAnders), cl::cat(PsrCat));
cl::alias PTATypeAlias("P", cl::aliasopt(PTATypeOpt),
                       cl::desc("Alias for --pointer-analysis"),
                       cl::cat(PsrCat));

cl::opt<CallGraphAnalysisType> CGTypeOpt(
    "call-graph-analysis", cl::desc("Set the call-graph algorithm to be used"),
    cl::values(
#define CALL_GRAPH_ANALYSIS_TYPE(NAME, CMDFLAG, DESC)                          \
  clEnumValN(CallGraphAnalysisType::NAME, CMDFLAG, DESC),
#include "phasar/PhasarLLVM/ControlFlow/Resolver/CallGraphAnalysisType.def"
        clEnumValN(CallGraphAnalysisType::Invalid, "invalid", "invalid")),
    cl::init(CallGraphAnalysisType::OTF), cl::cat(PsrCat));
cl::alias CGTypeAlias("C", cl::aliasopt(CGTypeOpt),
                      cl::desc("Alias for --call-graph-analysis"),
                      cl::cat(PsrCat));

cl::opt<Soundness>
    SoundnessOpt("soundness", cl::desc("Set the soundiness level to be used"),
                 cl::values(
#define SOUNDNESS_FLAG_TYPE(NAME, CMDFLAG, DESC)                               \
  clEnumValN(Soundness::NAME, CMDFLAG, DESC),
#include "phasar/Utils/Soundness.def"
                     clEnumValN(Soundness::Invalid, "invalid", "invalid")),
                 cl::init(Soundness::Soundy), cl::cat(PsrCat), cl::Hidden);
PSR_OPTION_FLAG(AutoGlobalsOpt, "auto-globals",
                "Enable automated support for global initializers",
                cl::init(true));

PSR_SHORTLONG_OPTION(
    StatisticsOpt, bool, "S", "statistical-analysis",
    "Collect and emit statistics of the module(s) under analysis");

#ifdef DYNAMIC_LOG
PSR_SHORTLONG_OPTION(LogOpt, bool, "L", "log", "Enable logging");
#endif

cl::opt<std::string>
    ExportOpt("export",
              cl::desc("Export mode (JSON, SARIF) (Not implemented yet!)"),
              cl::cat(PsrCat), cl::Hidden);

cl::opt<std::string> ProjectIdOpt("project-id",
                                  cl::desc("Project id used for output"),
                                  cl::init("default-phasar-project"),
                                  cl::cat(PsrCat), cl::Hidden);

PSR_SHORTLONG_OPTION(OutDirOpt, std::string, "O", "out",
                     "Output directory; if specified all results are written "
                     "to the output directory instead of stdout");

PSR_OPTION_FLAG(EmitIROpt, "emit-ir",
                "Emit preprocessed and annotated IR of analysis target");
PSR_OPTION_FLAG(EmitRawResultsOpt, "emit-raw-results",
                "Emit unprocessed/raw solver results");
PSR_OPTION_FLAG(EmitTextReportOpt, "emit-text-report",
                "Emit textual report of solver results", cl::init(true));
PSR_OPTION_FLAG(EmitGraphicalReportOpt, "emit-graphical-report",
                "Emit graphical report of solver results", cl::Hidden);
PSR_OPTION_FLAG(EmitESGAsDotOpt, "emit-esg-as-dot",
                "Emit the exploded super-graph (ESG) as DOT graph");
PSR_OPTION_FLAG(EmitTHAsTextOpt, "emit-th-as-text",
                "Emit the type hierarchy as text");
PSR_OPTION_FLAG(EmitTHAsDotOpt, "emit-th-as-dot",
                "Emit the type hierarchy as DOT graph");
PSR_OPTION_FLAG(EmitTHAsJsonOpt, "emit-th-as-json",
                "Emit the type hierarchy as JSON");
PSR_OPTION_FLAG(EmitCGAsTextOpt, "emit-cg-as-text",
                "Emit the call graph as text");
PSR_OPTION_FLAG(EmitCGAsDotOpt, "emit-cg-as-dot",
                "Emit the call graph as DOT graph");
PSR_OPTION_FLAG(EmitCGAsJsonOpt, "emit-cg-as-json",
                "Emit the call graph as json");
PSR_OPTION_FLAG(EmitPTAAsTextOpt, "emit-pta-as-text",
                "Emit the points-to information as text");
PSR_OPTION_FLAG(EmitPTAAsDotOpt, "emit-pta-as-dot",
                "Emit the points-to information as DOT graph");
PSR_OPTION_FLAG(EmitPTAAsJsonOpt, "emit-pta-as-json",
                "Emit the points-to information as json");
PSR_OPTION_FLAG(EmitStatsAsJsonOpt, "emit-statistics-as-json",
                "Emit the statistics information as json");
PSR_OPTION_FLAG(FollowReturnPastSeedsOpt, "follow-return-past-seeds",
                "Let the IFDS/IDE Solver process unbalanced returns",
                cl::init(true));
PSR_OPTION_FLAG(AutoAddZeroOpt, "auto-add-zero",
                "Let the IFDS/IDE Solver automatically add the special zero "
                "value to any set of dataflow-facts",
                cl::init(true));
PSR_OPTION_FLAG(
    ComputeValuesOpt, "compute-values",
    "Let the IDE Solver compute the values attached to each edge in the ESG",
    cl::init(true));
PSR_OPTION_FLAG(
    RecordEdgesOpt, "record-edges",
    "Let the IFDS/IDE Solver record all ESG edges whole solving the dataflow "
    "problem. This can have massive performance impact",
    cl::Hidden);
PSR_OPTION_FLAG(PersistedSummariesOpt, "persisted-summaries",
                "Let the IFDS/IDE Solver compute persisted procedure summaries "
                "(Currently not supported)",
                cl::Hidden);

cl::opt<std::string>
    LoadPTAFromJsonOpt("load-pta-from-json",
                       cl::desc("Load the points-to info previously exported "
                                "via emit-pta-as-json from the given file"),
                       cl::cat(PsrCat));

PSR_SHORTLONG_OPTION(PammOutOpt, std::string, "A", "pamm-out",
                     "Filename for PAMM's gathered data",
                     cl::init("PAMM_data.json"), cl::cat(PsrCat), cl::Hidden);

// void validateParamConfigFile(const std::string &Config) {
//   if (!(std::filesystem::exists(Config) &&
//         !std::filesystem::is_directory(Config))) {
//     llvm::errs() << "PhASAR configuration '" << Config << "' does not
//     exist!\n"; exit(1);
//   }
// }

void validateParamModule() {
  if (ModuleOpt.empty()) {
    llvm::errs() << "At least one LLVM target module is required!\n";
    exit(1);
  }
  for (const auto &Module : ModuleOpt) {
    std::filesystem::path ModulePath(Module);
    if (!(std::filesystem::exists(ModulePath) &&
          !std::filesystem::is_directory(ModulePath) &&
          (ModulePath.extension() == ".ll" ||
           ModulePath.extension() == ".bc"))) {
      llvm::errs() << "LLVM module '" << Module << "' does not exist!\n";
      exit(1);
    }
  }
}

void validateParamOutput() {
  if (!OutDirOpt.empty() &&
      !std::filesystem::is_directory(OutDirOpt.getValue())) {
    llvm::errs() << '\'' << OutDirOpt
                 << "' does not exist, a valid output directory is required!\n";
    exit(1);
  }
}

void validateParamPointerAnalysis() {
  if (PTATypeOpt == PointerAnalysisType::Invalid) {
    llvm::errs() << "'Invalid' is not a valid pointer analysis!\n";
    exit(1);
  }
}

void validateParamCallGraphAnalysis() {
  if (CGTypeOpt == CallGraphAnalysisType::Invalid) {
    llvm::errs() << "'Invalid' is not a valid call-graph analysis!\n";
    exit(1);
  }
}

void validateSoundnessFlag() {
  if (SoundnessOpt == Soundness::Invalid) {
    llvm::errs() << "'Invalid' is not a valid soundness level!\n";
    exit(1);
  }
}

void validateParamAnalysisConfig() {
  if (!AnalysisConfigOpt.empty() &&
      !(std::filesystem::exists(AnalysisConfigOpt.getValue()) &&
        !std::filesystem::is_directory(AnalysisConfigOpt.getValue()))) {
    llvm::errs() << "Analysis configuration '" << AnalysisConfigOpt
                 << "' does not exist!\n";
    exit(1);
  }
}

void validatePTAJsonFile() {
  if (!LoadPTAFromJsonOpt.empty() &&
      !(std::filesystem::exists(LoadPTAFromJsonOpt.getValue()) &&
        !std::filesystem::is_directory(LoadPTAFromJsonOpt.getValue()))) {
    llvm::errs() << "Points-to info file '" << LoadPTAFromJsonOpt
                 << "' does not exist!\n";
    exit(1);
  }
}

void validateLogLevel(const std::string &Level) {
  if (parseSeverityLevel(Level) == SeverityLevel::INVALID) {
    llvm::errs() << "Invalid logger severity level '" << Level
                 << "'. Expect DEBUG, INFO, WARNING or ERROR\n";
    exit(1);
  }
}

} // anonymous namespace

int main(int Argc, const char **Argv) {
  cl::SetVersionPrinter([](llvm::raw_ostream &OS) {
    OS << "PhASAR " << PhasarConfig::PhasarVersion() << '\n';
  });
  cl::HideUnrelatedOptions(PsrCat);
  cl::ParseCommandLineOptions(Argc, Argv);

#ifdef DYNAMIC_LOG
  if (LogOpt) {
    Logger::initializeStderrLogger(DEBUG);
  }
#endif

  // Vanity header
  if (!SilentOpt) {
    llvm::outs() << "PhASAR " << PhasarConfig::PhasarVersion()
                 << "\nA LLVM-based static analysis framework\n\n";
  }

  if (StrategyOpt == AnalysisStrategy::None) {
    llvm::errs() << "Invalid analysis strategy!\n";
    return 1;
  }

  validateParamModule();
  validateParamOutput();
  validateParamPointerAnalysis();
  validateParamCallGraphAnalysis();
  validateSoundnessFlag();
  validateParamAnalysisConfig();
  validatePTAJsonFile();

  // setup IRDB as source code manager
  ProjectIRDB IRDB(std::vector(ModuleOpt.begin(), ModuleOpt.end()));
  if (StatisticsOpt) {
    llvm::outs() << "Module " << IRDB.getWPAModule()->getName() << ":\n";
    llvm::outs() << "> LLVM IR instructions:\t" << IRDB.getNumInstructions()
                 << "\n";
    llvm::outs() << "> Functions:\t\t" << IRDB.getWPAModule()->size() << "\n";
    llvm::outs() << "> Global variables:\t"
                 << IRDB.getWPAModule()->global_size() << "\n";
    llvm::outs() << "> Alloca instructions:\t"
                 << IRDB.getAllocaInstructions().size() << "\n";
    llvm::outs() << "> Memory Locations:\t"
                 << IRDB.getAllMemoryLocations().size() << "\n";
    llvm::outs() << "> Call Sites:\t\t" << IRDB.getNumCallsites() << "\n";
  }

  // setup the emitter options to display the computed analysis results
  auto EmitterOptions = AnalysisControllerEmitterOptions::None;

  IFDSIDESolverConfig SolverConfig{};
  if (EmitIROpt) {
    EmitterOptions |= AnalysisControllerEmitterOptions::EmitIR;
  }
  if (EmitRawResultsOpt) {
    EmitterOptions |= AnalysisControllerEmitterOptions::EmitRawResults;
  }
  if (EmitTextReportOpt) {
    EmitterOptions |= AnalysisControllerEmitterOptions::EmitTextReport;
  }
  if (EmitGraphicalReportOpt) {
    EmitterOptions |= AnalysisControllerEmitterOptions::EmitGraphicalReport;
  }
  if (EmitESGAsDotOpt) {
    EmitterOptions |= AnalysisControllerEmitterOptions::EmitESGAsDot;
    SolverConfig.setEmitESG();
  }
  if (EmitTHAsTextOpt) {
    EmitterOptions |= AnalysisControllerEmitterOptions::EmitTHAsText;
  }
  if (EmitTHAsDotOpt) {
    EmitterOptions |= AnalysisControllerEmitterOptions::EmitTHAsDot;
  }
  if (EmitTHAsJsonOpt) {
    EmitterOptions |= AnalysisControllerEmitterOptions::EmitTHAsJson;
  }
  // if (EmitCGAsTextOpt) {
  //   EmitterOptions |= AnalysisControllerEmitterOptions::EmitCGAsText;
  // }
  if (EmitCGAsDotOpt) {
    EmitterOptions |= AnalysisControllerEmitterOptions::EmitCGAsDot;
  }
  // if (EmitCGAsJsonOpt) {
  //   EmitterOptions |= AnalysisControllerEmitterOptions::EmitCGAsJson;
  // }
  if (EmitPTAAsTextOpt) {
    EmitterOptions |= AnalysisControllerEmitterOptions::EmitPTAAsText;
  }
  if (EmitPTAAsDotOpt) {
    EmitterOptions |= AnalysisControllerEmitterOptions::EmitPTAAsDot;
  }
  if (EmitPTAAsJsonOpt) {
    EmitterOptions |= AnalysisControllerEmitterOptions::EmitPTAAsJson;
  }
  if (EmitStatsAsJsonOpt) {
    EmitterOptions |= AnalysisControllerEmitterOptions::EmitStatisticsAsJson;
  }
  SolverConfig.setFollowReturnsPastSeeds(FollowReturnPastSeedsOpt);
  SolverConfig.setAutoAddZero(AutoAddZeroOpt);
  SolverConfig.setComputeValues(ComputeValuesOpt);
  SolverConfig.setRecordEdges(RecordEdgesOpt || EmitESGAsDotOpt);
  SolverConfig.setComputePersistedSummaries(PersistedSummariesOpt);

  nlohmann::json PrecomputedPointsToSet;
  if (!LoadPTAFromJsonOpt.empty()) {
    PrecomputedPointsToSet = readJsonFile(llvm::StringRef(LoadPTAFromJsonOpt));
  }

  if (EntryOpt.empty()) {
    EntryOpt.push_back("main");
  }

  AnalysisController Controller(
      IRDB, std::vector(DataFlowAnalysisOpt.begin(), DataFlowAnalysisOpt.end()),
      {AnalysisConfigOpt.getValue()}, PTATypeOpt, CGTypeOpt, SoundnessOpt,
      AutoGlobalsOpt, std::vector(EntryOpt.begin(), EntryOpt.end()),
      StrategyOpt, EmitterOptions, SolverConfig, ProjectIdOpt, OutDirOpt,
      PrecomputedPointsToSet);
  return 0;
}
