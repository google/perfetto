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

#include <string.h>

#include "perfetto/base/logging.h"
#include "perfetto/base/utils.h"

namespace perfetto {
namespace base {

class MetaTrace {
 public:
  MetaTrace(const char* evt_name, size_t cpu) : evt_name_(evt_name), cpu_(cpu) {
    WriteEvent('B', evt_name, cpu);
  }

  ~MetaTrace() { WriteEvent('E', evt_name_, cpu_); }

 private:
  MetaTrace(const MetaTrace&) = delete;
  MetaTrace& operator=(const MetaTrace&) = delete;

  void WriteEvent(char type, const char* evt_name, size_t cpu);

  const char* const evt_name_;
  const size_t cpu_;
};

#if PERFETTO_DCHECK_IS_ON() && !PERFETTO_BUILDFLAG(PERFETTO_CHROMIUM_BUILD)

#define PERFETTO_METATRACE(...) \
  ::perfetto::base::MetaTrace metatrace_##__COUNTER__(__VA_ARGS__)
#else
#define PERFETTO_METATRACE(...) ::perfetto::base::ignore_result(__VA_ARGS__)
#endif

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_BASE_METATRACE_H_
