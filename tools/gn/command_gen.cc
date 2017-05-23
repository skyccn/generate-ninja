// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/atomicops.h"
#include "base/bind.h"
#include "base/command_line.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/timer/elapsed_timer.h"
#include "tools/gn/build_settings.h"
#include "tools/gn/commands.h"
#include "tools/gn/eclipse_writer.h"
#include "tools/gn/json_project_writer.h"
#include "tools/gn/ninja_target_writer.h"
#include "tools/gn/ninja_writer.h"
#include "tools/gn/qt_creator_writer.h"
#include "tools/gn/runtime_deps.h"
#include "tools/gn/scheduler.h"
#include "tools/gn/setup.h"
#include "tools/gn/standard_out.h"
#include "tools/gn/switches.h"
#include "tools/gn/target.h"
#include "tools/gn/visual_studio_writer.h"
#include "tools/gn/xcode_writer.h"

namespace commands {

namespace {

const char kSwitchCheck[] = "check";
const char kSwitchFilters[] = "filters";
const char kSwitchIde[] = "ide";
const char kSwitchIdeValueEclipse[] = "eclipse";
const char kSwitchIdeValueQtCreator[] = "qtcreator";
const char kSwitchIdeValueVs[] = "vs";
const char kSwitchIdeValueVs2013[] = "vs2013";
const char kSwitchIdeValueVs2015[] = "vs2015";
const char kSwitchIdeValueVs2017[] = "vs2017";
const char kSwitchIdeValueXcode[] = "xcode";
const char kSwitchIdeValueJson[] = "json";
const char kSwitchNinjaExtraArgs[] = "ninja-extra-args";
const char kSwitchNoDeps[] = "no-deps";
const char kSwitchRootTarget[] = "root-target";
const char kSwitchSln[] = "sln";
const char kSwitchWorkspace[] = "workspace";
const char kSwitchJsonFileName[] = "json-file-name";
const char kSwitchJsonIdeScript[] = "json-ide-script";
const char kSwitchJsonIdeScriptArgs[] = "json-ide-script-args";

// Collects Ninja rules for each toolchain. The lock protectes the rules.
struct TargetWriteInfo {
  base::Lock lock;
  NinjaWriter::PerToolchainRules rules;
};

// Called on worker thread to write the ninja file.
void BackgroundDoWrite(TargetWriteInfo* write_info, const Target* target) {
  std::string rule = NinjaTargetWriter::RunAndWriteFile(target);
  DCHECK(!rule.empty());

  {
    base::AutoLock lock(write_info->lock);
    write_info->rules[target->toolchain()].emplace_back(
        target, std::move(rule));
  }
}

// Called on the main thread.
void ItemResolvedAndGeneratedCallback(TargetWriteInfo* write_info,
                                      const BuilderRecord* record) {
  const Item* item = record->item();
  const Target* target = item->AsTarget();
  if (target) {
    g_scheduler->ScheduleWork(base::Bind(&BackgroundDoWrite,
                                         write_info, target));
  }
}

// Returns a pointer to the target with the given file as an output, or null
// if no targets generate the file. This is brute force since this is an
// error condition and performance shouldn't matter.
const Target* FindTargetThatGeneratesFile(const Builder& builder,
                                          const SourceFile& file) {
  std::vector<const Target*> targets = builder.GetAllResolvedTargets();
  if (targets.empty())
    return nullptr;

  OutputFile output_file(targets[0]->settings()->build_settings(), file);
  for (const Target* target : targets) {
    for (const auto& cur_output : target->computed_outputs()) {
      if (cur_output == output_file)
        return target;
    }
  }
  return nullptr;
}

// Prints an error that the given file was present as a source or input in
// the given target(s) but was not generated by any of its dependencies.
void PrintInvalidGeneratedInput(const Builder& builder,
                                const SourceFile& file,
                                const std::vector<const Target*>& targets) {
  std::string err;

  // Only show the toolchain labels (which can be confusing) if something
  // isn't the default.
  bool show_toolchains = false;
  const Label& default_toolchain =
      targets[0]->settings()->default_toolchain_label();
  for (const Target* target : targets) {
    if (target->settings()->toolchain_label() != default_toolchain) {
      show_toolchains = true;
      break;
    }
  }

  const Target* generator = FindTargetThatGeneratesFile(builder, file);
  if (generator &&
      generator->settings()->toolchain_label() != default_toolchain)
    show_toolchains = true;

  const std::string target_str = targets.size() > 1 ? "targets" : "target";
  err += "The file:\n";
  err += "  " + file.value() + "\n";
  err += "is listed as an input or source for the " + target_str + ":\n";
  for (const Target* target : targets)
    err += "  " + target->label().GetUserVisibleName(show_toolchains) + "\n";

  if (generator) {
    err += "but this file was not generated by any dependencies of the " +
           target_str + ". The target\nthat generates the file is:\n  ";
    err += generator->label().GetUserVisibleName(show_toolchains);
  } else {
    err += "but no targets in the build generate that file.";
  }

  Err(Location(), "Input to " + target_str + " not generated by a dependency.",
      err).PrintToStdout();
}

bool CheckForInvalidGeneratedInputs(Setup* setup) {
  std::multimap<SourceFile, const Target*> unknown_inputs =
      g_scheduler->GetUnknownGeneratedInputs();
  if (unknown_inputs.empty())
    return true;  // No bad files.

  int errors_found = 0;
  auto cur = unknown_inputs.begin();
  while (cur != unknown_inputs.end()) {
    errors_found++;
    auto end_of_range = unknown_inputs.upper_bound(cur->first);

    // Package the values more conveniently for printing.
    SourceFile bad_input = cur->first;
    std::vector<const Target*> targets;
    while (cur != end_of_range)
      targets.push_back((cur++)->second);

    PrintInvalidGeneratedInput(setup->builder(), bad_input, targets);
    OutputString("\n");
  }

  OutputString(
      "If you have generated inputs, there needs to be a dependency path "
      "between the\ntwo targets in addition to just listing the files. For "
      "indirect dependencies,\nthe intermediate ones must be public_deps. "
      "data_deps don't count since they're\nonly runtime dependencies. If "
      "you think a dependency chain exists, it might be\nbecause the chain "
      "is private. Try \"gn path\" to analyze.\n");

  if (errors_found > 1) {
    OutputString(base::StringPrintf("\n%d generated input errors found.\n",
                                    errors_found), DECORATION_YELLOW);
  }
  return false;
}

bool RunIdeWriter(const std::string& ide,
                  const BuildSettings* build_settings,
                  const Builder& builder,
                  Err* err) {
  const base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();
  bool quiet = command_line->HasSwitch(switches::kQuiet);
  base::ElapsedTimer timer;

  if (ide == kSwitchIdeValueEclipse) {
    bool res = EclipseWriter::RunAndWriteFile(build_settings, builder, err);
    if (res && !quiet) {
      OutputString("Generating Eclipse settings took " +
                   base::Int64ToString(timer.Elapsed().InMilliseconds()) +
                   "ms\n");
    }
    return res;
  } else if (ide == kSwitchIdeValueVs || ide == kSwitchIdeValueVs2013 ||
             ide == kSwitchIdeValueVs2015 || ide == kSwitchIdeValueVs2017) {
    VisualStudioWriter::Version version = VisualStudioWriter::Version::Vs2015;
    if (ide == kSwitchIdeValueVs2013)
      version = VisualStudioWriter::Version::Vs2013;
    else if (ide == kSwitchIdeValueVs2017)
      version = VisualStudioWriter::Version::Vs2017;

    std::string sln_name;
    if (command_line->HasSwitch(kSwitchSln))
      sln_name = command_line->GetSwitchValueASCII(kSwitchSln);
    std::string filters;
    if (command_line->HasSwitch(kSwitchFilters))
      filters = command_line->GetSwitchValueASCII(kSwitchFilters);
    bool no_deps = command_line->HasSwitch(kSwitchNoDeps);
    bool res = VisualStudioWriter::RunAndWriteFiles(
        build_settings, builder, version, sln_name, filters, no_deps, err);
    if (res && !quiet) {
      OutputString("Generating Visual Studio projects took " +
                   base::Int64ToString(timer.Elapsed().InMilliseconds()) +
                   "ms\n");
    }
    return res;
  } else if (ide == kSwitchIdeValueXcode) {
    bool res = XcodeWriter::RunAndWriteFiles(
        command_line->GetSwitchValueASCII(kSwitchWorkspace),
        command_line->GetSwitchValueASCII(kSwitchRootTarget),
        command_line->GetSwitchValueASCII(kSwitchNinjaExtraArgs),
        command_line->GetSwitchValueASCII(kSwitchFilters), build_settings,
        builder, err);
    if (res && !quiet) {
      OutputString("Generating Xcode projects took " +
                   base::Int64ToString(timer.Elapsed().InMilliseconds()) +
                   "ms\n");
    }
    return res;
  } else if (ide == kSwitchIdeValueQtCreator) {
    std::string root_target;
    if (command_line->HasSwitch(kSwitchRootTarget))
      root_target = command_line->GetSwitchValueASCII(kSwitchRootTarget);
    bool res = QtCreatorWriter::RunAndWriteFile(
        build_settings, builder, err,
        root_target, command_line->HasSwitch(switches::kAllToolchains));
    if (res && !quiet) {
      OutputString("Generating QtCreator projects took " +
                   base::Int64ToString(timer.Elapsed().InMilliseconds()) +
                   "ms\n");
    }
    return res;
  } else if (ide == kSwitchIdeValueJson) {
    std::string file_name =
        command_line->GetSwitchValueASCII(kSwitchJsonFileName);
    if (file_name.empty())
      file_name = "project.json";
    std::string exec_script =
        command_line->GetSwitchValueASCII(kSwitchJsonIdeScript);
    std::string exec_script_extra_args =
        command_line->GetSwitchValueASCII(kSwitchJsonIdeScriptArgs);
    std::string filters = command_line->GetSwitchValueASCII(kSwitchFilters);

    bool res = JSONProjectWriter::RunAndWriteFiles(
        build_settings, builder, file_name, exec_script, exec_script_extra_args,
        filters, quiet, err);
    if (res && !quiet) {
      OutputString("Generating JSON projects took " +
                   base::Int64ToString(timer.Elapsed().InMilliseconds()) +
                   "ms\n");
    }
    return res;
  }

  *err = Err(Location(), "Unknown IDE: " + ide);
  return false;
}

}  // namespace

const char kGen[] = "gen";
const char kGen_HelpShort[] = "gen: Generate ninja files.";
const char kGen_Help[] =
    R"(gn gen: Generate ninja files.

