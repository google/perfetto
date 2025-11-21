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
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/subprocess.h"
#include "perfetto/ext/base/temp_file.h"
#include "perfetto/ext/base/unix_socket.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/tracing/default_socket.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include "perfetto/base/time.h"
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace perfetto {
namespace {

// Environment variables for socket paths.
constexpr char kPerfettoProducerSockEnv[] = "PERFETTO_PRODUCER_SOCK_NAME";
constexpr char kPerfettoConsumerSockEnv[] = "PERFETTO_CONSUMER_SOCK_NAME";

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

bool IsSystemdServiceInstalled() {
#if PERFETTO_BUILDFLAG(PERFETTO_SYSTEMD)
  constexpr const char* kSystemdServices[] = {
      "/etc/systemd/system/traced.service",
      "/lib/systemd/system/traced.service",
      "/usr/lib/systemd/system/traced.service",
  };
  for (const char* path : kSystemdServices) {
    if (base::FileExists(path))
      return true;
  }
#endif
  return false;
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
    PERFETTO_ELOG("Daemon %s failed to start (timeout)", daemon_name.c_str());
    return 0;
  }
  if (daemon.status() != base::Subprocess::kTerminated ||
      daemon.returncode() != 0) {
    PERFETTO_ELOG("Daemon %s failed to start (exit code: %d)",
                  daemon_name.c_str(), daemon.returncode());
    return 0;
  }
  std::string output = daemon.output();
  output = base::TrimWhitespace(output);
  auto pid = base::StringToInt64(output);
  if (!pid.has_value()) {
    PERFETTO_ELOG("Failed to parse daemon PID from output: %s", output.c_str());
    return 0;
  }
  return static_cast<pid_t>(*pid);
}
#endif  // !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)

int CtlStop() {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  ServiceSockets sockets = GetRunningSockets();
  if (!sockets.IsValid()) {
    printf("No daemons detected.\n");
    return 0;
  }
  printf(
      "Daemons are running. Use Task Manager or taskkill /IM tracebox.exe /F "
      "to stop them.\n");
  return 1;
#else
  PERFETTO_LOG("Stopping daemons...");

  auto env_cleaner = base::OnScopeExit([] {
    base::UnsetEnv(kPerfettoProducerSockEnv);
    base::UnsetEnv(kPerfettoConsumerSockEnv);
  });

  bool found_any = false;
  bool all_stopped = true;
  for (const char* daemon : kDaemons) {
    std::string pid_path = GetPidFilePath(daemon);
    pid_t pid = ReadPidFromFile(pid_path);
    if (pid != 0) {
      found_any = true;
      if (kill(pid, SIGTERM) != 0) {
        PERFETTO_ELOG("Failed to stop daemon (pid=%d): %s",
                      static_cast<int>(pid), strerror(errno));
        all_stopped = false;
      } else {
        base::ignore_result(remove(pid_path.c_str()));
      }
    }
  }
  if (!found_any) {
    PERFETTO_LOG("No daemon PID files found.");
    ServiceSockets sockets = GetRunningSockets();
    if (!sockets.IsValid()) {
      printf("No daemons detected.\n");
      return 0;
    }
    printf(
        "However, daemons are running (detected via socket connectivity) with "
        "%s",
        sockets.ToString().c_str());
    if (IsSystemdServiceInstalled()) {
      if (base::GetCurrentUserId() != 0) {
        printf(
            "Managed by systemd. Use: sudo systemctl stop traced "
            "traced-probes\n");
        return 0;
      }
      PERFETTO_LOG("Systemd service found. Trying to stop via systemctl...");
      if (system("systemctl stop traced traced-probes") == 0) {
        printf("Daemons stopped.\n");
        return 0;
      }
      PERFETTO_ELOG("Failed to stop systemd services");
      return 1;
    }
    printf("Started manually or by other means. Please stop them directly.\n");
    return 0;
  }
  if (!all_stopped) {
    PERFETTO_ELOG("Some daemons could not be stopped.");
    return 1;
  }
  printf("Daemons stopped.\n");
  return 0;
#endif
}

int CtlStart() {
  ServiceSockets sockets = GetRunningSockets();
  if (sockets.IsValid()) {
    printf("Status: Daemons are already running with %s",
           sockets.ToString().c_str());
    return 0;
  }

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  PERFETTO_LOG("Starting daemons...");
  std::string tracebox_path = base::GetCurExecutablePath();
  std::vector<base::Subprocess> processes;
  for (const char* daemon : kDaemons) {
    auto proc = base::Subprocess({tracebox_path, daemon});
    proc.Start();
    processes.push_back(std::move(proc));
  }

  // Wait for daemons to start
  base::SleepMicroseconds(100 * 1000);

  ServiceSockets started_sockets = GetRunningSockets();
  if (!started_sockets.IsValid()) {
    PERFETTO_ELOG(
        "Failed to start daemons. Possible causes:\n"
        "  - Ports 32278/32279 may already be in use\n"
        "  - Firewall may be blocking the connections\n"
        "  - Insufficient permissions");
    return 1;
  }
  SetServiceSocketEnv(started_sockets);
  printf("Success: Daemons started with %s\nPress Ctrl+C to stop.",
         started_sockets.ToString().c_str());

  while (true)
    base::SleepMicroseconds(1000000);
#else  // Unix
  if (IsSystemdServiceInstalled()) {
    if (base::GetCurrentUserId() == 0) {
      PERFETTO_LOG("Starting daemons via systemd...");
      if (system("systemctl start traced traced-probes") == 0) {
        ServiceSockets sys_sockets = GetRunningSockets();
        SetServiceSocketEnv(sys_sockets);
        printf("Success: Daemons started via systemd\n");
        return 0;
      }
      return 1;
    }
    PERFETTO_ELOG(
        "Systemd service installed but requires root.\nUse: sudo "
        "systemctl start traced traced-probes");
    return 1;
  }
  PERFETTO_LOG("Starting daemons...");
  std::string tracebox_path = base::GetCurExecutablePath();
  for (const char* daemon : kDaemons) {
    pid_t pid = StartDaemon(tracebox_path, daemon);
    if (pid <= 0) {
      PERFETTO_ELOG("Failed to start %s daemon", daemon);
      CtlStop();  // Cleanup
      return 1;
    }
    std::string pid_path = GetPidFilePath(daemon);
    if (!WritePidToFile(pid_path, pid)) {
      PERFETTO_ELOG("Failed to write PID file for %s", daemon);
      kill(pid, SIGTERM);
      CtlStop();
      return 1;
    }
  }
  ServiceSockets started_sockets = GetRunningSockets();
  if (started_sockets.IsValid()) {
    SetServiceSocketEnv(started_sockets);
    printf("Success: Daemons started with %s\n",
           started_sockets.ToString().c_str());
    return 0;
  }
  printf("Failed to start daemons. Invalid sockets: %s",
         started_sockets.ToString().c_str());
  return 1;
#endif
}

int CtlStatus() {
  ServiceSockets sockets = GetRunningSockets();
  if (sockets.IsValid()) {
    printf("Success: Daemons are running and accessible with %s",
           sockets.ToString().c_str());
  } else {
    printf("Status: No daemons detected via sockets\n");
  }
#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  bool stale_found = false;
  for (const char* daemon : kDaemons) {
    std::string pid_path = GetPidFilePath(daemon);
    pid_t pid = ReadPidFromFile(pid_path);
    if (pid > 0) {
      if (kill(pid, 0) == 0) {
        printf("  %s : Running (PID %d)\n", daemon, static_cast<int>(pid));
      } else {
        printf("  %s : Not running (Stale PID file %d)\n", daemon,
               static_cast<int>(pid));
        stale_found = true;
      }
    }
  }
  if (stale_found) {
    printf(
        "\nStale PID files found. Run 'tracebox ctl stop' to clean them up.\n");
  }
#endif
  return sockets.IsValid();
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

  if (CanConnectToSocket(sockets.consumer_socket)) {
    return sockets;
  }

  PERFETTO_ELOG("Failed to connect to %s%s", sockets.consumer_socket.c_str(),
                getenv(kPerfettoConsumerSockEnv)
                    ? " (configured via PERFETTO_CONSUMER_SOCK_NAME)"
                    : "");

  return ServiceSockets{};
}

void SetServiceSocketEnv(const ServiceSockets& sockets) {
  base::SetEnv(kPerfettoProducerSockEnv, sockets.producer_socket);
  base::SetEnv(kPerfettoConsumerSockEnv, sockets.consumer_socket);
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
