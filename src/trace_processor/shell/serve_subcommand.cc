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

#include "src/trace_processor/shell/serve_subcommand.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/getopt.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/trace_processor/trace_processor_shell.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/rpc/rpc.h"
#include "src/trace_processor/rpc/stdiod.h"
#include "src/trace_processor/shell/common_flags.h"

#if PERFETTO_BUILDFLAG(PERFETTO_TP_HTTPD)
#include "src/trace_processor/rpc/httpd.h"
#endif

namespace perfetto::trace_processor::shell {

const char* ServeSubcommand::name() const {
  return "serve";
}

const char* ServeSubcommand::description() const {
  return "Start an RPC server.";
}

void ServeSubcommand::PrintUsage(const char* argv0) {
  PERFETTO_ELOG(R"(
Start an RPC server.

Usage: %s serve <mode> [flags] [trace_file]

Modes: http, stdio

Flags (http mode only):
  --port PORT            HTTP port.
  --ip-address IP        HTTP bind address.
  --additional-cors-origins O1,O2,...
)",
                argv0);
}

int ServeSubcommand::Run(const SubcommandContext& ctx, int argc, char** argv) {
  GlobalOptions global;
  std::string port_number;
  std::string listen_ip;
  std::vector<std::string> additional_cors_origins;

  enum LocalOption {
    OPT_PORT = 500,
    OPT_IP_ADDRESS,
    OPT_ADDITIONAL_CORS_ORIGINS,
  };

  static const option long_options[] = {
      {"port", required_argument, nullptr, OPT_PORT},
      {"ip-address", required_argument, nullptr, OPT_IP_ADDRESS},
      {"additional-cors-origins", required_argument, nullptr,
       OPT_ADDITIONAL_CORS_ORIGINS},
      GLOBAL_LONG_OPTIONS{nullptr, 0, nullptr, 0}};

  optind = 1;
  for (;;) {
    int option = getopt_long(argc, argv, "m:h", long_options, nullptr);
    if (option == -1)
      break;
    if (HandleGlobalOption(option, optarg, global))
      continue;
    if (option == OPT_PORT) {
      port_number = optarg;
      continue;
    }
    if (option == OPT_IP_ADDRESS) {
      listen_ip = optarg;
      continue;
    }
    if (option == OPT_ADDITIONAL_CORS_ORIGINS) {
      additional_cors_origins = base::SplitString(optarg, ",");
      continue;
    }
    PrintUsage(argv[0]);
    return option == 'h' ? 0 : 1;
  }

  // First positional arg is the mode.
  if (optind >= argc) {
    PERFETTO_ELOG("serve: must specify mode (http or stdio)");
    PrintUsage(argv[0]);
    return 1;
  }
  const char* mode = argv[optind++];

  // Optional trace file.
  if (optind < argc) {
    global.trace_file = argv[optind];
  }

  auto config = BuildConfig(global, ctx.platform);
  auto tp_or = SetupTraceProcessor(global, config, ctx.platform);
  if (!tp_or.ok()) {
    PERFETTO_ELOG("%s", tp_or.status().c_message());
    return 1;
  }
  auto tp = std::move(*tp_or);

  if (!global.trace_file.empty()) {
    auto t_load_or = LoadTraceFile(tp.get(), ctx.platform, global.trace_file);
    if (!t_load_or.ok()) {
      PERFETTO_ELOG("%s", t_load_or.status().c_message());
      return 1;
    }
  }

  bool has_trace = !global.trace_file.empty();

  if (strcmp(mode, "stdio") == 0) {
    Rpc rpc(std::move(tp), has_trace, config, [&ctx](TraceProcessor* new_tp) {
      ctx.platform->OnTraceProcessorCreated(new_tp);
    });
    auto status = RunStdioRpcServer(rpc);
    if (!status.ok()) {
      PERFETTO_ELOG("%s", status.c_message());
      return 1;
    }
    return 0;
  }

  if (strcmp(mode, "http") == 0) {
#if PERFETTO_BUILDFLAG(PERFETTO_TP_HTTPD)
    Rpc rpc(std::move(tp), has_trace, config, [&ctx](TraceProcessor* new_tp) {
      ctx.platform->OnTraceProcessorCreated(new_tp);
    });
    RunHttpRPCServer(rpc, listen_ip, port_number, additional_cors_origins);
    PERFETTO_FATAL("Should never return");
#else
    PERFETTO_ELOG("HTTP RPC module not supported in this build");
    return 1;
#endif
  }

  PERFETTO_ELOG("serve: unknown mode '%s' (expected http or stdio)", mode);
  PrintUsage(argv[0]);
  return 1;
}

}  // namespace perfetto::trace_processor::shell
