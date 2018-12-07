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

#include "src/profiling/memory/process_matcher.h"

#include "perfetto/base/logging.h"

namespace perfetto {
namespace profiling {

ProcessMatcher::Delegate::~Delegate() = default;

ProcessMatcher::ProcessHandle::ProcessHandle(ProcessMatcher* matcher, pid_t pid)
    : matcher_(matcher), pid_(pid) {}

ProcessMatcher::ProcessHandle::ProcessHandle(ProcessHandle&& other) noexcept
    : matcher_(other.matcher_), pid_(other.pid_) {
  other.matcher_ = nullptr;
}

ProcessMatcher::ProcessHandle& ProcessMatcher::ProcessHandle::operator=(
    ProcessHandle&& other) noexcept {
  // Construct this temporary because the RHS could be an lvalue cast to an
  // rvalue reference whose lifetime we do not know.
  ProcessHandle tmp(std::move(other));
  using std::swap;
  swap(*this, tmp);
  return *this;
}

ProcessMatcher::ProcessHandle::~ProcessHandle() {
  if (matcher_)
    matcher_->RemoveProcess(pid_);
}

ProcessMatcher::ProcessSetSpecHandle::ProcessSetSpecHandle(
    ProcessMatcher* matcher,
    std::multiset<ProcessSetSpecItem>::iterator iterator)
    : matcher_(matcher), iterator_(iterator) {}

ProcessMatcher::ProcessSetSpecHandle::ProcessSetSpecHandle(
    ProcessSetSpecHandle&& other) noexcept
    : matcher_(other.matcher_), iterator_(other.iterator_) {
  other.matcher_ = nullptr;
}

ProcessMatcher::ProcessSetSpecHandle& ProcessMatcher::ProcessSetSpecHandle::
operator=(ProcessSetSpecHandle&& other) noexcept {
  // Construct this temporary because the RHS could be an lvalue cast to an
  // rvalue reference whose lifetime we do not know.
  ProcessSetSpecHandle tmp(std::move(other));
  using std::swap;
  swap(*this, tmp);
  return *this;
}

std::set<pid_t> ProcessMatcher::ProcessSetSpecHandle::GetPIDs() const {
  std::set<pid_t> result;
  for (const ProcessItem* process_item : iterator_->process_items)
    result.emplace(process_item->process.pid);
  return result;
}

ProcessMatcher::ProcessSetSpecHandle::~ProcessSetSpecHandle() {
  if (matcher_)
    matcher_->UnwaitProcessSetSpec(iterator_);
}

ProcessMatcher::ProcessMatcher(Delegate* delegate) : delegate_(delegate) {}

ProcessMatcher::ProcessHandle ProcessMatcher::ProcessConnected(
    Process process) {
  pid_t pid = process.pid;
  decltype(pid_to_process_)::iterator it;
  bool inserted;
  std::tie(it, inserted) = pid_to_process_.emplace(pid, std::move(process));
  if (!inserted) {
    PERFETTO_DFATAL("Duplicated PID");
    return ProcessHandle(nullptr, 0);
  }

  ProcessItem* new_process_item = &(it->second);
  const std::string& cmdline = new_process_item->process.cmdline;
  cmdline_to_process_.emplace(cmdline, new_process_item);

  // Go through existing ProcessSetSpecs to find ones containing the newly
  // connected process.
  std::set<ProcessSetSpecItem*> matching_process_set_items =
      process_set_for_all_;
  auto pid_range = pid_to_process_set_.equal_range(pid);
  for (auto i = pid_range.first; i != pid_range.second; ++i) {
    ProcessSetSpec& ps = const_cast<ProcessSetSpec&>(i->second->process_set);
    if (ps.pids.find(pid) != ps.pids.end())
      matching_process_set_items.emplace(i->second);
  }
  auto cmdline_range = cmdline_to_process_set_.equal_range(cmdline);
  for (auto i = cmdline_range.first; i != cmdline_range.second; ++i) {
    ProcessSetSpec& ps = const_cast<ProcessSetSpec&>(i->second->process_set);
    if (ps.process_cmdline.find(cmdline) != ps.process_cmdline.end())
      matching_process_set_items.emplace(i->second);
  }

  for (ProcessSetSpecItem* process_set_item : matching_process_set_items) {
    process_set_item->process_items.emplace(new_process_item);
    new_process_item->references.emplace(process_set_item);
  }

  if (!matching_process_set_items.empty())
    RunMatchFn(new_process_item);

  return ProcessHandle(this, pid);
}

void ProcessMatcher::RemoveProcess(pid_t pid) {
  auto it = pid_to_process_.find(pid);
  if (it == pid_to_process_.end()) {
    PERFETTO_DFATAL("Could not find process.");
    return;
  }
  ProcessItem& process_item = it->second;
  auto range = cmdline_to_process_.equal_range(process_item.process.cmdline);
  for (auto process_it = range.first; process_it != range.second;
       ++process_it) {
    if (process_it->second == &process_item) {
      size_t erased = cmdline_to_process_.erase(process_item.process.cmdline);
      PERFETTO_DCHECK(erased);
      break;
    }
  }
  pid_to_process_.erase(it);
}

ProcessMatcher::ProcessSetSpecHandle ProcessMatcher::AwaitProcessSetSpec(
    ProcessSetSpec process_set) {
  auto it = process_sets_.emplace(this, std::move(process_set));
  ProcessSetSpecItem* new_process_set_item =
      const_cast<ProcessSetSpecItem*>(&*it);
  const ProcessSetSpec& new_process_set = new_process_set_item->process_set;

  // Go through currently active processes to find ones matching the new
  // ProcessSetSpec.
  std::set<ProcessItem*> matching_process_items;
  if (new_process_set.all) {
    process_set_for_all_.emplace(new_process_set_item);
    for (auto& p : pid_to_process_) {
      ProcessItem& process_item = p.second;
      matching_process_items.emplace(&process_item);
    }
  } else {
    for (pid_t pid : new_process_set.pids) {
      pid_to_process_set_.emplace(pid, new_process_set_item);
      auto process_it = pid_to_process_.find(pid);
      if (process_it != pid_to_process_.end())
        matching_process_items.emplace(&(process_it->second));
    }
    for (std::string cmdline : new_process_set.process_cmdline) {
      cmdline_to_process_set_.emplace(cmdline, new_process_set_item);
      auto range = cmdline_to_process_.equal_range(cmdline);
      for (auto process_it = range.first; process_it != range.second;
           ++process_it)
        matching_process_items.emplace(process_it->second);
    }
  }

  for (ProcessItem* process_item : matching_process_items) {
    new_process_set_item->process_items.emplace(process_item);
    process_item->references.emplace(new_process_set_item);
    RunMatchFn(process_item);
  }

  return ProcessSetSpecHandle(this, it);
}

void ProcessMatcher::UnwaitProcessSetSpec(
    std::multiset<ProcessSetSpecItem>::iterator iterator) {
  ProcessSetSpecItem& process_set_item =
      const_cast<ProcessSetSpecItem&>(*iterator);
  const ProcessSetSpec& process_set = process_set_item.process_set;

  for (pid_t pid : process_set.pids) {
    auto pid_range = pid_to_process_set_.equal_range(pid);
    for (auto i = pid_range.first; i != pid_range.second;) {
      if (i->second == &process_set_item)
        i = pid_to_process_set_.erase(i);
      else
        ++i;
    }
  }
  for (const std::string& cmdline : process_set.process_cmdline) {
    auto cmdline_range = cmdline_to_process_set_.equal_range(cmdline);
    for (auto i = cmdline_range.first; i != cmdline_range.second;) {
      if (i->second == &process_set_item)
        i = cmdline_to_process_set_.erase(i);
      else
        ++i;
    }
  }

  if (process_set.all)
    process_set_for_all_.erase(&process_set_item);
  process_sets_.erase(iterator);
}

ProcessMatcher::ProcessItem::~ProcessItem() {
  for (ProcessSetSpecItem* process_set_item : references) {
    size_t erased = process_set_item->process_items.erase(this);
    PERFETTO_DCHECK(erased);
  }
}

bool ProcessMatcher::ProcessSetSpecItem::operator<(
    const ProcessSetSpecItem& other) const {
  return std::tie(process_set.pids, process_set.process_cmdline,
                  process_set.all) < std::tie(other.process_set.pids,
                                              other.process_set.process_cmdline,
                                              other.process_set.all);
}

ProcessMatcher::ProcessSetSpecItem::~ProcessSetSpecItem() {
  for (ProcessItem* process_item : process_items) {
    size_t erased = process_item->references.erase(this);
    PERFETTO_DCHECK(erased);
    if (process_item->references.empty())
      matcher->ShutdownProcess(process_item->process.pid);
  }
}

void ProcessMatcher::ShutdownProcess(pid_t pid) {
  delegate_->Disconnect(pid);
}

void ProcessMatcher::RunMatchFn(ProcessItem* process_item) {
  std::vector<const ProcessSetSpec*> process_sets;
  for (ProcessSetSpecItem* process_set_item : process_item->references)
    process_sets.emplace_back(&(process_set_item->process_set));
  delegate_->Match(process_item->process, process_sets);
}

void swap(ProcessMatcher::ProcessHandle& a, ProcessMatcher::ProcessHandle& b) {
  using std::swap;
  swap(a.matcher_, b.matcher_);
  swap(a.pid_, b.pid_);
}

void swap(ProcessMatcher::ProcessSetSpecHandle& a,
          ProcessMatcher::ProcessSetSpecHandle& b) {
  using std::swap;
  swap(a.matcher_, b.matcher_);
  swap(a.iterator_, b.iterator_);
}

}  // namespace profiling
}  // namespace perfetto
