/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/proc_utils.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/pipe.h"
#include "perfetto/ext/base/subprocess.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/ext/traced/traced.h"
#include "src/perfetto_cmd/perfetto_cmd.h"
#include "src/tracebox/tracebox_ctl.h"
#include "src/websocket_bridge/websocket_bridge.h"

#if PERFETTO_BUILDFLAG(PERFETTO_TRACED_PERF)
#include "src/profiling/perf/traced_perf.h"
#endif

namespace perfetto {
namespace {

struct Applet {
  using MainFunction = int (*)(int /*argc*/, char** /*argv*/);
  const char* name;
  MainFunction entrypoint;
};

const Applet g_applets[]{
    {"traced", ServiceMain},
    {"traced_probes", ProbesMain},
#if PERFETTO_BUILDFLAG(PERFETTO_TRACED_PERF)
    {"traced_perf", TracedPerfMain},
#endif
    {"perfetto", PerfettoCmdMain},
    {"trigger_perfetto", TriggerPerfettoMain},
    {"websocket_bridge", WebsocketBridgeMain},
    {"ctl", TraceboxCtlMain},
};

void PrintTraceboxUsage() {
  printf(R"(Welcome to Perfetto tracing!

Tracebox is a bundle containing all the tracing services and the perfetto
cmdline client in one binary.

QUICK START (end-to-end workflow):
  tracebox ctl start                      # Start daemons
  tracebox -t 10s -o trace.pftrace sched  # Capture trace
  tracebox ctl stop                       # Stop daemons (optional)
)");

  PrintTraceboxCtlUsage();

  std::string applets;
  for (const Applet& applet : g_applets)
    applets += " " + std::string(applet.name);

  printf(R"(
ADVANCED: --legacy (NOT RECOMMENDED)
  Example: tracebox --legacy -t 10s -o trace.pftrace sched

  Spawns temporary daemons for one trace only.
  Limitations: SDK apps won't connect, inefficient for multiple traces.
  Use only for quick one-offs or when persistent daemons aren't feasible.

Available applets:%s

See also:
  * https://perfetto.dev/docs/
  * The config editor in the record page of https://ui.perfetto.dev/
)",
         applets.c_str());
}

// Legacy mode: spawns temporary daemons with private sockets for one trace.
int RunLegacy(int argc, char** argv) {
  auto* end = std::remove_if(argv, argv + argc, [](char* arg) {
    return !strcmp(arg, "--system-sockets");
  });
  if (end < (argv + argc - 1)) {
    PERFETTO_ELOG("Cannot specify --system-sockets multiple times");
    return 1;
  }
  if (bool system_sockets = end == (argv + argc - 1); system_sockets) {
    argc--;
  } else {
    auto pid_str = std::to_string(static_cast<uint64_t>(base::GetProcessId()));
#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
    // Use an unlinked abstract domain socket on Linux/Android.
    std::string consumer_socket = "@traced-c-" + pid_str;
    std::string producer_socket = "@traced-p-" + pid_str;
#elif PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
    std::string consumer_socket = "/tmp/traced-c-" + pid_str;
    std::string producer_socket = "/tmp/traced-p-" + pid_str;
#else
    PERFETTO_FATAL("The autostart mode is not supported on this platform");
#endif

    // If the caller has set the PERFETTO_*_SOCK_NAME, respect those.
    if (const char* env = getenv("PERFETTO_CONSUMER_SOCK_NAME"); env) {
      consumer_socket = env;
    }
    if (const char* env = getenv("PERFETTO_PRODUCER_SOCK_NAME"); env) {
      producer_socket = env;
    }
    base::SetEnv("PERFETTO_CONSUMER_SOCK_NAME", consumer_socket);
    base::SetEnv("PERFETTO_PRODUCER_SOCK_NAME", producer_socket);
  }

  PerfettoCmd perfetto_cmd;

  // If the cmdline parsing fails, stop here, no need to spawn services.
  // It will daemonize if --background. In that case the subprocesses will be
  // spawned by the damonized cmdline client, which is what we want so killing
  // the backgrounded cmdline client will also kill the other services, as they
  // will live in the same background session.
  auto opt_res = perfetto_cmd.ParseCmdlineAndMaybeDaemonize(argc, argv);
  if (opt_res.has_value()) {
    if (*opt_res != 0) {
      PrintTraceboxUsage();
    }
    return *opt_res;
  }

  std::string self_path = base::GetCurExecutablePath();
  base::Subprocess traced({self_path, "traced"});
#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  // |traced_sync_pipe| is used to synchronize with traced socket creation.
  // traced will write "1" and close the FD when the IPC socket is listening
  // (or traced crashed).
  base::Pipe traced_sync_pipe = base::Pipe::Create();
  int traced_fd = *traced_sync_pipe.wr;
  base::SetEnv("TRACED_NOTIFY_FD", std::to_string(traced_fd));
  traced.args.preserve_fds.emplace_back(traced_fd);
  // Create a new process group so CTRL-C is delivered only to the cmdline
  // process (the tracebox one) and not to traced. traced will still exit once
  // the main process exits, but this allows graceful stopping of the trace
  // without abruptedly killing traced{,probes} when hitting CTRL+C.
  traced.args.posix_proc_group_id = 0;  // 0 = start a new process group.
#endif
  traced.Start();

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  traced_sync_pipe.wr.reset();

  std::string traced_notify_msg;
  base::ReadPlatformHandle(*traced_sync_pipe.rd, &traced_notify_msg);
  if (traced_notify_msg != "1")
    PERFETTO_FATAL("The tracing service failed unexpectedly. Check the logs");
#endif

  base::Subprocess traced_probes(
      {self_path, "traced_probes", "--reset-ftrace"});
  // Put traced_probes in the same process group as traced. Same reason (CTRL+C)
  // but it's not worth creating a new group.
  traced_probes.args.posix_proc_group_id = traced.pid();
#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  // |traced_probes_sync_pipe| is used to synchronize with traced socket
  // creation. traced will write "1" and close the FD when the IPC socket is
  // listening (or traced crashed).
  base::Pipe traced_probes_sync_pipe = base::Pipe::Create();
  int traced_probes_fd = *traced_probes_sync_pipe.wr;
  base::SetEnv("TRACED_PROBES_NOTIFY_FD", std::to_string(traced_probes_fd));
  traced_probes.args.preserve_fds.emplace_back(traced_probes_fd);
#endif
  traced_probes.Start();

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  traced_probes_sync_pipe.wr.reset();

  std::string traced_probes_notify_msg;
  base::ReadPlatformHandle(*traced_probes_sync_pipe.rd,
                           &traced_probes_notify_msg);
  if (traced_probes_notify_msg != "1")
    PERFETTO_FATAL(
        "The traced_probes service failed unexpectedly. Check the logs");
#endif

#if PERFETTO_BUILDFLAG(PERFETTO_TRACED_PERF)
  base::Subprocess traced_perf({self_path, "traced_perf"});
  // Put traced_perf in the same process group as traced. Same reason (CTRL+C)
  // but it's not worth creating a new group.
  traced_perf.args.posix_proc_group_id = traced.pid();

  base::Pipe traced_perf_sync_pipe = base::Pipe::Create();
  int traced_perf_fd = *traced_perf_sync_pipe.wr;
  base::SetEnv("TRACED_PERF_NOTIFY_FD", std::to_string(traced_perf_fd));
  traced_perf.args.preserve_fds.emplace_back(traced_perf_fd);
  traced_perf.Start();
  traced_perf_sync_pipe.wr.reset();

  std::string traced_perf_notify_msg;
  base::ReadPlatformHandle(*traced_perf_sync_pipe.rd, &traced_perf_notify_msg);
  if (traced_perf_notify_msg != "1") {
    PERFETTO_FATAL(
        "The traced_perf service failed unexpectedly. Check the logs");
  }
#else
  PERFETTO_ELOG(
      "Unsupported: linux.perf data source support (traced_perf) "
      "compiled-out.");
#endif

  perfetto_cmd.ConnectToServiceRunAndMaybeNotify();
  return 0;
}

int TraceboxMain(int argc, char** argv) {
  // Applet mode: invoke directly if argv[0] or argv[1] matches an applet name.
  char* slash = strrchr(argv[0], '/');
  char* argv0 = slash ? slash + 1 : argv[0];

  for (const Applet& applet : g_applets) {
    if (strcmp(argv0, applet.name) == 0)
      return applet.entrypoint(argc, argv);
    if (argc > 1 && strcmp(argv[1], applet.name) == 0)
      return applet.entrypoint(argc - 1, &argv[1]);
  }

  if (argc <= 1) {
    PrintTraceboxUsage();
    return 1;
  }

  bool legacy_mode = false;
  bool use_system_sockets = false;
  for (int i = 1; i < argc;) {
    if (strcmp(argv[i], "--legacy") == 0) {
      legacy_mode = true;
      memmove(static_cast<void*>(&argv[i]), static_cast<void*>(&argv[i + 1]),
              sizeof(char*) * static_cast<size_t>(argc - i - 1));
      argc--;
    } else {
      if (strcmp(argv[i], "--system-sockets") == 0) {
        use_system_sockets = true;
      }
      ++i;
    }
  }

  if (legacy_mode) {
    if (use_system_sockets) {
      PERFETTO_ELOG(
          "Legacy mode using system sockets is deprecated. Please prefer to "
          "start daemons separately with `tracebox ctl start` for better "
          "stability and compatibility. See "
          "https://perfetto.dev/docs/reference/tracebox for details.");
    } else {
      PERFETTO_ELOG(
          "Using legacy autodaemonize mode. This is fine for quick "
          "one-off traces, but for production use or SDK compatibility, please "
          "use `tracebox ctl start` followed by `tracebox`. See "
          "https://perfetto.dev/docs/reference/tracebox for details.");
    }
    return RunLegacy(argc, argv);
  }

  if (use_system_sockets) {
    PERFETTO_FATAL(
        "System sockets is not a valid option for the new mode. By default "
        "tracebox uses system sockets. If you want the old self-contained "
        "behavior (autostarting daemons), append --legacy.");
  }

  ServiceSockets sockets = perfetto::GetRunningSockets();
  if (!sockets.IsValid()) {
    fprintf(stderr,
            "Error: Perfetto tracing daemons (traced, traced_probes) are not "
            "running.\n"
            "- To run daemons as the current user: `tracebox ctl start`\n"
            "- For a self-contained run: `tracebox --legacy ...` (Not "
            "Recommended)\n"
            "More info at: https://perfetto.dev/docs/reference/tracebox\n");
    return 1;
  }
  perfetto::SetServiceSocketEnv(sockets);

  PerfettoCmd perfetto_cmd;
  auto opt_res = perfetto_cmd.ParseCmdlineAndMaybeDaemonize(argc, argv);
  if (opt_res.has_value()) {
    if (*opt_res != 0)
      PrintTraceboxUsage();
    return *opt_res;
  }
  return perfetto_cmd.ConnectToServiceRunAndMaybeNotify();
}

}  // namespace
}  // namespace perfetto

int main(int argc, char** argv) {
  return perfetto::TraceboxMain(argc, argv);
}
