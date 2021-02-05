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

#include "src/tracing/test/api_test_support.h"

#include "perfetto/base/proc_utils.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/temp_file.h"
#include "src/tracing/internal/tracing_muxer_impl.h"

#include <sstream>

#if PERFETTO_BUILDFLAG(PERFETTO_IPC)
#include "test/test_helper.h"
#endif

namespace perfetto {
namespace test {

#if PERFETTO_BUILDFLAG(PERFETTO_IPC)
namespace {

class InProcessSystemService {
 public:
  InProcessSystemService()
      : test_helper_(&task_runner_, TestHelper::Mode::kStartDaemons) {
    // Will always start service because we explicitly set kStartDaemons.
    test_helper_.StartServiceIfRequired();
  }

 private:
  perfetto::base::TestTaskRunner task_runner_;
  perfetto::TestHelper test_helper_;
};

}  // namespace

bool StartSystemService() {
  static InProcessSystemService* system_service;

  // If there already was a system service running, make sure the new one is
  // running before tearing down the old one. This avoids a 1 second
  // reconnection delay between each test since the connection to the new
  // service succeeds immediately.
  std::unique_ptr<InProcessSystemService> old_service(system_service);
  system_service = new InProcessSystemService();

  // Tear down the service at process exit to make sure temporary files get
  // deleted.
  static bool cleanup_registered;
  if (!cleanup_registered) {
    atexit([] { delete system_service; });
    cleanup_registered = true;
  }
  return true;
}
#else   // !PERFETTO_BUILDFLAG(PERFETTO_IPC)
bool StartSystemService() {
  return false;
}
#endif  // !PERFETTO_BUILDFLAG(PERFETTO_IPC)

int32_t GetCurrentProcessId() {
  return static_cast<int32_t>(base::GetProcessId());
}

void SyncProducers() {
  auto* muxer = reinterpret_cast<perfetto::internal::TracingMuxerImpl*>(
      perfetto::internal::TracingMuxer::Get());
  muxer->SyncProducersForTesting();
}

void SetBatchCommitsDuration(uint32_t batch_commits_duration_ms,
                             BackendType backend_type) {
  auto* muxer = reinterpret_cast<perfetto::internal::TracingMuxerImpl*>(
      perfetto::internal::TracingMuxer::Get());
  muxer->SetBatchCommitsDurationForTesting(batch_commits_duration_ms,
                                           backend_type);
}

void DisableReconnectLimit() {
  auto* muxer = reinterpret_cast<perfetto::internal::TracingMuxerImpl*>(
      perfetto::internal::TracingMuxer::Get());
  muxer->SetMaxProducerReconnectionsForTesting(
      std::numeric_limits<uint32_t>::max());
}

bool EnableDirectSMBPatching(BackendType backend_type) {
  auto* muxer = reinterpret_cast<perfetto::internal::TracingMuxerImpl*>(
      perfetto::internal::TracingMuxer::Get());
  return muxer->EnableDirectSMBPatchingForTesting(backend_type);
}

TestTempFile CreateTempFile() {
  TestTempFile res{};
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  char temp_file[255]{};
  sprintf(temp_file, "%s\\perfetto-XXXXXX", getenv("TMP"));
  PERFETTO_CHECK(_mktemp_s(temp_file, strlen(temp_file) + 1) == 0);
  HANDLE handle =
      ::CreateFileA(temp_file, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_DELETE | FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_TEMPORARY, nullptr);
  PERFETTO_CHECK(handle && handle != INVALID_HANDLE_VALUE);
  res.fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), 0);
#elif PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  char temp_file[] = "/data/local/tmp/perfetto-XXXXXXXX";
  res.fd = mkstemp(temp_file);
#else
  char temp_file[] = "/tmp/perfetto-XXXXXXXX";
  res.fd = mkstemp(temp_file);
#endif
  res.path = temp_file;
  PERFETTO_CHECK(res.fd > 0);
  return res;
}

}  // namespace test
}  // namespace perfetto
