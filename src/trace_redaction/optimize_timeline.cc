/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_redaction/optimize_timeline.h"

#include "perfetto/base/status.h"
#include "src/trace_redaction/trace_redaction_framework.h"

namespace perfetto::trace_redaction {

base::Status OptimizeTimeline::Build(Context* context) const {
  if (!context->timeline) {
    return base::ErrStatus(
        "Cannot optimize a null timeline. Are you missing "
        "CollectTimelineEvents or an "
        "alternative?");
  }

  if (!context->package_uid.has_value()) {
    return base::ErrStatus(
        "Missing package uid. Are you missing FindPackageUid or an "
        "alternative?");
  }

  auto* timeline = context->timeline.get();

  // Change the timeline from read-only to write only mode.
  timeline->Sort();

  // Goes over the whole timeline, reducing the distance between a pid and its
  // uid.
  timeline->Flatten();

  // Reduce the number of events. This makes the timeline specific to the
  // package uid (i.e. either 0 or package_uid will be returned).
  timeline->Reduce(*context->package_uid);

  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
