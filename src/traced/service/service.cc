/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include <getopt.h>
#include <stdio.h>
#include <algorithm>

#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/unix_task_runner.h"
#include "perfetto/ext/base/watchdog.h"
#include "perfetto/ext/traced/traced.h"
#include "perfetto/ext/tracing/ipc/default_socket.h"
#include "perfetto/ext/tracing/ipc/service_ipc_host.h"
#include "src/traced/service/builtin_producer.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#define PERFETTO_SET_SOCKET_PERMISSIONS
#include <fcntl.h>
#include <grp.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#if PERFETTO_BUILDFLAG(PERFETTO_VERSION_GEN)
#include "perfetto_version.gen.h"
#else
#define PERFETTO_GET_GIT_REVISION() "unknown"
#endif

namespace perfetto {
namespace {
#if defined(PERFETTO_SET_SOCKET_PERMISSIONS)
void SetSocketPermissions(const std::string& socket_name,
                          const std::string& group_name,
                          const std::string& mode_bits) {
  PERFETTO_CHECK(!socket_name.empty());
  PERFETTO_CHECK(!group_name.empty());
  struct group* socket_group = nullptr;
  // Query the group ID of |group|.
  do {
    socket_group = getgrnam(group_name.c_str());
  } while (socket_group == nullptr && errno == EINTR);
  if (socket_group == nullptr) {
    PERFETTO_FATAL("Failed to get group information of %s ",
                   group_name.c_str());
  }

  if (PERFETTO_EINTR(
          chown(socket_name.c_str(), geteuid(), socket_group->gr_gid))) {
    PERFETTO_FATAL("Failed to chown %s ", socket_name.c_str());
  }

  // |mode| accepts values like "0660" as "rw-rw----" mode bits.
  auto mode_value = base::StringToInt32(mode_bits, 8);
  if (!(mode_bits.size() == 4 && mode_value.has_value())) {
    PERFETTO_FATAL(
        "The chmod option must be a 4-digit octal number, e.g. 0660");
  }
  if (PERFETTO_EINTR(chmod(socket_name.c_str(),
                           static_cast<mode_t>(mode_value.value())))) {
    PERFETTO_FATAL("Failed to chmod %s", socket_name.c_str());
  }
}
#endif  // defined(PERFETTO_SET_SOCKET_PERMISSIONS)

void PrintUsage(const char* prog_name) {
  PERFETTO_ELOG(R"(
Usage: %s [option] ...
Options and arguments
    --version : print the version number and exit.
    --set-socket-permissions <permissions> : sets group ownership and permission
        mode bits of the producer and consumer sockets.
        <permissions> format: <prod_group>:<prod_mode>:<cons_group>:<cons_mode>,
        where <prod_group> is the group name for chgrp the producer socket,
        <prod_mode> is the mode bits (e.g. 0660) for chmod the produce socket,
        <cons_group> is the group name for chgrp the consumer socket, and
        <cons_mode> is the mode bits (e.g. 0660) for chmod the consumer socket.
Example: %s --set-socket-permissions traced-producer:0660:traced-consumer:0660
    starts the service and sets the group ownership of the producer and consumer
    sockets to "traced-producer" and "traced-consumer", respectively. Both
    producer and consumer sockets are chmod with 0660  (rw-rw----) mode bits.
)",
                prog_name, prog_name);
}
}  // namespace

int __attribute__((visibility("default"))) ServiceMain(int argc, char** argv) {
  enum LongOption {
    OPT_VERSION = 1000,
    OPT_SET_SOCKET_PERMISSIONS = 1001,
  };

  static const struct option long_options[] = {
      {"version", no_argument, nullptr, OPT_VERSION},
      {"set-socket-permissions", required_argument, nullptr,
       OPT_SET_SOCKET_PERMISSIONS},
      {nullptr, 0, nullptr, 0}};

  std::string producer_socket_group, consumer_socket_group,
      producer_socket_mode, consumer_socket_mode;

  int option_index;
  for (;;) {
    int option = getopt_long(argc, argv, "", long_options, &option_index);
    if (option == -1)
      break;
    switch (option) {
      case OPT_VERSION:
        printf("%s\n", PERFETTO_GET_GIT_REVISION());
        return 0;
      case OPT_SET_SOCKET_PERMISSIONS: {
        // Check that the socket permission argument is well formed.
        auto parts = base::SplitString(std::string(optarg), ":");
        PERFETTO_CHECK(parts.size() == 4);
        PERFETTO_CHECK(
            std::all_of(parts.cbegin(), parts.cend(),
                        [](const std::string& part) { return !part.empty(); }));
        producer_socket_group = parts[0];
        producer_socket_mode = parts[1];
        consumer_socket_group = parts[2];
        consumer_socket_mode = parts[3];
        break;
      }
      default:
        PrintUsage(argv[0]);
        return 1;
    }
  }

  base::UnixTaskRunner task_runner;
  std::unique_ptr<ServiceIPCHost> svc;
  svc = ServiceIPCHost::CreateInstance(&task_runner);

  // When built as part of the Android tree, the two socket are created and
  // bound by init and their fd number is passed in two env variables.
  // See libcutils' android_get_control_socket().
  const char* env_prod = getenv("ANDROID_SOCKET_traced_producer");
  const char* env_cons = getenv("ANDROID_SOCKET_traced_consumer");
  PERFETTO_CHECK((!env_prod && !env_cons) || (env_prod && env_cons));
  bool started;
  if (env_prod) {
    base::ScopedFile producer_fd(atoi(env_prod));
    base::ScopedFile consumer_fd(atoi(env_cons));
    started = svc->Start(std::move(producer_fd), std::move(consumer_fd));
  } else {
    unlink(GetProducerSocket());
    unlink(GetConsumerSocket());
    started = svc->Start(GetProducerSocket(), GetConsumerSocket());

    if (!producer_socket_group.empty()) {
#if defined(PERFETTO_SET_SOCKET_PERMISSIONS)
      SetSocketPermissions(GetProducerSocket(), producer_socket_group,
                           producer_socket_mode);
      SetSocketPermissions(GetConsumerSocket(), consumer_socket_group,
                           consumer_socket_mode);
#else
      PERFETTO_ELOG(
          "Setting socket permissions is not supported on this platform");
      return 1;
#endif
    }
  }

  if (!started) {
    PERFETTO_ELOG("Failed to start the traced service");
    return 1;
  }

  BuiltinProducer builtin_producer(&task_runner, /*lazy_stop_delay_ms=*/30000);
  builtin_producer.ConnectInProcess(svc->service());

  // Set the CPU limit and start the watchdog running. The memory limit will
  // be set inside the service code as it relies on the size of buffers.
  // The CPU limit is the generic one defined in watchdog.h.
  base::Watchdog* watchdog = base::Watchdog::GetInstance();
  watchdog->SetCpuLimit(base::kWatchdogDefaultCpuLimit,
                        base::kWatchdogDefaultCpuWindow);
  watchdog->Start();

  PERFETTO_ILOG("Started traced, listening on %s %s", GetProducerSocket(),
                GetConsumerSocket());
  task_runner.Run();
  return 0;
}

}  // namespace perfetto
