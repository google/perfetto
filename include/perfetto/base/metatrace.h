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

#ifndef INCLUDE_PERFETTO_BASE_METATRACE_H_
#define INCLUDE_PERFETTO_BASE_METATRACE_H_

#include <string>
#include <tuple>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/utils.h"

namespace perfetto {
namespace base {

template <typename T>
std::string FormatJSON(T value) {
  return std::to_string(value);
}
template <>
std::string FormatJSON<std::string>(std::string value);
template <>
std::string FormatJSON<const char*>(const char* value);

int MaybeOpenTraceFile();

class MetaTrace {
 public:
  template <typename... Ts>
  MetaTrace(Ts... args) {
    AddElements(args...);
    WriteEvent("B");
  }

  template <typename T, typename... Ts>
  void AddElements(const char* name, T arg, Ts... args) {
    trace_.emplace_back(FormatJSON(name), FormatJSON(std::move(arg)));
    AddElements(args...);
  }

  template <typename T>
  void AddElements(const char* name, T arg) {
    trace_.emplace_back(FormatJSON(name), FormatJSON(std::move(arg)));
  }

  ~MetaTrace() { WriteEvent("E"); }

 private:
  void WriteEvent(std::string type);

  std::vector<std::pair<std::string, std::string>> trace_;
};

#if PERFETTO_DCHECK_IS_ON() && !PERFETTO_BUILDFLAG(PERFETTO_CHROMIUM_BUILD)
#define PERFETTO_METATRACE(...) ::perfetto::base::MetaTrace(__VA_ARGS__)
#else
#define PERFETTO_METATRACE(...) ::perfetto::base::ignore_result(__VA_ARGS__)
#endif

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_BASE_METATRACE_H_
