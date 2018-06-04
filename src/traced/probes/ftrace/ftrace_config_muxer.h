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

#ifndef SRC_TRACED_PROBES_FTRACE_FTRACE_CONFIG_MUXER_H_
#define SRC_TRACED_PROBES_FTRACE_FTRACE_CONFIG_MUXER_H_

#include "src/traced/probes/ftrace/ftrace_controller.h"
#include "src/traced/probes/ftrace/ftrace_procfs.h"

namespace perfetto {

// Ftrace is a bunch of globaly modifiable persistent state.
// Given a number of FtraceConfig's we need to find the best union of all
// the settings to make eveyone happy while also watching out for anybody
// messing with the ftrace settings at the same time as us.

// Specifically FtraceConfigMuxer takes in a *requested* FtraceConfig
// (|RequestConfig|), makes a best effort attempt to modify the ftrace
// debugfs files to honor those settings without interupting other perfetto
// traces already in progress or other users of ftrace, then returns an
// FtraceConfigId representing that config or zero on failure.

// To see which settings we actually managed to set you can call |GetConfig|
// and when you are finished with a config you can signal that with
// |RemoveConfig|.
class FtraceConfigMuxer {
 public:
  // The FtraceConfigMuxer and ProtoTranslationTable
  // should outlive this instance.
  FtraceConfigMuxer(FtraceProcfs* ftrace, const ProtoTranslationTable* table);
  virtual ~FtraceConfigMuxer();

  // Ask FtraceConfigMuxer to adjust ftrace procfs settings to
  // match the requested config. Returns an id to manage this
  // config or zero on failure.
  // This is best effort. FtraceConfigMuxer may not be able to adjust the
  // buffer size right now. Events may be missing or there may be extra events
  // (if you enable an atrace catagory we try to give you the matching events).
  // If someone else is tracing we won't touch atrace (since it resets the
  // buffer).
  // To see the config you ended up with use |GetConfig|.
  FtraceConfigId RequestConfig(const FtraceConfig& request);

  // Undo changes for the given config. Returns false iff the id is 0
  // or already removed.
  bool RemoveConfig(FtraceConfigId id);

  // public for testing
  void SetupClockForTesting(const FtraceConfig& request) {
    SetupClock(request);
  }

  const FtraceConfig* GetConfig(FtraceConfigId id);

 private:
  struct FtraceState {
    std::set<std::string> ftrace_events;
    std::set<std::string> atrace_categories;
    std::set<std::string> atrace_apps;
    bool tracing_on = false;
    bool atrace_on = false;
    size_t cpu_buffer_size_pages = 0;
  };

  FtraceConfigMuxer(const FtraceConfigMuxer&) = delete;
  FtraceConfigMuxer& operator=(const FtraceConfigMuxer&) = delete;

  void SetupClock(const FtraceConfig& request);
  void SetupBufferSize(const FtraceConfig& request);
  void UpdateAtrace(const FtraceConfig& request);
  void DisableAtrace();

  FtraceConfigId GetNextId();

  FtraceConfigId last_id_ = 1;
  FtraceProcfs* ftrace_;
  const ProtoTranslationTable* table_;

  FtraceState current_state_;
  std::map<FtraceConfigId, FtraceConfig> configs_;
};

std::set<std::string> GetFtraceEvents(const FtraceConfig& request,
                                      const ProtoTranslationTable*);
size_t ComputeCpuBufferSizeInPages(size_t requested_buffer_size_kb);

}  // namespace perfetto

#endif  // SRC_TRACED_PROBES_FTRACE_FTRACE_CONFIG_MUXER_H_
