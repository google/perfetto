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

#include "src/trace_processor/shell/agent_mode.h"

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/shell/result_formatter.h"
#include "src/trace_processor/shell/shell_utils.h"
#include "src/trace_processor/util/simple_json_serializer.h"

namespace perfetto::trace_processor::shell {
namespace {

// Environment variables which coding agents set for the commands they run.
// There is no single convention yet: this is the list used by DuckDB's agent
// mode (https://github.com/duckdb/duckdb/pull/26167). AI_AGENT and AGENT are
// generic markers (set by e.g. Claude Code, Goose and Amp), the others are
// specific to one agent.
constexpr const char* kAgentEnvVars[] = {
    "AI_AGENT",
    "AGENT",
    "CLAUDECODE",
    "CODEX_CI",
    "CODEX_SANDBOX",
    "CODEX_THREAD_ID",
    "CURSOR_AGENT",
    "GEMINI_CLI",
    "COPILOT_AGENT",
    "COPILOT_CLI",
    "COPILOT_AGENT_SESSION_ID",
};

std::optional<std::string> DetectAgentEnvVar() {
  for (const char* var : kAgentEnvVars) {
    const char* value = getenv(var);
    if (value && *value) {
      return var;
    }
  }
  return std::nullopt;
}

// Splits an error message into the SQL traceback (if any) and the error
// itself. Tracebacks are laid out like rustc diagnostics (see
// SqlSource::AsTraceback): frames each starting with a `--> ` line, then a
// final line starting with "error: ".
struct SplitError {
  std::string_view traceback;
  std::string_view error;
  std::optional<std::string_view> location;
};
SplitError SplitErrorMessage(std::string_view message) {
  constexpr std::string_view kErrorPrefix = "error: ";
  SplitError res;
  res.error = message;
  size_t pos = message.rfind("\nerror: ");
  if (pos == std::string_view::npos) {
    return res;
  }
  res.traceback = message.substr(0, pos + 1);
  res.error = message.substr(pos + 1 + kErrorPrefix.size());

  // The first frame is the SQL the user wrote.
  constexpr std::string_view kArrow = "--> ";
  size_t arrow = res.traceback.find(kArrow);
  if (arrow != std::string_view::npos) {
    size_t start = arrow + kArrow.size();
    size_t end = res.traceback.find('\n', start);
    res.location = res.traceback.substr(start, end - start);
  }
  return res;
}

// A short next step for the errors agents hit most often.
std::optional<std::string> HintForError(std::string_view error) {
  std::string e(error);
  if (base::Contains(e, "no such table:")) {
    return "list tables and views with `SELECT name FROM perfetto_tables`. "
           "Standard library tables need `INCLUDE PERFETTO MODULE` first: "
           "`trace_processor help agent` shows how to search for them";
  }
  if (base::Contains(e, "no such column:")) {
    return "`SELECT * FROM <table> LIMIT 0` prints a table's columns";
  }
  if (base::Contains(e, "no such function:")) {
    return "standard library functions need `INCLUDE PERFETTO MODULE` first: "
           "`trace_processor help agent` shows how to search for them";
  }
  return std::nullopt;
}

}  // namespace

AgentMode DecideAgentMode(std::optional<bool> force,
                          bool explicit_format,
                          bool stdout_is_tty) {
  AgentMode mode;
  if (force.has_value()) {
    mode.enabled = *force;
    return mode;
  }
  if (explicit_format || stdout_is_tty) {
    return mode;
  }
  mode.detected_env_var = DetectAgentEnvVar();
  mode.enabled = mode.detected_env_var.has_value();
  return mode;
}

ResultFormatOptions AgentResultFormatOptions() {
  ResultFormatOptions options;
  options.format = ResultFormat::kMarkdown;
  options.max_rows = 1000;
  options.max_bytes = 10000;
  options.max_cell_chars = 500;
  options.max_rows_past_limit = 100000;
  return options;
}

void PrintAgentModeNotice(const AgentMode& mode) {
  if (!mode.detected_env_var) {
    return;
  }
  PrintStatusLine(
      StatusLabelStyle::kHint, "agent mode",
      "on as " + *mode.detected_env_var +
          " is set: compact tables, capped results, JSON errors. "
          "--no-agent turns it off, `trace_processor help agent` explains "
          "more");
}

void PrintAgentModeHint() {
  PrintStatusLine(StatusLabelStyle::kHint, "hint",
                  "coding agent? --agent prints compact, capped tables and "
                  "JSON errors (`trace_processor help agent`)");
}

std::string FormatErrorAsJson(const base::Status& status) {
  SplitError split = SplitErrorMessage(status.message());
  std::optional<uint32_t> statement;
  if (auto payload = status.GetPayload(kFailedStatementPayload); payload) {
    statement = base::StringToUInt32(std::string(*payload));
  }
  std::optional<std::string> hint = HintForError(split.error);
  return json::SerializeJson([&](json::JsonValueSerializer&& writer) {
    std::move(writer).WriteDict([&](json::JsonDictSerializer& dict) {
      dict.AddString("error", split.error);
      if (statement) {
        dict.AddUint("statement", *statement);
      }
      if (split.location) {
        dict.AddString("location", *split.location);
      }
      if (!split.traceback.empty()) {
        dict.AddString("traceback", split.traceback);
      }
      if (hint) {
        dict.AddString("hint", *hint);
      }
    });
  });
}

}  // namespace perfetto::trace_processor::shell
