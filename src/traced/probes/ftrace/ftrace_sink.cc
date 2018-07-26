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

#include "src/traced/probes/ftrace/ftrace_sink.h"

#include "src/traced/probes/ftrace/cpu_reader.h"
#include "src/traced/probes/ftrace/ftrace_controller.h"

namespace perfetto {

FtraceSink::FtraceSink(base::WeakPtr<FtraceController> controller_weak,
                       FtraceConfigId id,
                       FtraceConfig config,
                       std::unique_ptr<EventFilter> filter,
                       Delegate* delegate)
    : controller_weak_(std::move(controller_weak)),
      id_(id),
      config_(std::move(config)),
      filter_(std::move(filter)),
      delegate_(delegate){};

FtraceSink::~FtraceSink() {
  if (controller_weak_)
    controller_weak_->Unregister(this);
};

const std::set<std::string>& FtraceSink::enabled_events() {
  return filter_->enabled_names();
}

void FtraceSink::DumpFtraceStats(FtraceStats* stats) {
  if (controller_weak_)
    controller_weak_->DumpFtraceStats(stats);
}

FtraceSink::Delegate::~Delegate() = default;

}  // namespace perfetto
