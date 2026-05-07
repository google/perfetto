/*
 * Copyright (C) 2019 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_EXT_TRACE_PROCESSOR_TRACE_PROCESSOR_SHELL_H_
#define INCLUDE_PERFETTO_EXT_TRACE_PROCESSOR_TRACE_PROCESSOR_SHELL_H_

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/trace_processor.h"

namespace perfetto::trace_processor {

// Forward declaration.
class TraceProcessorShell_PlatformInterface;

// Describes a coding agent that `trace_processor ai install-skills` can
// target. Three agents are always built in (claudecode, geminicli, codex);
// embedders that wrap trace_processor for restricted environments (e.g.
// Google internal) can register additional agents — for instance one that
// installs into an internal config root — by overriding
// TraceProcessorShell_PlatformInterface::GetExtraAiAgents().
struct AiAgentSpec {
  // The token the user passes on the command line, e.g. "claudecode".
  // Must be unique across builtins and embedder-supplied agents; if a
  // collision occurs the embedder's spec wins.
  std::string cli_name;

  // Human-readable name shown in install logs and help text, e.g.
  // "Anthropic Claude Code".
  std::string description;

  // Returns the absolute on-disk install root for this agent's skills,
  // or an error if it cannot be resolved (e.g. $HOME unset). Called
  // once when the agent is selected, before any files are written.
  std::function<base::StatusOr<std::string>()> resolve_install_dir;
};

// Class for implementing a trace processor shell.
//
// Only visible for embedders who want very fine grained control of how shell
// integrates with other systems. This is mainly for Google internal builds
// where we wrap trace_processor_shell with custom gfile handlers, integration
// with internal packages etc.
class TraceProcessorShell {
 public:
  // Type alias for platform interface.
  using PlatformInterface = TraceProcessorShell_PlatformInterface;

  // Creates a new instance of TraceProcessorShell with the provided
  // |platform_interface|.
  static std::unique_ptr<TraceProcessorShell> Create(
      std::unique_ptr<PlatformInterface> platform_interface);

  // Creates an instance of TraceProcessorShell with the default platform
  // implementation (i.e. no special customizations, works on all platforms).
  static std::unique_ptr<TraceProcessorShell> CreateWithDefaultPlatform();

  // Runs the shell with the provided command line arguments.
  base::Status Run(int argc, char** argv);

 private:
  // Creates a new instance of TraceProcessorShell with the provided
  // |platform_interface|.
  explicit TraceProcessorShell(
      std::unique_ptr<PlatformInterface> platform_interface);

  std::unique_ptr<PlatformInterface> platform_interface_;
};

// Abstract class for platform specific operations.
class TraceProcessorShell_PlatformInterface {
 public:
  virtual ~TraceProcessorShell_PlatformInterface();

  // Returns the default config struct for creating a new instance of
  // TraceProcessor.
  virtual Config DefaultConfig() const = 0;

  // Callback invoked when a new TraceProcessor instance is created.
  // Allows configuring the TraceProcessor before use (adding PerfettoSQL
  // modules etc).
  virtual base::Status OnTraceProcessorCreated(
      TraceProcessor* trace_processor) = 0;

  // Loads the trace located at |path| into the provided |trace_processor|.
  //
  // The implementation may optionally provide progress updates by invoking
  // |progress_callback| with the number of bytes parsed so far.
  virtual base::Status LoadTrace(
      TraceProcessor* trace_processor,
      const std::string& path,
      std::function<void(size_t)> progress_callback) = 0;

  // Additional agents the `trace_processor ai install-skills` subcommand
  // should accept on top of the built-in claudecode/geminicli/codex set.
  // Default returns empty; override to register environment-specific
  // agents. cli_name collisions resolve in favour of the override (so
  // embedders can also redirect a built-in agent to a different install
  // path).
  virtual std::vector<AiAgentSpec> GetExtraAiAgents() const { return {}; }
};

int PERFETTO_EXPORT_COMPONENT TraceProcessorShellMain(int argc, char** argv);

}  // namespace perfetto::trace_processor

#endif  // INCLUDE_PERFETTO_EXT_TRACE_PROCESSOR_TRACE_PROCESSOR_SHELL_H_
