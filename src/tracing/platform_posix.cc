/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "perfetto/base/build_config.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) ||   \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/thread_task_runner.h"
#include "perfetto/tracing/internal/tracing_tls.h"
#include "perfetto/tracing/platform.h"
#include "perfetto/tracing/trace_writer_base.h"

#include <pthread.h>
#include <stdlib.h>

namespace perfetto {

namespace {

class PlatformPosix : public Platform {
 public:
  PlatformPosix();
  ~PlatformPosix() override;

  ThreadLocalObject* GetOrCreateThreadLocalObject() override;
  std::unique_ptr<base::TaskRunner> CreateTaskRunner(
      const CreateTaskRunnerArgs&) override;
  std::string GetCurrentProcessName() override;

 private:
  pthread_key_t tls_key_{};
};

using ThreadLocalObject = Platform::ThreadLocalObject;

PlatformPosix::PlatformPosix() {
  auto tls_dtor = [](void* obj) {
    delete static_cast<ThreadLocalObject*>(obj);
  };
  PERFETTO_CHECK(pthread_key_create(&tls_key_, tls_dtor) == 0);
}

PlatformPosix::~PlatformPosix() {
  pthread_key_delete(tls_key_);
}

ThreadLocalObject* PlatformPosix::GetOrCreateThreadLocalObject() {
  // In chromium this should be implemented using base::ThreadLocalStorage.
  auto tls = static_cast<ThreadLocalObject*>(pthread_getspecific(tls_key_));
  if (!tls) {
    tls = ThreadLocalObject::CreateInstance().release();
    pthread_setspecific(tls_key_, tls);
  }
  return tls;
}

std::unique_ptr<base::TaskRunner> PlatformPosix::CreateTaskRunner(
    const CreateTaskRunnerArgs&) {
  return std::unique_ptr<base::TaskRunner>(
      new base::ThreadTaskRunner(base::ThreadTaskRunner::CreateAndStart()));
}

std::string PlatformPosix::GetCurrentProcessName() {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  std::string cmdline;
  base::ReadFile("/proc/self/cmdline", &cmdline);
  return cmdline.substr(0, cmdline.find('\0'));
#elif PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
  return std::string(getprogname());
#else
  return "unknown_producer";
#endif
}

}  // namespace

// static
Platform* Platform::GetDefaultPlatform() {
  static PlatformPosix* instance = new PlatformPosix();
  return instance;
}

}  // namespace perfetto
#endif  // OS_LINUX || OS_ANDROID || OS_APPLE
