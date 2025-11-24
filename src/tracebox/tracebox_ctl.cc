/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/tracebox/tracebox_ctl.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/subprocess.h"
#include "perfetto/ext/base/temp_file.h"
#include "perfetto/ext/base/unix_socket.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/tracing/default_socket.h"

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace perfetto {
namespace {

constexpr const char* kDaemons[] = {
    "traced",
    "traced_probes",
#if PERFETTO_BUILDFLAG(PERFETTO_TRACED_PERF)
    "traced_perf",
#endif
};

bool CanConnectToSocket(const std::string& path) {
  base::SockFamily family = base::GetSockFamily(path.c_str());
  auto sock =
      base::UnixSocketRaw::CreateMayFail(family, base::SockType::kStream);
  return sock && sock.Connect(path);
}

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)

std::string GetPidFilePath(const char* daemon_name) {
  return base::GetSysTempDir() + "/" + daemon_name + ".pid";
}

bool WritePidToFile(const std::string& path, pid_t pid) {
  base::ScopedFile fd =
      base::OpenFile(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (!fd)
    return false;
  std::string content = std::to_string(pid);
  auto written = base::WriteAll(fd.get(), content.c_str(), content.size());
  return written >= 0 && static_cast<size_t>(written) == content.size();
}

pid_t ReadPidFromFile(const std::string& path) {
  std::string pid_str;
  if (!base::ReadFile(path, &pid_str))
    return 0;
  pid_str = base::TrimWhitespace(pid_str);
  if (pid_str.empty())
    return 0;
  auto pid = base::StringToInt64(pid_str);
  return pid.has_value() ? static_cast<pid_t>(*pid) : 0;
}

pid_t StartDaemon(const std::string& tracebox_path,
                  const std::string& daemon_name) {
  std::vector<std::string> cmd = {tracebox_path, daemon_name, "--background"};

  base::Subprocess daemon;
  daemon.args.exec_cmd = cmd;
  daemon.args.stdout_mode = base::Subprocess::OutputMode::kBuffer;
  daemon.Start();

  constexpr uint32_t kDaemonStartTimeoutMs = 1000;
  if (!daemon.Wait(kDaemonStartTimeoutMs)) {
    printf("Error: Daemon %s failed to start after %u ms\n",
           daemon_name.c_str(), kDaemonStartTimeoutMs);
    return 0;
  }
  if (daemon.status() != base::Subprocess::kTerminated ||
      daemon.returncode() != 0) {
    printf("Error: Daemon %s failed to start, status:%d exit code:%d\n",
           daemon_name.c_str(), daemon.status(), daemon.returncode());
    return 0;
  }

  std::string output = daemon.output();
  auto pid = base::StringToInt64(base::TrimWhitespace(output));
  if (!pid.has_value()) {
    printf("Error: Failed to parse daemon PID from output: %s\n",
           output.c_str());
    return 0;
  }
  return static_cast<pid_t>(*pid);
}

#endif  // !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)

int CtlStop() {
  printf("Stopping daemons...\n");

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  ServiceSockets sockets = GetRunningSockets();
  if (!sockets.IsValid()) {
    printf("No daemons detected.\n");
    return 0;
  }
  printf("Daemons are running. Use Task Manager to stop them.\n");
  return 1;
#else
  int daemons_stopped = 0, daemons_stored = 0;
  for (const char* daemon : kDaemons) {
    std::string pid_path = GetPidFilePath(daemon);
    if (!base::FileExists(pid_path)) {
      PERFETTO_LOG("Not found pid file for %s at %s", daemon, pid_path.c_str());
      continue;
    }
    daemons_stored++;
    pid_t pid = ReadPidFromFile(pid_path);
    if (pid && kill(pid, SIGTERM) == 0) {
      daemons_stopped++;
      printf("%s terminated (PID: %d)\n", daemon, pid);
      if (remove(pid_path.c_str()) != 0) {
        printf("Error: Failed to delete file: %s with error %s\n",
               pid_path.c_str(), strerror(errno));
      }
    } else {
      printf("Error: Failed to stop daemon %s (pid=%d): %s\n", daemon,
             static_cast<int>(pid), strerror(errno));
    }
  }

  if (daemons_stopped == sizeof(kDaemons)) {
    printf("All daemons stopped.\n");
    return 0;
  }
  return daemons_stored == 0;
#endif
}

int CtlStart() {
  constexpr uint32_t kDaemonStartWaitUs = 1000 * 1000;  // 1s
  ServiceSockets sockets = GetRunningSockets();
  if (sockets.IsValid()) {
    printf("Status: Daemons are already running with %s\n",
           sockets.ToString().c_str());
    return 0;
  }

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  printf("Starting daemons...\n");
  std::string tracebox_path = base::GetCurExecutablePath();
  std::vector<base::Subprocess> processes;
  for (const char* daemon : kDaemons) {
    auto proc = base::Subprocess({tracebox_path, daemon});
    proc.Start();
    processes.push_back(std::move(proc));
  }
  base::SleepMicroseconds(kDaemonStartWaitUs);

  ServiceSockets started_sockets = GetRunningSockets();
  if (!started_sockets.IsValid()) {
    printf(R"(Error: Failed to start daemons. Possible causes:
  - Ports 32278/32279 may already be in use
  - Firewall may be blocking the connections
  - Insufficient permissions
)");
    return 1;
  }
  printf("Success: Daemons started with %s. Press Ctrl+C to stop.\n",
         started_sockets.ToString().c_str());
  while (true)
    base::SleepMicroseconds(1000000);
#else
  printf("Starting daemons...\n");
  std::string tracebox_path = base::GetCurExecutablePath();
  for (const char* daemon : kDaemons) {
    pid_t pid = StartDaemon(tracebox_path, daemon);
    if (pid <= 0) {
      printf("Error: Failed to start %s daemon\n", daemon);
      CtlStop();
      return 1;
    }
    std::string pid_path = GetPidFilePath(daemon);
    if (!WritePidToFile(pid_path, pid)) {
      printf("Error: Failed to write PID file for %s\n", daemon);
      CtlStop();
      return 1;
    }
    printf("%s started (PID: %d)\n", daemon, pid);
  }

  base::SleepMicroseconds(kDaemonStartWaitUs);
  ServiceSockets started_sockets = GetRunningSockets();
  if (!started_sockets.IsValid()) {
    printf("Failed to connect to socket: %s\n",
           started_sockets.ToString().c_str());
    CtlStop();
    return 1;
  }
  return 0;
#endif
}

int CtlStatus() {
  ServiceSockets sockets = GetRunningSockets();
  if (sockets.IsValid()) {
    printf("Success: Daemons are running and accessible with %s\n",
           sockets.ToString().c_str());
  } else {
    printf("Status: No daemons detected via sockets\n");
  }

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  bool stale_file_found = false;
  for (const char* daemon : kDaemons) {
    std::string pid_path = GetPidFilePath(daemon);
    if (!base::FileExists(pid_path)) {
      PERFETTO_LOG("PID file for %s not present at %s", daemon,
                   pid_path.c_str());
      continue;
    }
    pid_t pid = ReadPidFromFile(pid_path);
    if (pid && kill(pid, 0) == 0) {
      printf("%s: Running (PID %d)\n", daemon, static_cast<int>(pid));
    } else {
      printf("%s: Not running (Stale PID file %d)\n", daemon,
             static_cast<int>(pid));
      stale_file_found = true;
    }
  }
  if (stale_file_found) {
    printf(
        "Stale PID files found. Run 'tracebox ctl stop' to clean them up.\n");
  }
#endif
  return 0;
}

}  // namespace

void PrintTraceboxCtlUsage() {
  printf(R"( 
tracebox ctl [start|stop|status] [OPTIONS]
Manages the lifecycle of Perfetto daemons (traced, traced_probes).

Commands:
  start: Starts daemons for the current user session.
  stop: Stops user-session daemons.
  status: Shows the status of user-session daemons.
)");
}

ServiceSockets GetRunningSockets() {
  ServiceSockets sockets;
  sockets.producer_socket = perfetto::GetProducerSocket();
  sockets.consumer_socket = perfetto::GetConsumerSocket();
  return CanConnectToSocket(sockets.consumer_socket) ? sockets
                                                     : ServiceSockets{};
}

int TraceboxCtlMain(int argc, char** argv) {
  if (argc < 2) {
    PrintTraceboxCtlUsage();
    return 1;
  }

  const char* cmd = argv[1];
  if (strcmp(cmd, "start") == 0)
    return CtlStart();
  if (strcmp(cmd, "stop") == 0)
    return CtlStop();
  if (strcmp(cmd, "status") == 0)
    return CtlStatus();

  PrintTraceboxCtlUsage();
  return 1;
}
}  // namespace perfetto
