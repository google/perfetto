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

#include "src/android_internal/tracing_service_proxy.h"

#include <android/tracing/ITracingServiceProxy.h>
#include <android/tracing/TraceReportParams.h>
#include <binder/IBinder.h>
#include <binder/IServiceManager.h>
#include <binder/ParcelFileDescriptor.h>
#include <binder/Status.h>
#include <utils/String16.h>
#include <chrono>
#include <thread>

namespace perfetto {
namespace android_internal {

using android::sp;
using android::binder::Status;
using android::os::ParcelFileDescriptor;
using android::tracing::ITracingServiceProxy;
using android::tracing::TraceReportParams;

namespace {
static constexpr char kServiceName[] = "tracing.proxy";
}

bool NotifyTraceSessionEnded(bool session_stolen) {
  auto service = android::waitForService<ITracingServiceProxy>(
      android::String16(kServiceName));
  if (service == nullptr) {
    return false;
  }

  Status s = service->notifyTraceSessionEnded(session_stolen);
  return s.isOk();
}

bool ReportTrace(const char* reporter_package_name,
                 const char* reporter_class_name,
                 int owned_trace_fd,
                 int64_t uuid_lsb,
                 int64_t uuid_msb,
                 bool use_pipe_in_framework_for_testing) {
  // Keep this first so we recapture the raw fd in a RAII type as soon as
  // possible.
  android::base::unique_fd fd(owned_trace_fd);

  // A trace upload may start soon after boot completes.
  // However, the device must be unlocked first before the trace can be
  // uploaded. So, if the user unlocks the device much later, waitForService
  // will log every second. Polling first for at most 1 hour to reduce log spam.
  uint32_t polling_count = 60 * 60;  // 1 hour
  while (polling_count > 0 && android::defaultServiceManager()->checkService(
                                  android::String16(kServiceName)) == nullptr) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    polling_count--;
  }

  auto service = android::waitForService<ITracingServiceProxy>(
      android::String16(kServiceName));
  if (service == nullptr) {
    return false;
  }

  TraceReportParams params{};
  params.reporterPackageName = android::String16(reporter_package_name);
  params.reporterClassName = android::String16(reporter_class_name);
  params.fd = ParcelFileDescriptor(std::move(fd));
  params.uuidLsb = uuid_lsb;
  params.uuidMsb = uuid_msb;
  params.usePipeForTesting = use_pipe_in_framework_for_testing;

  Status s = service->reportTrace(std::move(params));
  if (!s.isOk()) {
    __android_log_print(ANDROID_LOG_ERROR, "perfetto", "reportTrace failed: %s",
                        s.toString8().c_str());
  }

  return s.isOk();
}

}  // namespace android_internal
}  // namespace perfetto
