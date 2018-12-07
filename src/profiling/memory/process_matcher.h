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

#ifndef SRC_PROFILING_MEMORY_PROCESS_MATCHER_H_
#define SRC_PROFILING_MEMORY_PROCESS_MATCHER_H_

#include <map>
#include <set>
#include <string>
#include <vector>

#include "src/profiling/memory/wire_protocol.h"

namespace perfetto {
namespace profiling {

struct Process {
  pid_t pid;
  std::string cmdline;
};

struct ProcessSetSpec {
  std::set<pid_t> pids;
  std::set<std::string> process_cmdline;
  bool all = false;

  ClientConfiguration client_configuration{};
};

// The Matcher allows DataSources to wait for ProcessSetSpecs, and the
// SocketListener to notify connection of a new Process. Both of these
// operations return an opaque handle that should be held on to by the caller.
//
// If the ProcessHandle gets destroyed, it signals to the Matcher that the
// process disconnected. If the ProcessSetSpecHandle goes away, it signals to
// the Matcher that the ProcessSetSpec has been torn down. When the last
// ProcessSetSpec referring to a Process gets torn down, the Process has to be
// shut down.
//
// In the constructor, a match_fn and a shutdown_fn are supplied.
// match_fn is called when the set of ProcessSetSpecs for a given process
// changes, so that the SocketListener can compute and send the appropriate
// ClientConfiguration.
// shutdown_fn is called when the last DataSource for a process gets torn
// down.
class ProcessMatcher {
 private:
  struct ProcessItem;
  struct ProcessSetSpecItem;

 public:
  class Delegate {
   public:
    virtual void Match(
        const Process& process,
        const std::vector<const ProcessSetSpec*>& process_sets) = 0;
    virtual void Disconnect(pid_t pid) = 0;
    virtual ~Delegate();
  };

  class ProcessHandle {
   public:
    friend class ProcessMatcher;
    friend void swap(ProcessHandle&, ProcessHandle&);
    ProcessHandle() = default;

    ~ProcessHandle();
    ProcessHandle(const ProcessHandle&) = delete;
    ProcessHandle& operator=(const ProcessHandle&) = delete;
    ProcessHandle(ProcessHandle&&) noexcept;
    ProcessHandle& operator=(ProcessHandle&&) noexcept;

   private:
    ProcessHandle(ProcessMatcher* matcher, pid_t pid);

    ProcessMatcher* matcher_ = nullptr;
    pid_t pid_;
  };

  class ProcessSetSpecHandle {
   public:
    friend class ProcessMatcher;
    friend void swap(ProcessSetSpecHandle&, ProcessSetSpecHandle&);
    ProcessSetSpecHandle() = default;

    ~ProcessSetSpecHandle();
    ProcessSetSpecHandle(const ProcessSetSpecHandle&) = delete;
    ProcessSetSpecHandle& operator=(const ProcessSetSpecHandle&) = delete;
    ProcessSetSpecHandle(ProcessSetSpecHandle&&) noexcept;
    ProcessSetSpecHandle& operator=(ProcessSetSpecHandle&&) noexcept;

    std::set<pid_t> GetPIDs() const;

   private:
    ProcessSetSpecHandle(ProcessMatcher* matcher,
                         std::multiset<ProcessSetSpecItem>::iterator iterator);

    ProcessMatcher* matcher_ = nullptr;
    std::multiset<ProcessSetSpecItem>::iterator iterator_;
  };

  ProcessMatcher(Delegate* delegate);

  // Notify that a process has connected. This will determine which
  // ProcessSetSpecs it matches, and call match_fn with that set.
  // This is called by the SocketListener.
  ProcessHandle ProcessConnected(Process process);

  // Wait for connection of a set of processes as specified in ProcessSetSpec.
  // When a process matching that specificaton connects, match_fn will be called
  // with this and other ProcessSetSpecs that have called this function
  // previously.
  // This is called by HeapprofdProducer.
  ProcessSetSpecHandle AwaitProcessSetSpec(ProcessSetSpec process_set);

 private:
  // ProcessItem and ProcessSetSpecItem are held internally in the Matcher for
  // each Process and ProcessSetSpec. Matched Processes and ProcessSetSpecs have
  // pointers to each other in their ProcessItem and ProcessSetSpecItem structs,
  // which are automatically kept up to date in the destructors.
  struct ProcessItem {
    // No copy or move as we rely on pointer stability in ProcessSetSpecItem.
    ProcessItem(const ProcessItem&) = delete;
    ProcessItem& operator=(const ProcessItem&) = delete;
    ProcessItem(ProcessItem&&) = delete;
    ProcessItem& operator=(ProcessItem&&) = delete;

    ProcessItem(Process p) : process(std::move(p)) {}

    Process process;
    std::set<ProcessSetSpecItem*> references;

    ~ProcessItem();
  };

  struct ProcessSetSpecItem {
    // No copy or move as we rely on pointer stability in ProcessSetSpec.
    ProcessSetSpecItem(const ProcessSetSpecItem&) = delete;
    ProcessSetSpecItem& operator=(const ProcessSetSpecItem&) = delete;
    ProcessSetSpecItem(ProcessSetSpecItem&&) = delete;
    ProcessSetSpecItem& operator=(ProcessSetSpecItem&&) = delete;

    ProcessSetSpecItem(ProcessMatcher* m, ProcessSetSpec ps)
        : matcher(m), process_set(std::move(ps)) {}

    ~ProcessSetSpecItem();
    bool operator<(const ProcessSetSpecItem& other) const;

    ProcessMatcher* matcher;
    const ProcessSetSpec process_set;
    std::set<ProcessItem*> process_items;
  };

  void UnwaitProcessSetSpec(
      std::multiset<ProcessSetSpecItem>::iterator iterator);
  void RemoveProcess(pid_t pid);
  void ShutdownProcess(pid_t pid);
  void RunMatchFn(ProcessItem* process_item);

  Delegate* delegate_;

  std::map<pid_t, ProcessItem> pid_to_process_;
  std::multimap<std::string, ProcessItem*> cmdline_to_process_;

  std::multiset<ProcessSetSpecItem> process_sets_;
  std::multimap<pid_t, ProcessSetSpecItem*> pid_to_process_set_;
  std::multimap<std::string, ProcessSetSpecItem*> cmdline_to_process_set_;
  std::set<ProcessSetSpecItem*> process_set_for_all_;
};

void swap(ProcessMatcher::ProcessHandle& a, ProcessMatcher::ProcessHandle& b);
void swap(ProcessMatcher::ProcessSetSpecHandle& a,
          ProcessMatcher::ProcessSetSpecHandle& b);

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_MEMORY_PROCESS_MATCHER_H_
