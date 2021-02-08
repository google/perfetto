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
#include <binder/IBinder.h>
#include <binder/IServiceManager.h>
#include <binder/Status.h>

namespace perfetto {
namespace android_internal {

using android::sp;
using android::binder::Status;
using android::tracing::ITracingServiceProxy;

bool NotifyTraceSessionEnded(bool session_stolen) {
  sp<ITracingServiceProxy> service = android::interface_cast<ITracingServiceProxy>(
      android::defaultServiceManager()->getService(android::String16("tracing.proxy")));

  if (service == nullptr) {
    return false;
  }

  Status s = service->notifyTraceSessionEnded(session_stolen);
  return s.isOk();
}

} // namespace android_internal
} // namespace perfetto
