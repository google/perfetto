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

#ifndef SRC_TRACED_PROBES_FTRACE_ATRACE_WRAPPER_H_
#define SRC_TRACED_PROBES_FTRACE_ATRACE_WRAPPER_H_

#include <string>
#include <type_traits>
#include <vector>

namespace perfetto {

using RunAtraceFunction =
    std::add_pointer<bool(const std::vector<std::string>& /*args*/,
                          std::string* /*atrace_errors*/)>::type;

// When we are sideloaded on an old version of Android (pre P), we cannot use
// atrace --only_userspace because that option doesn't exist. In that case we:
// - Just use atrace --async_start/stop, which will cause atrace to also
//   poke at ftrace.
// - Suppress the checks for "somebody else enabled ftrace unexpectedly".
bool IsOldAtrace();
void SetIsOldAtraceForTesting(bool);
void ClearIsOldAtraceForTesting();

bool RunAtrace(const std::vector<std::string>& args,
               std::string* atrace_errors);
void SetRunAtraceForTesting(RunAtraceFunction);

}  // namespace perfetto

#endif  // SRC_TRACED_PROBES_FTRACE_ATRACE_WRAPPER_H_
