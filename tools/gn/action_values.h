// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef TOOLS_GN_ACTION_VALUES_H_
#define TOOLS_GN_ACTION_VALUES_H_

#include <string>
#include <vector>

#include "base/macros.h"
#include "tools/gn/source_file.h"
#include "tools/gn/substitution_pattern.h"
#include "tools/gn/substitution_list.h"

class Target;

// Holds the values (outputs, args, script name, etc.) for either an action or
// an action_foreach target.
class ActionValues {
 public:
  ActionValues();
  ~ActionValues();

  // Filename of the script to execute.
  const SubstitutionPattern& command() const { return command_; }
  void set_command(const SubstitutionPattern & cmd) { command_ = cmd; }

  // Filename of the script to execute.
  const SourceFile& script() const { return script_; }
  void set_script(const SourceFile& s) { script_ = s; }

  // Arguments to the script.
  SubstitutionList& args() { return args_; }
  const SubstitutionList& args() const { return args_; }

  // Files created by the script. These are strings rather than SourceFiles
  // since they will often contain {{source expansions}}.
  SubstitutionList& outputs() { return outputs_; }
  const SubstitutionList& outputs() const { return outputs_; }

  // Expands the outputs() above to the final SourceFile list.
  void GetOutputsAsSourceFiles(const Target* target,
                               std::vector<SourceFile>* result) const;

  // Depfile generated by the script.
  const SubstitutionPattern& depfile() const { return depfile_; }
  bool has_depfile() const { return !depfile_.ranges().empty(); }
  void set_depfile(const SubstitutionPattern& depfile) { depfile_ = depfile; }

  // Response file contents. Empty means no response file.
  SubstitutionList& rsp_file_contents() { return rsp_file_contents_; }
  const SubstitutionList& rsp_file_contents() const {
    return rsp_file_contents_;
  }
  bool uses_rsp_file() const { return !rsp_file_contents_.list().empty(); }

  // Console pool option
  bool is_console() const { return console_; }
  void set_console(bool value) { console_ = value; }

  // Description option
  bool has_description() const { return !description_.empty(); }
  const SubstitutionPattern & description() const { return description_; }
  void set_description(const SubstitutionPattern & desc) { description_ = desc; }

 private:
  SubstitutionPattern command_;
  SubstitutionPattern description_;
  SourceFile script_;
  SubstitutionList args_;
  SubstitutionList outputs_;
  SubstitutionPattern depfile_;
  SubstitutionList rsp_file_contents_;
  bool console_;

  DISALLOW_COPY_AND_ASSIGN(ActionValues);
};

#endif  // TOOLS_GN_ACTION_VALUES_H_
