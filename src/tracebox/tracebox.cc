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

void PrintUsage() {
  printf(R"(Welcome to Perfetto tracing!

Tracebox is a bundle containing all the tracing services and the perfetto
cmdline client in one binary.

QUICK START (end-to-end workflow):)");

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  printf(R"(
  tracebox ctl start                      # Start daemons
  tracebox -t 10s -o trace.pftrace sched  # Capture trace
  tracebox ctl stop                       # Stop daemons (optional)
)");
  PrintTraceboxCtlUsage();
  printf(R"(
ADVANCED: --autodaemonize (NOT RECOMMENDED)
  Example: tracebox --autodaemonize -t 10s -o trace.pftrace sched

  Spawns temporary daemons for one trace only.
  Limitations: SDK apps won't connect, inefficient for multiple traces.
  Use only for quick one-offs or when persistent daemons aren't feasible.
)");
  printf("\nAvailable applets:");
  for (const Applet& applet : g_applets)
    printf(" %s", applet.name);
  printf("\n");
#else
  printf(
      "\nStart the daemons manually before tracing and then run tracebox.\n");
#endif
  printf(R"(
See also:
  * https://perfetto.dev/docs/
  * The config editor in the record page of https://ui.perfetto.dev/
)");
}

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
// Synchronizes with child process startup via a pipe.
// The child writes "1" to the pipe when its IPC socket is ready.
base::Pipe CreateSyncPipe(base::Subprocess* proc, const char* env_name) {
  base::Pipe pipe = base::Pipe::Create();
  int fd = *pipe.wr;
  base::SetEnv(env_name, std::to_string(fd));
  proc->args.preserve_fds.emplace_back(fd);
  return pipe;
}

void WaitForNotify(base::Pipe* pipe, const char* name) {
  pipe->wr.reset();
  std::string msg;
  base::ReadPlatformHandle(*pipe->rd, &msg);
  if (msg != "1")
    PERFETTO_FATAL("The %s service failed unexpectedly. Check the logs", name);
}
#endif

// Legacy mode: spawns temporary daemons with private sockets for one trace.
int RunWithAutodaemonize(int argc, char** argv) {
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
  PERFETTO_FATAL("The autodaemonize mode is not supported on this platform");
#endif

  // If the caller has set the PERFETTO_*_SOCK_NAME, respect those.
  if (const char* env = getenv(kPerfettoConsumerSockEnv); env) {
    consumer_socket = env;
  }
  if (const char* env = getenv(kPerfettoProducerSockEnv); env) {
    producer_socket = env;
  }
  base::SetEnv(kPerfettoConsumerSockEnv, consumer_socket);
  base::SetEnv(kPerfettoProducerSockEnv, producer_socket);

  PerfettoCmd perfetto_cmd;

  // If the cmdline parsing fails, stop here, no need to spawn services.
  // It will daemonize if --background. In that case the subprocesses will be
  // spawned by the damonized cmdline client, which is what we want so killing
  // the backgrounded cmdline client will also kill the other services, as they
  // will live in the same background session.
  auto opt_res = perfetto_cmd.ParseCmdlineAndMaybeDaemonize(argc, argv);
  if (opt_res.has_value()) {
    if (*opt_res != 0) {
      PrintUsage();
    }
    return *opt_res;
  }

  std::string self_path = base::GetCurExecutablePath();
  base::Subprocess traced({self_path, "traced"});
#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  // |traced_sync_pipe| is used to synchronize with traced socket creation.
  // traced will write "1" and close the FD when the IPC socket is listening
  // (or traced crashed).
  base::Pipe traced_sync_pipe = CreateSyncPipe(&traced, "TRACED_NOTIFY_FD");
  // Create a new process group so CTRL-C is delivered only to the cmdline
  // process (the tracebox one) and not to traced. traced will still exit once
  // the main process exits, but this allows graceful stopping of the trace
  // without abruptedly killing traced{,probes} when hitting CTRL+C.
  traced.args.posix_proc_group_id = 0;  // 0 = start a new process group.
#endif
  traced.Start();

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  WaitForNotify(&traced_sync_pipe, "traced");
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
  base::Pipe traced_probes_sync_pipe =
      CreateSyncPipe(&traced_probes, "TRACED_PROBES_NOTIFY_FD");
#endif
  traced_probes.Start();

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  WaitForNotify(&traced_probes_sync_pipe, "traced_probes");
#endif

#if PERFETTO_BUILDFLAG(PERFETTO_TRACED_PERF)
  base::Subprocess traced_perf({self_path, "traced_perf"});
  // Put traced_perf in the same process group as traced. Same reason (CTRL+C)
  // but it's not worth creating a new group.
  traced_perf.args.posix_proc_group_id = traced.pid();

  base::Pipe traced_perf_sync_pipe =
      CreateSyncPipe(&traced_perf, "TRACED_PERF_NOTIFY_FD");
  traced_perf.Start();
  WaitForNotify(&traced_perf_sync_pipe, "traced_perf");
#else
  PERFETTO_ELOG(
      "Unsupported: linux.perf data source support (traced_perf) "
      "compiled-out.");
#endif

  return perfetto_cmd.ConnectToServiceRunAndMaybeNotify();
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
    PrintUsage();
    return 1;
  }

  // Handle --autodaemonize and --system-sockets flags by removing them from
  // argv.
  bool autodaemonize = false;

  for (int i = 1; i < argc;) {
    if (strcmp(argv[i], "--autodaemonize") == 0) {
      autodaemonize = true;
      memmove(&argv[i], &argv[i + 1], sizeof(char*) * static_cast<size_t>(argc - i - 1));
      argc--;
    } else if (strcmp(argv[i], "--system-sockets") == 0) {
      PERFETTO_ELOG(
          "Warning: --system-sockets is deprecated. System sockets are now the "
          "default.");
      memmove(&argv[i], &argv[i + 1], sizeof(char*) * static_cast<size_t>(argc - i - 1));
      argc--;
    } else {
      ++i;
    }
  }

  if (autodaemonize)
    return RunWithAutodaemonize(argc, argv);

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  // Require daemons to be running (started via 'tracebox ctl start').
  ServiceSockets sockets = perfetto::GetServiceSockets();
  if (!sockets.IsValid()) {
    fprintf(stderr,
            "Error: Perfetto tracing daemons (traced, traced_probes) are not "
            "running.\n"
            "- To run daemons as the current user: `tracebox ctl start`\n"
            "- For a self-contained run: `tracebox --autodaemonize ...` (Not "
            "Recommended)\n"
            "More info at: https://perfetto.dev/docs/reference/tracebox\n");
    return 1;
  }
  // Propagate discovered socket paths to perfetto_cmd via environment.
  perfetto::SetServiceSocketEnv(sockets);
#endif

  PerfettoCmd perfetto_cmd;
  auto opt_res = perfetto_cmd.ParseCmdlineAndMaybeDaemonize(argc, argv);
  if (opt_res.has_value()) {
    if (*opt_res != 0)
      PrintUsage();
    return *opt_res;
  }

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  // Windows: Spawn daemons with system sockets (no daemon-running check).
  // Maintains backward compatibility with old --system-sockets behavior.
  std::string self_path = base::GetCurExecutablePath();
  base::Subprocess traced({self_path, "traced"});
  traced.Start();

  base::Subprocess traced_probes(
      {self_path, "traced_probes", "--reset-ftrace"});
  traced_probes.Start();
#endif

  return perfetto_cmd.ConnectToServiceRunAndMaybeNotify();
}

}  // namespace
}  // namespace perfetto

int main(int argc, char** argv) {
  return perfetto::TraceboxMain(argc, argv);
}
