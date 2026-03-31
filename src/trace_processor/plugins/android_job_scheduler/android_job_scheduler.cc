/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/trace_processor/plugins/android_job_scheduler/android_job_scheduler.h"

#include <memory>
#include <vector>

#include "perfetto/base/compiler.h"
#include "src/trace_processor/core/plugin/plugin.h"
#include "src/trace_processor/plugins/android_job_scheduler/android_job_scheduler_tracker.h"

namespace perfetto::trace_processor::android_job_scheduler {
namespace {

class AndroidJobSchedulerPlugin : public Plugin<AndroidJobSchedulerPlugin> {
 public:
  ~AndroidJobSchedulerPlugin() override;

  void RegisterTrackEventExtensions(
      TrackEventExtensionParserContext* ctx,
      TraceProcessorContext* trace_context) override {
    ctx->parsers.emplace_back(
        std::make_unique<AndroidJobSchedulerTracker>(ctx, trace_context));
  }
};

AndroidJobSchedulerPlugin::~AndroidJobSchedulerPlugin() = default;

}  // namespace

void RegisterPlugin() {
  static PluginRegistration reg(
      []() -> std::unique_ptr<PluginBase> {
        return std::make_unique<AndroidJobSchedulerPlugin>();
      },
      AndroidJobSchedulerPlugin::kPluginId,
      AndroidJobSchedulerPlugin::kDepIds.data(),
      AndroidJobSchedulerPlugin::kDepIds.size());
  base::ignore_result(reg);
}

}  // namespace perfetto::trace_processor::android_job_scheduler
