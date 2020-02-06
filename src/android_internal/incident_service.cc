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

#include "src/android_internal/incident_service.h"

#include <binder/IBinder.h>
#include <binder/IServiceManager.h>
#include <binder/Status.h>
#include <incident/incident_report.h>
#include <stddef.h>
#include <stdint.h>

#include <string>

namespace perfetto {
namespace android_internal {

bool StartIncidentReport(const char* dest_pkg,
                         const char* dest_class,
                         int privacy_level) {
  android::os::IncidentReportRequest incidentReport;
  incidentReport.addSection(3026);  // system_trace only

  if (privacy_level != INCIDENT_REPORT_PRIVACY_POLICY_AUTOMATIC &&
      privacy_level != INCIDENT_REPORT_PRIVACY_POLICY_EXPLICIT) {
    return false;
  }
  incidentReport.setPrivacyPolicy(privacy_level);

  std::string pkg(dest_pkg);
  std::string cls(dest_class);
  if (pkg.size() == 0 || cls.size() == 0) {
    return false;
  }
  incidentReport.setReceiverPackage(pkg);
  incidentReport.setReceiverClass(cls);

  return incidentReport.takeReport() == 0;
}

}  // namespace android_internal
}  // namespace perfetto
