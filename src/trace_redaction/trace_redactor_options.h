/*
 * Copyright (C) 2024 The Android Open Source Project
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

#ifndef SRC_TRACE_REDACTION_TRACE_REDACTOR_OPTIONS_H_
#define SRC_TRACE_REDACTION_TRACE_REDACTOR_OPTIONS_H_

#include <cstdint>
#include <optional>
#include <string_view>

namespace perfetto::trace_redaction {

// Holds optional command-line arguments parsed for trace_redactor.
struct TraceRedactorOptions {
  // Explicit Linux UID of the target package instance (--uid <target uid>).
  // When unset, trace_redactor resolves the package's normalized UID from the
  // trace's PackagesList and normalizes process UIDs across user profiles.
  std::optional<uint64_t> target_uid;
};

// Parses the string value passed to --uid.
// Accepts only unsigned decimal ASCII digits, bounded to [0, INT32_MAX].
// Rejects leading/trailing whitespace, signs (+/-), and non-digits.
bool ParseUidOption(std::string_view uid_str, TraceRedactorOptions* options);

// Parses optional arguments beginning at argv[start_index].
bool ParseOptionalArgs(int argc,
                       char** argv,
                       int start_index,
                       TraceRedactorOptions* options);

}  // namespace perfetto::trace_redaction

#endif  // SRC_TRACE_REDACTION_TRACE_REDACTOR_OPTIONS_H_
