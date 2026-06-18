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

#include "src/trace_processor/shell/server_subcommand.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/ext/trace_processor/trace_processor_shell.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/rpc/rpc.h"
#include "src/trace_processor/rpc/session_paths.h"
#include "src/trace_processor/rpc/stdiod.h"
#include "src/trace_processor/shell/common_flags.h"
#include "src/trace_processor/shell/metatrace.h"
#include "src/trace_processor/shell/subcommand.h"

#if PERFETTO_BUILDFLAG(PERFETTO_TP_HTTPD)
#include "src/trace_processor/rpc/httpd.h"
#endif

// The unix server depends on the IPC layer (AF_UNIX sockets), which is not
// available on all platforms (e.g. Chromium+Windows).
#if PERFETTO_BUILDFLAG(PERFETTO_IPC)
#include "src/trace_processor/rpc/unixd.h"
#endif

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) ||   \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_FREEBSD) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
#define PERFETTO_HAS_SIGNAL_H() 1
#include <signal.h>
#else
#define PERFETTO_HAS_SIGNAL_H() 0
#endif

namespace perfetto::trace_processor::shell {

const char* ServerSubcommand::name() const {
  return "server";
}

const char* ServerSubcommand::description() const {
  return "Start an RPC server.";
}

const char* ServerSubcommand::usage_args() const {
  return "<mode> [trace_file]";
}

const char* ServerSubcommand::detailed_help() const {
  return R"(Start an RPC server for remote trace processor access.

Modes:
  http   Start an HTTP server (default port 9001). This is what the
         Perfetto UI (ui.perfetto.dev) connects to. Configure with
         --port and --ip-address.
  stdio  Communicate via stdin/stdout using length-prefixed RPC protocol.
         Used by tooling that embeds trace processor as a subprocess.
  unix   Serve over an AF_UNIX socket addressed by name, keeping the trace
         warm for repeated 'query --remote <name>' calls. Use --name to pick
         a name (otherwise one is generated) or --path for an explicit socket
         path. Runs in the foreground; press Ctrl-C to stop.

The trace file is optional in http mode (traces can be loaded remotely) but
required in unix mode.)";
}

std::vector<FlagSpec> ServerSubcommand::GetFlags() {
  return {
      StringFlag("port", '\0', "PORT", "HTTP port.", &port_number_),
      StringFlag("ip-address", '\0', "IP", "HTTP bind address.", &listen_ip_),
      StringFlag("additional-cors-origins", '\0', "O1,O2,...",
                 "Additional CORS origins for HTTP mode.",
                 &additional_cors_origins_str_),
      StringFlag("name", '\0', "NAME",
                 "Session name for unix mode (default: auto-generated).",
                 &session_name_),
      StringFlag("path", '\0', "PATH",
                 "Explicit socket path for unix mode (overrides --name).",
                 &socket_path_),
  };
}

base::Status ServerSubcommand::Run(const SubcommandContext& ctx) {
  // First positional arg is the mode.
  if (ctx.positional_args.empty()) {
    return base::ErrStatus(
        "server: must specify mode (expected http, stdio or unix)");
  }
  const std::string& mode = ctx.positional_args[0];

  // Optional trace file is second positional arg.
  std::string trace_file;
  if (ctx.positional_args.size() >= 2) {
    trace_file = ctx.positional_args[1];
  }

  auto config = BuildConfig(*ctx.global, ctx.platform);
  ASSIGN_OR_RETURN(auto tp,
                   SetupTraceProcessor(*ctx.global, config, ctx.platform));

  if (!trace_file.empty()) {
    ASSIGN_OR_RETURN(auto t_load,
                     LoadTraceFile(tp.get(), ctx.platform, trace_file));
    base::ignore_result(t_load);
  }

  bool has_trace = !trace_file.empty();

  if (mode == "stdio") {
    Rpc rpc(std::move(tp), has_trace, config, [&ctx](TraceProcessor* new_tp) {
      ctx.platform->OnTraceProcessorCreated(new_tp);
    });
#if PERFETTO_HAS_SIGNAL_H()
    static Rpc* g_rpc_for_signal_handler = &rpc;
    signal(SIGINT, [](int) {
      g_rpc_for_signal_handler->trace_processor()->InterruptQuery();
    });
#endif
    return RunStdioRpcServer(rpc);
  }

  if (mode == "http") {
#if PERFETTO_BUILDFLAG(PERFETTO_TP_HTTPD)
    std::vector<std::string> additional_cors_origins;
    if (!additional_cors_origins_str_.empty()) {
      additional_cors_origins =
          base::SplitString(additional_cors_origins_str_, ",");
    }
    Rpc rpc(std::move(tp), has_trace, config, [&ctx](TraceProcessor* new_tp) {
      ctx.platform->OnTraceProcessorCreated(new_tp);
    });
#if PERFETTO_HAS_SIGNAL_H()
    if (ctx.global->metatrace_path.empty()) {
      signal(SIGINT, SIG_DFL);
    } else {
      static std::string* metatrace_path = &ctx.global->metatrace_path;
      static Rpc* g_rpc_for_signal_handler = &rpc;
      signal(SIGINT, [](int) {
        MaybeWriteMetatrace(g_rpc_for_signal_handler->trace_processor(),
                            *metatrace_path);
        exit(1);
      });
    }
#endif
    RunHttpRPCServer(rpc, listen_ip_, port_number_, additional_cors_origins);
    PERFETTO_FATAL("Should never return");
#else
    return base::ErrStatus("HTTP RPC module not supported in this build");
#endif
  }

  if (mode == "unix") {
#if !PERFETTO_BUILDFLAG(PERFETTO_IPC)
    return base::ErrStatus("Unix RPC module not supported in this build");
#else
    if (!has_trace) {
      return base::ErrStatus("server unix: a trace file is required");
    }
    // --path and --name are mutually exclusive: --path is an explicit socket
    // path while --name selects one in the managed session dir.
    if (!socket_path_.empty() && !session_name_.empty()) {
      return base::ErrStatus(
          "server unix: --path and --name are mutually exclusive");
    }
    // Resolve the socket path: --path wins, else --name, else a generated name.
    std::string socket_path = socket_path_;
    std::string session_name = session_name_;
    if (socket_path.empty()) {
      if (session_name.empty()) {
        session_name = session::GenerateSessionName();
      } else if (!session::IsValidSessionName(session_name)) {
        return base::ErrStatus(
            "server unix: invalid --name '%s' (must match "
            "[A-Za-z0-9][A-Za-z0-9_-]*)",
            session_name.c_str());
      }
      ASSIGN_OR_RETURN(socket_path, session::SessionSocketPath(session_name));
    } else {
      RETURN_IF_ERROR(session::ValidateAfUnixPathLength(socket_path));
      if (session_name.empty())
        session_name = socket_path;
    }

    Rpc rpc(std::move(tp), has_trace, config, [&ctx](TraceProcessor* new_tp) {
      ctx.platform->OnTraceProcessorCreated(new_tp);
    });
    UnixServerArgs server_args;
    server_args.socket_path = socket_path;
    server_args.session_name = session_name;
    return RunUnixRpcServer(rpc, server_args);
#endif
  }

  return base::ErrStatus(
      "server: unknown mode '%s' (expected http, stdio or unix)", mode.c_str());
}

}  // namespace perfetto::trace_processor::shell
