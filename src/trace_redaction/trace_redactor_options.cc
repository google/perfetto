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

#include "src/trace_redaction/trace_redactor_options.h"

#include <limits>
#include <string>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_utils.h"

namespace perfetto::trace_redaction {

bool ParseUidOption(std::string_view uid_str, TraceRedactorOptions* options) {
  PERFETTO_DCHECK(options);
  if (options->target_uid.has_value()) {
    PERFETTO_ELOG("Duplicate --uid argument");
    return false;
  }
  if (uid_str.empty()) {
    PERFETTO_ELOG("Invalid target uid (cannot be empty)");
    return false;
  }
  if (uid_str.find_first_not_of("0123456789") != std::string_view::npos) {
    PERFETTO_ELOG("Invalid target uid (must contain only decimal digits): %.*s",
                  static_cast<int>(uid_str.size()), uid_str.data());
    return false;
  }

  auto parsed = base::StringToUInt64(std::string(uid_str));
  if (!parsed.has_value() ||
      *parsed > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
    PERFETTO_ELOG("Invalid target uid (exceeds max int32): %.*s",
                  static_cast<int>(uid_str.size()), uid_str.data());
    return false;
  }

  options->target_uid = *parsed;
  return true;
}

bool ParseOptionalArgs(int argc,
                       char** argv,
                       int start_index,
                       TraceRedactorOptions* options) {
  PERFETTO_DCHECK(options);
  for (int i = start_index; i < argc; ++i) {
    std::string_view arg(argv[i]);
    if (arg == "--uid") {
      if (i + 1 >= argc) {
        PERFETTO_ELOG("Missing argument for %.*s", static_cast<int>(arg.size()),
                      arg.data());
        return false;
      }
      std::string_view value(argv[++i]);
      if (!ParseUidOption(value, options)) {
        return false;
      }
    }
    // Future optional arguments (e.g. else if (arg == "--...")) can be parsed
    // here.
    else {
      PERFETTO_ELOG("Unknown argument: %.*s", static_cast<int>(arg.size()),
                    arg.data());
      return false;
    }
  }
  return true;
}

}  // namespace perfetto::trace_redaction
