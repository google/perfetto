/*
 * Copyright (C) 2025 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_ART_HPROF_ART_HEAP_GRAPH_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_ART_HPROF_ART_HEAP_GRAPH_H_

#include "src/trace_processor/importers/art_hprof/art_hprof_model.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace perfetto::trace_processor::art_hprof {
constexpr const char* kUnknownString = "[unknown string]";

class HeapGraph {
 public:
  HeapGraph(uint64_t timestamp) : timestamp_(timestamp) {}

  HeapGraph(const HeapGraph&) = default;
  HeapGraph& operator=(const HeapGraph&) = default;
  HeapGraph(HeapGraph&&) = default;
  HeapGraph& operator=(HeapGraph&&) = default;
  ~HeapGraph() = default;

  void AddObject(Object object) {
    objects_[object.GetId()] = std::move(object);
  }

  void AddClass(ClassDefinition cls) { classes_[cls.GetId()] = std::move(cls); }

  void AddString(uint64_t id, std::string string) {
    strings_[id] = std::move(string);
  }

  std::string GetString(uint64_t id) const {
    auto it = strings_.find(id);
    if (it != strings_.end()) {
      return it->second;
    }
    return kUnknownString;
  }

  const std::unordered_map<uint64_t, Object>& GetObjects() const {
    return objects_;
  }

  const std::unordered_map<uint64_t, ClassDefinition>& GetClasses() const {
    return classes_;
  }

  size_t GetObjectCount() const { return objects_.size(); }
  size_t GetClassCount() const { return classes_.size(); }
  size_t GetStringCount() const { return strings_.size(); }
  uint64_t GetTimestamp() const { return timestamp_; }

  static std::string GetRootTypeName(HprofHeapRootTag root_type_id) {
    switch (root_type_id) {
      case HprofHeapRootTag::kJniGlobal:
        return "JNI_GLOBAL";
      case HprofHeapRootTag::kJniLocal:
        return "JNI_LOCAL";
      case HprofHeapRootTag::kJavaFrame:
        return "JAVA_FRAME";
      case HprofHeapRootTag::kNativeStack:
        return "NATIVE_STACK";
      case HprofHeapRootTag::kStickyClass:
        return "STICKY_CLASS";
      case HprofHeapRootTag::kThreadBlock:
        return "THREAD_BLOCK";
      case HprofHeapRootTag::kMonitorUsed:
        return "MONITOR_USED";
      case HprofHeapRootTag::kThreadObj:
        return "THREAD_OBJECT";
      case HprofHeapRootTag::kInternedString:
        return "INTERNED_STRING";
      case HprofHeapRootTag::kFinalizing:
        return "FINALIZING";
      case HprofHeapRootTag::kDebugger:
        return "DEBUGGER";
      case HprofHeapRootTag::kVmInternal:
        return "VM_INTERNAL";
      case HprofHeapRootTag::kJniMonitor:
        return "JNI_MONITOR";
      case HprofHeapRootTag::kUnknown:
        return "UNKNOWN";
    }
  }

 private:
  std::unordered_map<uint64_t, Object> objects_;
  std::unordered_map<uint64_t, ClassDefinition> classes_;
  std::unordered_map<uint64_t, std::string> strings_;
  std::unordered_map<uint32_t, std::string> heap_id_to_name_;
  uint64_t timestamp_;
};
}  // namespace perfetto::trace_processor::art_hprof

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_ART_HPROF_ART_HEAP_GRAPH_H_
