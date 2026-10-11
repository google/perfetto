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

#ifndef SRC_TRACE_PROCESSOR_SHELL_AGENT_MODE_H_
#define SRC_TRACE_PROCESSOR_SHELL_AGENT_MODE_H_

#include <cstdint>
#include <optional>
#include <string>

#include "perfetto/base/status.h"
#include "src/trace_processor/shell/result_formatter.h"

namespace perfetto::trace_processor::shell {

// Agent mode tunes the output of `query` for coding agents, which read stdout
// through a pipe, can't scroll or ask for omitted data and pay for every
// token: compact markdown tables with capped results and errors as
// single-line JSON.
struct AgentMode {
  bool enabled = false;
  // Set when agent mode was enabled because a coding-agent environment
  // variable is set (e.g. "CLAUDECODE") rather than by --agent.
  std::optional<std::string> detected_env_var;
};

// Decides whether agent mode is on. |force| is set by --agent (true) or
// --no-agent (false). Otherwise, agent mode is on when a coding-agent
// environment variable is set, stdout is not a terminal and no output format
// was requested explicitly: an explicit format is a deliberate choice (e.g. by
// a script parsing the output) which agent mode must not interfere with.
AgentMode DecideAgentMode(std::optional<bool> force,
                          bool explicit_format,
                          bool stdout_is_tty);

// Result output defaults in agent mode. Explicit flags take precedence.
ResultFormatOptions AgentResultFormatOptions();

// Status payload key: the 1-based index of the SQL statement which failed.
// Reported by FormatErrorAsJson.
inline constexpr char kFailedStatementPayload[] =
    "perfetto.dev/failed_statement";

// Prints the one-line stderr notice explaining that agent mode was turned on
// by |mode.detected_env_var| and how to turn it off.
void PrintAgentModeNotice(const AgentMode& mode);

// Prints a one-line stderr hint pointing at --agent. Used when a query fails
// with stdout redirected but agent mode was neither detected nor declined:
// agents which don't set a known environment variable would otherwise never
// learn about it.
void PrintAgentModeHint();

// Formats |status| as a single-line JSON object:
//   {"error": "no such column: foo", "statement": 2, "location": "query:3:8",
//    "traceback": "...", "hint": "..."}
// "statement" comes from the kFailedStatementPayload payload, "location" and
// "traceback" from SQL tracebacks (see SqlSource::AsTraceback) and "hint"
// suggests a next step for common errors. Only "error" is always present.
std::string FormatErrorAsJson(const base::Status& status);

}  // namespace perfetto::trace_processor::shell

#endif  // SRC_TRACE_PROCESSOR_SHELL_AGENT_MODE_H_
