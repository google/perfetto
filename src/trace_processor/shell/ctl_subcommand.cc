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

#include "src/trace_processor/shell/ctl_subcommand.h"

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/rpc/session_paths.h"
#include "src/trace_processor/shell/subcommand.h"

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <csignal>
#else
#include <windows.h>
#endif

namespace perfetto::trace_processor::shell {
namespace {

// Returns true if a process with |pid| is alive (and, on Windows, terminates
// it). On POSIX this only probes; the actual kill is done by |TerminatePid|.
bool IsPidAlive(int pid) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
  if (!h)
    return false;
  bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
  CloseHandle(h);
  return alive;
#else
  return kill(pid, 0) == 0;
#endif
}

base::Status TerminatePid(int pid) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
  if (!h || !TerminateProcess(h, 0)) {
    if (h)
      CloseHandle(h);
    return base::ErrStatus("Failed to terminate pid %d", pid);
  }
  CloseHandle(h);
  return base::OkStatus();
#else
  if (kill(pid, SIGTERM) != 0)
    return base::ErrStatus("Failed to signal pid %d", pid);
  return base::OkStatus();
#endif
}

base::Status KillServer(const std::string& addr) {
  std::string socket_path;
  switch (session::ClassifyRemoteAddr(addr)) {
    case session::RemoteAddrKind::kHttp:
      return base::ErrStatus(
          "kill-server over HTTP (%s) is not supported; stop the server with "
          "Ctrl-C or run it with --idle-timeout.",
          addr.c_str());
    case session::RemoteAddrKind::kUnixPath:
      socket_path = addr;
      break;
    case session::RemoteAddrKind::kSessionName: {
      ASSIGN_OR_RETURN(socket_path, session::SessionSocketPath(addr));
      break;
    }
  }

  // The server writes its pid beside the socket; stop it by pid.
  std::string pid_path = socket_path + session::kPidFileSuffix;
  std::string contents;
  std::optional<int32_t> pid;
  if (base::ReadFile(pid_path, &contents))
    pid = base::StringToInt32(base::TrimWhitespace(contents));
  if (!pid.has_value() || !IsPidAlive(*pid)) {
    // Clean up a stale pid file / socket left by a crashed server.
    remove(pid_path.c_str());
    return base::ErrStatus("No live session at '%s'", addr.c_str());
  }

  RETURN_IF_ERROR(TerminatePid(*pid));
  // The POSIX server unlinks these from its signal handler; on Windows
  // TerminateProcess is abrupt, so clean up here too. remove() of an
  // already-gone path is harmless.
  remove(pid_path.c_str());
  remove(socket_path.c_str());
  printf("Stopped session '%s' (pid %d)\n", addr.c_str(), *pid);
  return base::OkStatus();
}

}  // namespace

const char* CtlSubcommand::name() const {
  return "ctl";
}

const char* CtlSubcommand::description() const {
  return "Manage warm sessions.";
}

const char* CtlSubcommand::usage_args() const {
  return "kill-server <name|socket-path>";
}

const char* CtlSubcommand::detailed_help() const {
  return R"(Management commands for warm `server unix` sessions.

Verbs:
  kill-server <name|socket-path>
      Stop a running session. The target is a session name, an absolute socket
      path, or a *.sock path (as printed in the server's startup record). The
      server is stopped by the pid it records beside its socket. HTTP targets
      are not supported (stop those with Ctrl-C or --idle-timeout).)";
}

std::vector<FlagSpec> CtlSubcommand::GetFlags() {
  return {};
}

base::Status CtlSubcommand::Run(const SubcommandContext& ctx) {
  if (ctx.positional_args.empty()) {
    return base::ErrStatus("ctl: a verb is required (e.g. kill-server)");
  }
  const std::string& verb = ctx.positional_args[0];
  if (verb == "kill-server") {
    if (ctx.positional_args.size() < 2) {
      return base::ErrStatus(
          "ctl kill-server: a session name or socket path is required");
    }
    return KillServer(ctx.positional_args[1]);
  }
  return base::ErrStatus("ctl: unknown verb '%s' (expected kill-server)",
                         verb.c_str());
}

}  // namespace perfetto::trace_processor::shell
