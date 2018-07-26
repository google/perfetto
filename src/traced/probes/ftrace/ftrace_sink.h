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

#ifndef SRC_TRACED_PROBES_FTRACE_FTRACE_SINK_H_
#define SRC_TRACED_PROBES_FTRACE_FTRACE_SINK_H_

#include <memory>
#include <set>
#include <string>

#include "perfetto/base/scoped_file.h"
#include "perfetto/base/weak_ptr.h"
#include "perfetto/protozero/message_handle.h"
#include "src/traced/probes/ftrace/ftrace_config.h"
#include "src/traced/probes/ftrace/ftrace_metadata.h"

namespace perfetto {

class EventFilter;
class FtraceController;
struct FtraceStats;

namespace protos {
namespace pbzero {
class FtraceEventBundle;
}  // namespace pbzero
}  // namespace protos

// To consume ftrace data clients implement a |FtraceSink::Delegate| and use it
// to create a |FtraceSink|. While the FtraceSink lives FtraceController will
// call |GetBundleForCpu|, write data into the bundle then call
// |OnBundleComplete| allowing the client to perform finalization.
class FtraceSink {
 public:
  using FtraceEventBundle = protos::pbzero::FtraceEventBundle;
  class Delegate {
   public:
    virtual void OnCreate(FtraceSink*) {}
    virtual protozero::MessageHandle<FtraceEventBundle> GetBundleForCpu(
        size_t) = 0;
    virtual void OnBundleComplete(size_t,
                                  protozero::MessageHandle<FtraceEventBundle>,
                                  const FtraceMetadata&) = 0;
    virtual ~Delegate();
  };

  FtraceSink(base::WeakPtr<FtraceController>,
             FtraceConfigId id,
             FtraceConfig config,
             std::unique_ptr<EventFilter>,
             Delegate*);
  ~FtraceSink();

  void DumpFtraceStats(FtraceStats*);

  const FtraceConfig& config() const { return config_; }

 private:
  friend FtraceController;

  FtraceSink(const FtraceSink&) = delete;
  FtraceSink& operator=(const FtraceSink&) = delete;

  EventFilter* event_filter() { return filter_.get(); }
  FtraceMetadata* metadata_mutable() { return &metadata_; }

  protozero::MessageHandle<FtraceEventBundle> GetBundleForCpu(size_t cpu) {
    return delegate_->GetBundleForCpu(cpu);
  }
  void OnBundleComplete(size_t cpu,
                        protozero::MessageHandle<FtraceEventBundle> bundle) {
    delegate_->OnBundleComplete(cpu, std::move(bundle), metadata_);
    metadata_.Clear();
  }

  const std::set<std::string>& enabled_events();

  base::WeakPtr<FtraceController> controller_weak_;
  const FtraceConfigId id_;
  const FtraceConfig config_;
  std::unique_ptr<EventFilter> filter_;
  FtraceMetadata metadata_;
  FtraceSink::Delegate* delegate_;
};

}  // namespace perfetto

#endif  // SRC_TRACED_PROBES_FTRACE_FTRACE_SINK_H_
