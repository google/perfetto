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

#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/importers/art_hprof/art_hprof_model.h"
#include "src/trace_processor/storage/trace_storage.h"

#include <cstdint>
#include <string>

namespace perfetto::trace_processor::art_hprof {
constexpr const char* kUnknownString = "[unknown string]";

class HeapGraph {
 public:
  HeapGraph(uint64_t timestamp) : timestamp_(timestamp) {}

  // Delete copy constructor and assignment operator
  HeapGraph(const HeapGraph&) = delete;
  HeapGraph& operator=(const HeapGraph&) = delete;
  HeapGraph(HeapGraph&&) = default;
  HeapGraph& operator=(HeapGraph&&) = default;
  ~HeapGraph() = default;

  void SetObjects(ObjectStore objs) { objects_ = std::move(objs); }
  void SetClassFields(ClassFieldLayouts fields) {
    class_fields_ = std::move(fields);
  }
  void SetIdSize(uint32_t id_size) { id_size_ = id_size; }
  void SetClasses(base::FlatHashMap<uint64_t, ClassDefinition> cls) {
    classes_ = std::move(cls);
  }
  void SetStrings(base::FlatHashMap<uint64_t, StringId> strs) {
    strings_ = std::move(strs);
  }

  const ObjectStore& GetObjects() const { return objects_; }

  // Instance field layout of a class hierarchy, or nullptr if unknown.
  const ClassFieldLayout* GetClassFields(uint64_t class_id) const {
    return class_fields_.Find(class_id);
  }

  uint32_t GetIdSize() const { return id_size_; }

  const base::FlatHashMap<uint64_t, ClassDefinition>& GetClasses() const {
    return classes_;
  }

  size_t GetObjectCount() const { return objects_.size(); }
  size_t GetClassCount() const { return classes_.size(); }
  size_t GetStringCount() const { return strings_.size(); }
  uint64_t GetTimestamp() const { return timestamp_; }

  void ClearAll() {
    objects_.Clear();
    classes_.Clear();
    strings_.Clear();
    class_fields_.Clear();
    heap_id_to_name_.Clear();
  }

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

    return "UNKNOWN";
  }

 private:
  ObjectStore objects_;
  ClassFieldLayouts class_fields_;
  base::FlatHashMap<uint64_t, ClassDefinition> classes_;
  base::FlatHashMap<uint64_t, StringId> strings_;
  base::FlatHashMap<uint32_t, std::string> heap_id_to_name_;
  uint64_t timestamp_;
  uint32_t id_size_ = 4;
};
}  // namespace perfetto::trace_processor::art_hprof

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_ART_HPROF_ART_HEAP_GRAPH_H_
