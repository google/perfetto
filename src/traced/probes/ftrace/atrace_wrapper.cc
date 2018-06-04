/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/traced/probes/ftrace/atrace_wrapper.h"

#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "perfetto/base/logging.h"

namespace perfetto {

namespace {

RunAtraceFunction g_run_atrace_for_testing = nullptr;

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
// Args should include "atrace" for argv[0].
bool ExecvAtrace(const std::vector<std::string>& args) {
  int status = 1;

  std::vector<char*> argv;
  // args, and then a null.
  argv.reserve(1 + args.size());
  for (const auto& arg : args)
    argv.push_back(const_cast<char*>(arg.c_str()));
  argv.push_back(nullptr);

  pid_t pid = fork();
  PERFETTO_CHECK(pid >= 0);
  if (pid == 0) {
    // Close stdin/out/err + any file descriptor that we might have mistakenly
    // not marked as FD_CLOEXEC.
    for (int i = 0; i < 128; i++)
      close(i);
    execv("/system/bin/atrace", &argv[0]);
    // Reached only if execv fails.
    _exit(1);
  }
  PERFETTO_EINTR(waitpid(pid, &status, 0));
  return status == 0;
}
#endif

}  // namespace

bool RunAtrace(const std::vector<std::string>& args) {
  if (g_run_atrace_for_testing)
    return g_run_atrace_for_testing(args);
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  return ExecvAtrace(args);
#else
  PERFETTO_LOG("Atrace only supported on Android.");
  return false;
#endif
}

void SetRunAtraceForTesting(RunAtraceFunction f) {
  g_run_atrace_for_testing = f;
}

}  // namespace perfetto
