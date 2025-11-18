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

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
namespace perfetto {

ServiceSockets GetServiceSockets() {
  return ServiceSockets{};
}

void SetServiceSocketEnv(const ServiceSockets& sockets) {
  // No-op on Windows
}

void PrintTraceboxCtlUsage() {
  PERFETTO_FATAL("'tracebox ctl' not supported on Windows");
}

int TraceboxCtlMain(int, char**) {
  PERFETTO_FATAL("'tracebox ctl' not supported on Windows");
  return 1;
}

}  // namespace perfetto

#else  // !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/pipe.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/unix_socket.h"
#include "perfetto/ext/base/utils.h"

namespace perfetto {
namespace {

constexpr char kTmpDir[] = "/tmp";
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
constexpr char kAndroidDevSocketDir[] = "/dev/socket";
#else
constexpr char kRunPerfettoDir[] = "/run/perfetto";
#endif

constexpr char kTracedBinary[] = "traced";
constexpr char kTracedProbesBinary[] = "traced_probes";
#if PERFETTO_BUILDFLAG(PERFETTO_TRACED_PERF)
constexpr char kTracedPerfBinary[] = "traced_perf";
#endif

// Runtime paths for daemon management (PID files and sockets).
struct RuntimePaths {
  std::string traced_pid;
  std::string traced_probes_pid;
#if PERFETTO_BUILDFLAG(PERFETTO_TRACED_PERF)
  std::string traced_perf_pid;
#endif
  std::string producer_sock;
  std::string consumer_sock;
};

bool WriteFile(const std::string& path, const std::string& content) {
  base::ScopedFile fd =
      base::OpenFile(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (!fd)
    return false;
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

bool CanConnectToSocket(const std::string& path) {
  auto sock = base::UnixSocketRaw::CreateMayFail(base::SockFamily::kUnix,
                                                 base::SockType::kStream);
  return sock && sock.Connect(path);
}

bool IsSystemdServiceInstalled() {
#if PERFETTO_BUILDFLAG(PERFETTO_SYSTEMD)
  constexpr const char* kSystemdServices[] = {
      "/etc/systemd/system/traced.service",
      "/lib/systemd/system/traced.service",
      "/usr/lib/systemd/system/traced.service",
  };
  struct stat st;
  for (const char* path : kSystemdServices) {
    if (stat(path, &st) == 0)
      return true;
  }
#endif
  return false;
}

// Returns the best available runtime directory for sockets and PID files.
// On Android: Use /tmp for user-started daemons.
// On Linux: /run/perfetto if writable (or creatable), otherwise /tmp.
std::string GetRuntimeDir() {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  // System daemons in /dev/socket are detected separately via
  // GetServiceSockets().
  return kTmpDir;
#else
  // On Linux/macOS, prefer /run/perfetto (create if needed).
  struct stat st;
  if (stat(kRunPerfettoDir, &st) == 0) {
    if (access(kRunPerfettoDir, W_OK) == 0)
      return kRunPerfettoDir;
  } else if (mkdir(kRunPerfettoDir, 0755) == 0) {
    return kRunPerfettoDir;
  }
  return kTmpDir;
#endif
}

RuntimePaths GetPaths(const std::string& run_dir) {
  RuntimePaths paths;
  paths.traced_pid = run_dir + "/" + kTracedBinary + ".pid";
  paths.traced_probes_pid = run_dir + "/" + kTracedProbesBinary + ".pid";
#if PERFETTO_BUILDFLAG(PERFETTO_TRACED_PERF)
  paths.traced_perf_pid = run_dir + "/" + kTracedPerfBinary + ".pid";
#endif

  // Socket naming conventions:
  // - Android /dev/socket: traced_producer, traced_consumer
  // - /run/perfetto: traced-producer.sock, traced-consumer.sock
  // - /tmp: perfetto-producer, perfetto-consumer
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  if (run_dir == kAndroidDevSocketDir) {
    paths.producer_sock = run_dir + "/traced_producer";
    paths.consumer_sock = run_dir + "/traced_consumer";
  } else {
    paths.producer_sock = run_dir + "/perfetto-producer";
    paths.consumer_sock = run_dir + "/perfetto-consumer";
  }
#else
  if (run_dir == kRunPerfettoDir) {
    paths.producer_sock = run_dir + "/traced-producer.sock";
    paths.consumer_sock = run_dir + "/traced-consumer.sock";
  } else {
    paths.producer_sock = run_dir + "/perfetto-producer";
    paths.consumer_sock = run_dir + "/perfetto-consumer";
  }
#endif
  return paths;
}

bool CheckProcessRunning(pid_t pid, const char* name) {
  if (pid <= 0)
    return false;
  // kill(pid, 0) checks process existence without sending a signal.
  if (kill(pid, 0) != 0) {
    if (errno == EPERM) {
      PERFETTO_LOG("%s is running with PID %d (permission denied)", name,
                   static_cast<int>(pid));
      return true;
    }
    return false;
  }
  PERFETTO_LOG("%s is running with PID %d", name, static_cast<int>(pid));
  return true;
}

bool StopDaemon(const std::string& pid_file) {
  pid_t pid = ReadPidFromFile(pid_file);
  if (pid == 0) {
    base::ignore_result(remove(pid_file.c_str()));
    return true;
  }

  // Check if process exists before attempting to stop it.
  if (kill(pid, 0) != 0) {
    if (errno == EPERM) {
      PERFETTO_ELOG("Cannot stop daemon (pid=%d): Permission denied",
                    static_cast<int>(pid));
      return false;
    }
    // ESRCH: process doesn't exist, clean up stale PID file.
    if (errno == ESRCH) {
      base::ignore_result(remove(pid_file.c_str()));
      return true;
    }
    PERFETTO_ELOG("Error checking daemon (pid=%d): %s", static_cast<int>(pid),
                  strerror(errno));
    return false;
  }

  if (kill(pid, SIGTERM) != 0) {
    PERFETTO_ELOG("Failed to stop daemon (pid=%d): %s", static_cast<int>(pid),
                  strerror(errno));
    return false;
  }

  base::ignore_result(remove(pid_file.c_str()));
  return true;
}

// Double-fork daemonization to fully detach process from parent and terminal.
// Returns grandchild PID (the actual daemon) or -1 on error.
// The intermediate child exits immediately, ensuring the daemon is reparented
// to init and has no controlling terminal.
// If log is non-empty, redirects stdout and stderr to that file.
pid_t SpawnDaemonWithLogging(const std::string& binary_path,
                             const std::vector<std::string>& args,
                             const std::string& log_file_path = "") {
  base::Pipe pipe = base::Pipe::Create(base::Pipe::kBothBlock);
  pid_t pid;
  switch (pid = fork()) {
    case -1:
      return -1;
    case 0: {
      // Intermediate child: create new session and fork again.
      pipe.rd.reset();
      PERFETTO_CHECK(setsid() != -1);  // Detach from controlling terminal.
      base::ignore_result(chdir("/"));

      pid_t pid2 = fork();
      if (pid2 < 0)
        _exit(1);

      if (pid2 > 0) {
        // Intermediate child: send grandchild PID to parent and exit.
        // This ensures the grandchild is reparented to init.
        base::WriteAll(*pipe.wr, &pid2, sizeof(pid2));
        _exit(0);
      }

      // Grandchild: the actual daemon process.
      pipe.wr.reset();

      // Redirect stdin to /dev/null always.
      base::ScopedFile null = base::OpenFile("/dev/null", O_RDWR);
      PERFETTO_CHECK(null);
      PERFETTO_CHECK(dup2(*null, STDIN_FILENO) != -1);

      // Redirect stdout/stderr to log file or /dev/null.
      auto redirect_fd = [&null](int fd, const std::string& log_file_path) {
        if (log_file_path.empty()) {
          PERFETTO_CHECK(dup2(*null, fd) != -1);
          return;
        }
        base::ScopedFile log_file =
            base::OpenFile(log_file_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (log_file) {
          PERFETTO_CHECK(dup2(*log_file, fd) != -1);
        } else {
          PERFETTO_ELOG("Failed to open log file %s", log_file_path.c_str());
          PERFETTO_CHECK(dup2(*null, fd) != -1);
        }
      };

      // Both stdout and stderr go to the same log file for ease of use.
      redirect_fd(STDOUT_FILENO, log_file_path);
      redirect_fd(STDERR_FILENO, log_file_path);

      // Avoid closing stdio FDs if files happened to use them.
      if (*null <= 2)
        null.release();

      std::vector<char*> argv_ptrs;
      argv_ptrs.push_back(const_cast<char*>(binary_path.c_str()));
      for (const auto& arg : args)
        argv_ptrs.push_back(const_cast<char*>(arg.c_str()));
      argv_ptrs.push_back(nullptr);

      execv(binary_path.c_str(), argv_ptrs.data());
      _exit(127);  // execv failed (127 = command not found convention).
    }
    default: {
      // Parent: read grandchild PID from pipe and reap intermediate child.
      pipe.wr.reset();
      pid_t grandchild_pid = -1;
      ssize_t res =
          base::Read(*pipe.rd, &grandchild_pid, sizeof(grandchild_pid));
      if (res != sizeof(grandchild_pid))
        grandchild_pid = -1;
      int status;
      waitpid(pid, &status, 0);  // Reap intermediate child.
      return grandchild_pid;
    }
  }
}

bool StartDaemon(const std::string& binary_path,
                 const std::string& daemon_name,
                 const std::string& pid_file_path,
                 const std::string& log_file_path = "") {
  std::vector<std::string> args = {daemon_name};
  pid_t pid = SpawnDaemonWithLogging(binary_path, args, log_file_path);
  if (pid <= 0) {
    PERFETTO_ELOG("Failed to start %s", daemon_name.c_str());
    return false;
  }
  if (!WriteFile(pid_file_path, std::to_string(pid))) {
    PERFETTO_ELOG("Failed to write PID file for %s", daemon_name.c_str());
    kill(pid, SIGTERM);
    return false;
  }
  return true;
}

int CtlStart(bool enable_logging) {
  ServiceSockets sockets = GetServiceSockets();
  if (sockets.IsValid()) {
    printf("Perfetto daemons are already running.\n");
    SetServiceSocketEnv(sockets);
    return 0;
  }

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  // On Android, we expect system daemons to be present by default.
  PERFETTO_LOG(
      "System daemons not found. Starting user-session daemons in /tmp.");
  PERFETTO_LOG(
      "Note: SDK-instrumented apps require system daemons and will not "
      "connect to user-session daemons.");
#endif

  if (IsSystemdServiceInstalled()) {
    if (getuid() == 0) {
      printf("Systemd service found. Starting via systemctl...\n");
      if (system("systemctl start traced traced-probes") == 0) {
        // After starting via systemd, discover and set the socket paths
        ServiceSockets systemd_sockets = GetServiceSockets();
        SetServiceSocketEnv(systemd_sockets);
        return 0;
      }
      PERFETTO_ELOG("Failed to start systemd service");
    } else {
      PERFETTO_ELOG(
          "Systemd service installed. Use: sudo systemctl start traced "
          "traced-probes");
    }
    return 1;
  }

  std::string run_dir = GetRuntimeDir();
  if (run_dir == kTmpDir) {
    PERFETTO_ELOG("Warning: Using %s sockets. SDK apps may fail to connect.",
                  kTmpDir);
  }

  RuntimePaths paths = GetPaths(run_dir);
  PERFETTO_LOG("Starting daemons in %s...", run_dir.c_str());
  std::string tracebox_bin = base::GetCurExecutablePath();

  // Set socket paths in environment to guarantee future tracebox commands
  // (including spawned daemons) use the correct socket locations.
  SetServiceSocketEnv({paths.producer_sock, paths.consumer_sock});

  std::string traced_log = enable_logging ? run_dir + "/traced.log" : "";
  std::string traced_probes_log =
      enable_logging ? run_dir + "/traced_probes.log" : "";

  if (!StartDaemon(tracebox_bin, kTracedBinary, paths.traced_pid, traced_log))
    return 1;

  if (!StartDaemon(tracebox_bin, kTracedProbesBinary, paths.traced_probes_pid,
                   traced_probes_log)) {
    StopDaemon(paths.traced_pid);
    return 1;
  }

#if PERFETTO_BUILDFLAG(PERFETTO_TRACED_PERF)
  std::string traced_perf_log =
      enable_logging ? run_dir + "/traced_perf.log" : "";
  if (!StartDaemon(tracebox_bin, kTracedPerfBinary, paths.traced_perf_pid,
                   traced_perf_log)) {
    StopDaemon(paths.traced_pid);
    StopDaemon(paths.traced_probes_pid);
    return 1;
  }
#endif

  printf(R"(
Daemons started in: %s
Producer socket: %s
Consumer socket: %s
)",
         run_dir.c_str(), paths.producer_sock.c_str(),
         paths.consumer_sock.c_str());

  if (enable_logging) {
    printf("Logs: %s/traced.log, %s/traced_probes.log\n", run_dir.c_str(),
           run_dir.c_str());
#if PERFETTO_BUILDFLAG(PERFETTO_TRACED_PERF)
    printf("      %s/traced_perf.log\n", run_dir.c_str());
#endif
  }

  printf("\nEnvironment variables set for this session.\n");
  return 0;
}

int CtlStop() {
  printf("Stopping daemons...\n");
  bool found_any = false;
  bool all_stopped = true;

  auto try_stop = [&](const std::string& pid_path) {
    if (base::FileExists(pid_path)) {
      found_any = true;
      if (!StopDaemon(pid_path))
        all_stopped = false;
    }
  };

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  const char* search_dirs[] = {kAndroidDevSocketDir, kTmpDir};
#else
  const char* search_dirs[] = {kRunPerfettoDir, kTmpDir};
#endif
  for (const char* dir : search_dirs) {
    if (access(dir, R_OK) != 0)
      continue;
    RuntimePaths paths = GetPaths(std::string(dir));
    try_stop(paths.traced_pid);
    try_stop(paths.traced_probes_pid);
#if PERFETTO_BUILDFLAG(PERFETTO_TRACED_PERF)
    try_stop(paths.traced_perf_pid);
#endif
  }

  if (!found_any) {
    printf("No daemon PID files found (not started via 'ctl start').\n");
    ServiceSockets sockets = GetServiceSockets();
    if (!sockets.IsValid()) {
      printf("No daemons detected.\n");
      return 0;
    }

    printf(
        "However, daemons are running (detected via socket connectivity).\n");
    printf("  Producer socket: %s\n", sockets.producer_socket.c_str());
    printf("  Consumer socket: %s\n", sockets.consumer_socket.c_str());

    if (!IsSystemdServiceInstalled()) {
      printf("Started manually or by other means. Stop them directly.\n");
      return 0;
    }

    if (getuid() != 0) {
      printf(
          "Managed by systemd. Use: sudo systemctl stop traced "
          "traced-probes\n");
      return 0;
    }

    printf("Systemd service found. Stopping via systemctl...\n");
    if (system("systemctl stop traced traced-probes") == 0) {
      printf("Daemons stopped.\n");
      return 0;
    }
    PERFETTO_ELOG("Failed to stop systemd services");
    return 1;
  }

  if (!all_stopped) {
    printf("Error: Some daemons could not be stopped.\n");
    return 1;
  }

  printf("Daemons stopped.\n");
  return 0;
}

int CtlStatus() {
  RuntimePaths paths;
  bool found = false;
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  const char* search_dirs[] = {kAndroidDevSocketDir, kTmpDir};
#else
  const char* search_dirs[] = {kRunPerfettoDir, kTmpDir};
#endif
  for (const char* dir : search_dirs) {
    RuntimePaths p = GetPaths(dir);
    if (base::FileExists(p.traced_pid)) {
      paths = p;
      found = true;
      break;
    }
  }

  if (!found) {
    printf("No daemon PID files found.\n");
    ServiceSockets sockets = GetServiceSockets();
    if (!sockets.IsValid())
      return 1;

    // Show which sockets are accessible to help the user.
    printf("However, daemons are accessible via:\n");
    printf("  Producer socket: %s\n", sockets.producer_socket.c_str());
    printf("  Consumer socket: %s\n", sockets.consumer_socket.c_str());
    return 1;
  }

  pid_t traced_pid = ReadPidFromFile(paths.traced_pid);
  pid_t probes_pid = ReadPidFromFile(paths.traced_probes_pid);

  bool traced_running = CheckProcessRunning(traced_pid, "traced");
  bool probes_running = CheckProcessRunning(probes_pid, "traced_probes");

#if PERFETTO_BUILDFLAG(PERFETTO_TRACED_PERF)
  pid_t perf_pid = ReadPidFromFile(paths.traced_perf_pid);
  bool perf_running = CheckProcessRunning(perf_pid, "traced_perf");
#endif

  // Clean up stale PID files (process recorded in file but no longer running).
  if (traced_pid > 0 && !traced_running) {
    printf("Removing stale PID file: %s\n", paths.traced_pid.c_str());
    remove(paths.traced_pid.c_str());
  }
  if (probes_pid > 0 && !probes_running) {
    printf("Removing stale PID file: %s\n", paths.traced_probes_pid.c_str());
    remove(paths.traced_probes_pid.c_str());
  }
#if PERFETTO_BUILDFLAG(PERFETTO_TRACED_PERF)
  if (perf_pid > 0 && !perf_running) {
    printf("Removing stale PID file: %s\n", paths.traced_perf_pid.c_str());
    remove(paths.traced_perf_pid.c_str());
  }
#endif

  if (!traced_running && !probes_running) {
    printf("Daemons are not running.\n");
    return 1;
  }

  // Verify socket connectivity (ensures daemons are actually functional).
  if (!CanConnectToSocket(paths.producer_sock)) {
    printf(
        "Warning: Daemons running but producer socket not accessible at %s\n",
        paths.producer_sock.c_str());
    return 1;
  }
  printf(R"(Producer socket: %s
Consumer socket: %s
Daemons are running and accessible.
)",
         paths.producer_sock.c_str(), paths.consumer_sock.c_str());
  return 0;
}

}  // namespace

void PrintTraceboxCtlUsage() {
  printf(R"(
tracebox ctl [start|stop|status] [OPTIONS]
Manages the lifecycle of Perfetto daemons (traced, traced_probes).

Commands:
  start [--log]: Starts daemons for the current user session.
                 --log: Enable logging to traced.log and traced_probes.log
  stop: Stops user-session daemons.
  status: Shows the status of user-session daemons.

Note: traced_probes does not automatically reset ftrace state.
If needed, run: traced_probes --reset-ftrace (requires root)
)");
}

// Checks if daemons are accessible and returns their socket paths.
// Search order: env var, Android system sockets, /run/perfetto, /tmp.
ServiceSockets GetServiceSockets() {
  // Check env var first.
  if (const char* env = getenv(kPerfettoProducerSockEnv); env) {
    if (CanConnectToSocket(env)) {
      const char* consumer_env = getenv(kPerfettoConsumerSockEnv);
      return ServiceSockets{env, consumer_env ? consumer_env : ""};
    }
  }

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  // On Android, check for system sockets first.
  constexpr char kAndroidProducerSock[] = "/dev/socket/traced_producer";
  constexpr char kAndroidConsumerSock[] = "/dev/socket/traced_consumer";
  if (CanConnectToSocket(kAndroidProducerSock)) {
    return ServiceSockets{kAndroidProducerSock, kAndroidConsumerSock};
  }
#endif

  // Check standard directories in order.
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  const char* search_dirs[] = {kAndroidDevSocketDir, kTmpDir};
#else
  const char* search_dirs[] = {kRunPerfettoDir, kTmpDir};
#endif
  for (const char* dir : search_dirs) {
    RuntimePaths paths = GetPaths(dir);
    if (CanConnectToSocket(paths.producer_sock)) {
      return ServiceSockets{paths.producer_sock, paths.consumer_sock};
    }
  }
  return ServiceSockets{};
}

// Sets the environment variables for daemon socket paths.
void SetServiceSocketEnv(const ServiceSockets& sockets) {
  if (sockets.IsValid()) {
    base::SetEnv(kPerfettoProducerSockEnv, sockets.producer_socket);
    base::SetEnv(kPerfettoConsumerSockEnv, sockets.consumer_socket);
  }
}

int TraceboxCtlMain(int argc, char** argv) {
  if (argc < 2) {
    PrintTraceboxCtlUsage();
    return 1;
  }

  const char* cmd = argv[1];
  if (strcmp(cmd, "start") == 0) {
    bool enable_logging = false;
    // Check for --log flag.
    for (int i = 2; i < argc; ++i) {
      if (strcmp(argv[i], "--log") == 0) {
        enable_logging = true;
        break;
      }
    }
    return CtlStart(enable_logging);
  }
  if (strcmp(cmd, "stop") == 0)
    return CtlStop();
  if (strcmp(cmd, "status") == 0)
    return CtlStatus();

  PrintTraceboxCtlUsage();
  return 1;
}
}  // namespace perfetto

#endif  // !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
