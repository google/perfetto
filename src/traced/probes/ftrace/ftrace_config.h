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

#ifndef SRC_TRACED_PROBES_FTRACE_FTRACE_CONFIG_H_
#define SRC_TRACED_PROBES_FTRACE_FTRACE_CONFIG_H_

#include <set>
#include <string>

#include "perfetto/tracing/core/ftrace_config.h"

namespace perfetto {

// 0 is invalid.
using FtraceConfigId = uint64_t;

// Utility method for the common case where we don't care about atrace events.
FtraceConfig CreateFtraceConfig(std::set<std::string> names);

// Get the ftrace events for a config as a set.
std::set<std::string> FtraceEventsAsSet(const FtraceConfig&);

// Returns true iff the config has any atrace categories or apps.
bool RequiresAtrace(const FtraceConfig&);

// Returns true iff the config is 'valid'. Spesfically all the
// event/categories/app names should not look like:
// "../../some/kind/of/directory/escape".
bool ValidConfig(const FtraceConfig& config);

}  // namespace perfetto

#endif  // SRC_TRACED_PROBES_FTRACE_FTRACE_CONFIG_H_