  gn gen [--check] [<ide options>] <out_dir>

  Generates ninja files from the current tree and puts them in the given output
  directory.

  The output directory can be a source-repo-absolute path name such as:
      //out/foo
  Or it can be a directory relative to the current directory such as:
      out/foo

  "gn gen --check" is the same as running "gn check". See "gn help check"
  for documentation on that mode.

  See "gn help switches" for the common command-line switches.

IDE options

  GN optionally generates files for IDE. Possibilities for <ide options>

  --ide=<ide_name>
      Generate files for an IDE. Currently supported values:
      "eclipse" - Eclipse CDT settings file.
      "vs" - Visual Studio project/solution files.
             (default Visual Studio version: 2015)
      "vs2013" - Visual Studio 2013 project/solution files.
      "vs2015" - Visual Studio 2015 project/solution files.
      "vs2017" - Visual Studio 2017 project/solution files.
      "xcode" - Xcode workspace/solution files.
      "qtcreator" - QtCreator project files.
      "json" - JSON file containing target information

  --filters=<path_prefixes>
      Semicolon-separated list of label patterns used to limit the set of
      generated projects (see "gn help label_pattern"). Only matching targets
      and their dependencies will be included in the solution. Only used for
      Visual Studio, Xcode and JSON.

Visual Studio Flags

