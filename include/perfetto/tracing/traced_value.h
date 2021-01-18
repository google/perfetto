/*
 * Copyright (C) 2021 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_TRACING_TRACED_VALUE_H_
#define INCLUDE_PERFETTO_TRACING_TRACED_VALUE_H_

#include "perfetto/base/compiler.h"
#include "perfetto/base/export.h"
#include "protos/perfetto/trace/track_event/debug_annotation.pbzero.h"

#include <type_traits>
#include <utility>

namespace perfetto {

class DebugAnnotation;

// *** NOTE ***
// This is work-in-progress and the examples below do not work yet.
//
//
// These classes provide a JSON-inspired way to write structed data into traces.
//
// Each TracedValue can be consumed exactly once to write a value into a trace
// using one of the Write* methods.
//
// Write* methods fall into two categories:
// - Primitive types (int, string, bool, double, etc): they just write the
//   provided value, consuming the TracedValue in the process.
// - Complex types (arrays and dicts): they consume the TracedValue and
//   return a corresponding scoped object (TracedArray or TracedDictionary).
//   This scope then can be used to write multiple items into the container:
//   TracedArray::AppendItem and TracedDictionary::AddItem return a new
//   TracedValue which then can be used to write an element of the
//   dictionary or array.
//
// To define how a custom class should be written into the trace, users should
// define one of the two following functions:
// - Foo::WriteIntoTrace(TracedValue) const
//   (preferred for code which depends on perfetto directly)
// - perfetto::TraceFormatTraits<T>::WriteIntoTrace(TracedValue, const T&);
//   (should be used if T is defined in a library which doesn't know anything
//   about tracing).
//
// After definiting a conversion method, the object can be used directly as a
// TRACE_EVENT argument:
//
// Foo foo;
// TRACE_EVENT("cat", "Event", "arg", foo);
//
// Examples:
//
// TRACE_EVENT("cat", "event", "params", [&](perfetto::TracedValue writer)
// {
//   auto dict = std::move(writer).WriteDictionary();
//   dict->Add("param1", param1);
//   dict->Add("param2", param2);
//   ...
//   dict->Add("paramN", paramN);
//
//   {
//     auto inner_array = dict->AddArray("inner");
//     inner_array->Append(value1);
//     inner_array->Append(value2);
//   }
// });
//
// template <class T>
// TraceFormatTraits<std::optional<T>>::WriteIntoTrace(
//    TracedValue writer, const std::optional<T>& value) {
//  if (!value) {
//    std::move(writer).WritePointer(nullptr);
//    return;
//  }
//  perfetto::Write(std::move(writer), *value);
// }
//
// template <class T>
// TraceFormatTraits<std::vector<T>>::WriteIntoTrace(
//    TracedValue writer, const std::array<T>& value) {
//  auto array = std::move(writer).WriteArray();
//  for (const auto& item: value) {
//    array_scope.Append(item);
//  }
// }
//
// class Foo {
//   void WriteIntoTrace(TracedValue writer) const {
//     auto dict = std::move(writer).WriteDictionary();
//     dict->Set("key", 42);
//     dict->Set("foo", "bar");
//     dict->Set("member", member_);
//   }
// }
class TracedArray;
class TracedDictionary;

class PERFETTO_EXPORT TracedValue {
 public:
  TracedValue(const TracedValue&) = delete;
  TracedValue& operator=(const TracedValue&) = delete;
  TracedValue& operator=(TracedValue&&) = delete;
  TracedValue(TracedValue&&) = default;
  ~TracedValue() = default;

  void WriteInt64(int64_t value) &&;
  void WriteUInt64(uint64_t value) &&;
  void WriteDouble(double value) &&;
  void WriteBoolean(bool value) &&;
  void WriteString(const char*) &&;
  void WriteString(const char*, size_t len) &&;
  void WriteString(const std::string&) &&;
  void WritePointer(const void* value) &&;

  // Rules for writing nested dictionaries and arrays:
  // - Only one scope (TracedArray, TracedDictionary or TracedValue) can be
  // active at the same time. It's only allowed to call methods on the active
  // scope.
  // - When a scope creates a nested scope, the new scope becomes active.
  // - When a scope is destroyed, it's parent scope becomes active again.
  //
  // Typically users will have to create a scope only at the beginning of a
  // conversion function and this scope should be destroyed at the end of it.
  // TracedArray::Append and TracedDictionary::Add create, write and complete
  // inner scopes automatically.

  // Scope which allows multiple values to be appended.
  TracedArray WriteArray() && PERFETTO_WARN_UNUSED_RESULT;

  // Scope which allows multiple key-value pairs to be added.
  TracedDictionary WriteDictionary() && PERFETTO_WARN_UNUSED_RESULT;

  static TracedValue CreateForTest(protos::pbzero::DebugAnnotation*);

 private:
  friend class TracedArray;
  friend class TracedDictionary;

  inline explicit TracedValue(protos::pbzero::DebugAnnotation* root_context)
      : root_context_(root_context) {}
  inline explicit TracedValue(
      protos::pbzero::DebugAnnotation::NestedValue* nested_context)
      : nested_context_(nested_context) {}

  // Temporary support for perfetto::DebugAnnotation C++ class before it's going
  // to be replaced by TracedValue.
  // TODO(altimin): Convert v8 to use TracedValue directly and delete it.
  friend class DebugAnnotation;

  // Only one of them can be null.
  // TODO(altimin): replace DebugAnnotation with something that doesn't require
  // this duplication.
  protos::pbzero::DebugAnnotation* root_context_ = nullptr;
  protos::pbzero::DebugAnnotation::NestedValue* nested_context_ = nullptr;
};

class TracedArray {
 public:
  TracedArray(const TracedArray&) = delete;
  TracedArray& operator=(const TracedArray&) = delete;
  TracedArray& operator=(TracedArray&&) = delete;
  TracedArray(TracedArray&&) = default;
  ~TracedArray() { value_->Finalize(); }

  TracedValue AppendItem();

  TracedDictionary AppendDictionary() PERFETTO_WARN_UNUSED_RESULT;
  TracedArray AppendArray();

 private:
  friend class TracedValue;

  inline explicit TracedArray(
      protos::pbzero::DebugAnnotation::NestedValue* value)
      : value_(value) {}

  protos::pbzero::DebugAnnotation::NestedValue* value_;
};

class TracedDictionary {
 public:
  TracedDictionary(const TracedDictionary&) = delete;
  TracedDictionary& operator=(const TracedDictionary&) = delete;
  TracedDictionary& operator=(TracedDictionary&&) = delete;
  TracedDictionary(TracedDictionary&&) = default;
  ~TracedDictionary() {}

  TracedValue AddItem(const char* key);

  TracedDictionary AddDictionary(const char* key);
  TracedArray AddArray(const char* key);

 private:
  friend class TracedValue;

  inline explicit TracedDictionary(
      protos::pbzero::DebugAnnotation::NestedValue* value)
      : value_(value) {}

  protos::pbzero::DebugAnnotation::NestedValue* value_;
};

}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_TRACING_TRACED_VALUE_H_
