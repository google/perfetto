/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_PARALLEL_QUERY_CONFIG_H_
#define SRC_TRACE_PROCESSOR_PARALLEL_QUERY_CONFIG_H_

#include <cstring>

namespace perfetto::trace_processor {

// Resolves whether parallel query execution is enabled. |env_override| is the
// value of the PERFETTO_TP_PARALLEL_QUERIES environment variable (nullptr if
// unset). When present and non-empty it takes precedence over |config_enabled|
// and acts as a runtime kill-switch: "0" forces off, any other value forces on.
inline bool ResolveParallelQueriesEnabled(bool config_enabled,
                                          const char* env_override) {
  if (env_override != nullptr && env_override[0] != '\0')
    return std::strcmp(env_override, "0") != 0;
  return config_enabled;
}

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_PARALLEL_QUERY_CONFIG_H_