  --sln=<file_name>
      Override default sln file name ("all"). Solution file is written to the
      root build directory.

  --no-deps
      Don't include targets dependencies to the solution. Changes the way how
      --filters option works. Only directly matching targets are included.

Xcode Flags

  --workspace=<file_name>
      Override defaut workspace file name ("all"). The workspace file is
      written to the root build directory.

  --ninja-extra-args=<string>
      This string is passed without any quoting to the ninja invocation
      command-line. Can be used to configure ninja flags, like "-j" if using
      goma for example.

  --root-target=<target_name>
      Name of the target corresponding to "All" target in Xcode. If unset,
      "All" invokes ninja without any target and builds everything.

QtCreator Flags

  --all-toolchains
      Generates a QtCreator project that contains files and dependencies
      for all toolchains. If unset, only files and dependencies for the
      default toolchain will be used.

  --root-target=<target_name>
      Name of the root target for which the QtCreator project will be generated
      to contain files of it and its dependencies. If unset, the whole build
      graph will be emitted.


Eclipse IDE Support

  GN DOES NOT generate Eclipse CDT projects. Instead, it generates a settings
  file which can be imported into an Eclipse CDT project. The XML file contains
  a list of include paths and defines. Because GN does not generate a full
  .cproject definition, it is not possible to properly define includes/defines
  for each file individually. Instead, one set of includes/defines is generated
  for the entire project. This works fairly well but may still result in a few
  indexer issues here and there.

Generic JSON Output

