/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "perfetto/tracing/debug_annotation.h"

#include "protos/perfetto/trace/track_event/debug_annotation.pbzero.h"

namespace perfetto {

DebugAnnotation::~DebugAnnotation() = default;

namespace internal {

void WriteDebugAnnotation(protos::pbzero::DebugAnnotation* annotation,
                          bool value) {
  annotation->set_bool_value(value);
}

void WriteDebugAnnotation(protos::pbzero::DebugAnnotation* annotation,
                          uint64_t value) {
  annotation->set_uint_value(value);
}

void WriteDebugAnnotation(protos::pbzero::DebugAnnotation* annotation,
                          unsigned value) {
  annotation->set_uint_value(value);
}

void WriteDebugAnnotation(protos::pbzero::DebugAnnotation* annotation,
                          int64_t value) {
  annotation->set_int_value(value);
}

void WriteDebugAnnotation(protos::pbzero::DebugAnnotation* annotation,
                          int value) {
  annotation->set_int_value(value);
}

void WriteDebugAnnotation(protos::pbzero::DebugAnnotation* annotation,
                          double value) {
  annotation->set_double_value(value);
}

void WriteDebugAnnotation(protos::pbzero::DebugAnnotation* annotation,
                          float value) {
  annotation->set_double_value(static_cast<double>(value));
}

void WriteDebugAnnotation(protos::pbzero::DebugAnnotation* annotation,
                          const char* value) {
  annotation->set_string_value(value);
}

void WriteDebugAnnotation(protos::pbzero::DebugAnnotation* annotation,
                          const std::string& value) {
  annotation->set_string_value(value);
}

void WriteDebugAnnotation(protos::pbzero::DebugAnnotation* annotation,
                          const void* value) {
  annotation->set_pointer_value(reinterpret_cast<uint64_t>(value));
}

void WriteDebugAnnotation(protos::pbzero::DebugAnnotation* annotation,
                          const DebugAnnotation& custom_annotation) {
  custom_annotation.Add(annotation);
}

}  // namespace internal
}  // namespace perfetto
