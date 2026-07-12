/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SRC_TRACE_PROCESSOR_SHELL_REPORT_SUBCOMMAND_H_
#define SRC_TRACE_PROCESSOR_SHELL_REPORT_SUBCOMMAND_H_

#include <string>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/shell/subcommand.h"

namespace perfetto::trace_processor::shell {

// The resolved noun/view/trace_file from a `report` invocation's positionals.
struct ParsedReportArgs {
  std::string noun;
  std::string view;
  std::string trace_file;  // empty in --remote mode
};

// Parses `report [noun [view]] <trace_file>` from |positional|. In local mode
// (|remote| false) the trace file is the last positional; in --remote mode all
// positionals are noun/view. Validates the noun/view against the closed set and
// resolves the default view. Returns an error with the valid options on a bad
// noun/view or a missing trace file.
base::StatusOr<ParsedReportArgs> ParseReportArgs(
    const std::vector<std::string>& positional,
    bool remote);

// `report`: opinionated, zero-config built-in trace summaries. Emits a stream
// of self-delimited packets in one of three encodings (human text, JSONL,
// length-delimited binary).
class ReportSubcommand : public Subcommand {
 public:
  const char* name() const override;
  const char* description() const override;
  const char* usage_args() const override;
  const char* detailed_help() const override;
  std::vector<FlagSpec> GetFlags() override;
  base::Status Run(const SubcommandContext& ctx) override;

 private:
  // The body of Run(); on error under --format json/jsonl, Run() turns its
  // status into a structured JSON error object instead of a text message.
  base::Status RunInner(const SubcommandContext& ctx);

  std::string format_;
  std::string top_;
  std::string name_glob_;
};

}  // namespace perfetto::trace_processor::shell

#endif  // SRC_TRACE_PROCESSOR_SHELL_REPORT_SUBCOMMAND_H_