  Dumps target information to a JSON file and optionally invokes a
  python script on the generated file. See the comments at the beginning
  of json_project_writer.cc and desc_builder.cc for an overview of the JSON
  file format.

  --json-file-name=<json_file_name>
      Overrides default file name (project.json) of generated JSON file.

  --json-ide-script=<path_to_python_script>
      Executes python script after the JSON file is generated. Path can be
      project absolute (//), system absolute (/) or relative, in which case the
      output directory will be base. Path to generated JSON file will be first
      argument when invoking script.

  --json-ide-script-args=<argument>
      Optional second argument that will passed to executed script.
)";

int RunGen(const std::vector<std::string>& args) {
  base::ElapsedTimer timer;

  if (args.size() != 1) {
    Err(Location(), "Need exactly one build directory to generate.",
        "I expected something more like \"gn gen out/foo\"\n"
        "You can also see \"gn help gen\".").PrintToStdout();
    return 1;
  }

  // Deliberately leaked to avoid expensive process teardown.
  Setup* setup = new Setup();
  if (!setup->DoSetup(args[0], true))
    return 1;

  const base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();
  if (command_line->HasSwitch(kSwitchCheck))
    setup->set_check_public_headers(true);

  // Cause the load to also generate the ninja files for each target.
  TargetWriteInfo write_info;
  setup->builder().set_resolved_and_generated_callback(
      base::Bind(&ItemResolvedAndGeneratedCallback, &write_info));

  // Do the actual load. This will also write out the target ninja files.
  if (!setup->Run())
    return 1;

  // Sort the targets in each toolchain according to their label. This makes
  // the ninja files have deterministic content.
  for (auto& cur_toolchain : write_info.rules) {
    std::sort(cur_toolchain.second.begin(), cur_toolchain.second.end(),
              [](const NinjaWriter::TargetRulePair& a,
                 const NinjaWriter::TargetRulePair& b) {
                return a.first->label() < b.first->label();
              });
  }

  Err err;
  // Write the root ninja files.
  if (!NinjaWriter::RunAndWriteFiles(&setup->build_settings(),
                                     setup->builder(),
                                     write_info.rules,
                                     &err)) {
    err.PrintToStdout();
    return 1;
  }

  if (!WriteRuntimeDepsFilesIfNecessary(setup->builder(), &err)) {
    err.PrintToStdout();
    return 1;
  }

  if (!CheckForInvalidGeneratedInputs(setup))
    return 1;

  if (command_line->HasSwitch(kSwitchIde) &&
      !RunIdeWriter(command_line->GetSwitchValueASCII(kSwitchIde),
                    &setup->build_settings(), setup->builder(), &err)) {
    err.PrintToStdout();
    return 1;
  }

  base::TimeDelta elapsed_time = timer.Elapsed();

  if (!command_line->HasSwitch(switches::kQuiet)) {
    OutputString("Done. ", DECORATION_GREEN);

    size_t targets_collected = 0;
    for (const auto& rules : write_info.rules)
      targets_collected += rules.second.size();

    std::string stats = "Made " + base::SizeTToString(targets_collected) +
        " targets from " +
        base::IntToString(
            setup->scheduler().input_file_manager()->GetInputFileCount()) +
        " files in " +
        base::Int64ToString(elapsed_time.InMilliseconds()) + "ms\n";
    OutputString(stats);
  }

  return 0;
}

}  // namespace commands
