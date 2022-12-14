// Copyright (C) 2022 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef INCLUDE_PERFETTO_TRACE_PROCESSOR_METATRACE_CONFIG_H_
#define INCLUDE_PERFETTO_TRACE_PROCESSOR_METATRACE_CONFIG_H_

#include <cstddef>

namespace perfetto {
namespace trace_processor {
namespace metatrace {

enum MetatraceCategories {
  TOPLEVEL = 1 << 0,
  QUERY = 1 << 1,
  FUNCTION = 1 << 2,

  NONE = 0,
  ALL = TOPLEVEL | QUERY | FUNCTION,
};

struct MetatraceConfig {
  MetatraceConfig();

  MetatraceCategories categories = MetatraceCategories::ALL;
  // Requested buffer size. The implemenation may choose to allocate a larger
  // buffer size for efficiency.
  size_t override_buffer_size = 0;
};

}  // namespace metatrace
}  // namespace trace_processor
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_TRACE_PROCESSOR_METATRACE_CONFIG_H_
