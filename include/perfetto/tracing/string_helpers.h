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

#ifndef INCLUDE_PERFETTO_TRACING_STRING_HELPERS_H_
#define INCLUDE_PERFETTO_TRACING_STRING_HELPERS_H_

#include "perfetto/base/export.h"

#include <cstddef>
#include <string>

namespace perfetto {

// A wrapper for marking strings that can't be determined to be static at build
// time, but are in fact static.
class PERFETTO_EXPORT StaticString {
 public:
  // Implicit constructor for string literals.
  template <size_t N>
  constexpr StaticString(const char (&str)[N]) : value(str) {}

  // Implicit constructor for null strings.
  constexpr StaticString(std::nullptr_t) : value(nullptr) {}

  constexpr explicit StaticString(const char* str) : value(str) {}

  const char* value;
};

namespace internal {

// Ensure that |string| is a static constant string.
//
// If you get a compiler failure here, you are most likely trying to use
// TRACE_EVENT with a dynamic event name. There are two ways to fix this:
//
// 1) If the event name is actually dynamic (e.g., std::string), write it into
//    the event manually:
//
//      TRACE_EVENT("category", nullptr, [&](perfetto::EventContext ctx) {
//        ctx.event()->set_name(dynamic_name);
//      });
//
// 2) If the name is static, but the pointer is computed at runtime, wrap it
//    with perfetto::StaticString:
//
//      TRACE_EVENT("category", perfetto::StaticString{name});
//
//    DANGER: Using perfetto::StaticString with strings whose contents change
//            dynamically can cause silent trace data corruption.
//
constexpr const char* GetStaticString(StaticString string) {
  return string.value;
}

}  // namespace internal

// A explicit wrapper for marking strings as dynamic to ensure that perfetto
// doesn't try to cache the pointer value.
class PERFETTO_EXPORT DynamicString {
 public:
  explicit DynamicString(const std::string& str)
      : value(str.data()), length(str.length()) {}
  explicit DynamicString(const char* str) : value(str), length(strlen(str)) {}
  DynamicString(const char* str, size_t len) : value(str), length(len) {}

  const char* value;
  size_t length;
};

}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_TRACING_STRING_HELPERS_H_
