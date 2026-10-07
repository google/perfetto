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

#ifndef SRC_TOOLS_PROTO_MERGER_EXTENSION_PROTO_MERGER_H_
#define SRC_TOOLS_PROTO_MERGER_EXTENSION_PROTO_MERGER_H_

#include <string>
#include <vector>

#include "perfetto/base/status.h"

namespace perfetto {
namespace proto_merger {

// Extracts preamble (syntax, package, imports, comments) from the given .proto
// text before any message or enum definitions begin (or before '// Begin of ').
std::string ExtractPreamble(const std::string& content);

// Inlines all proto extension fields from |extension_proto_files| into the
// target messages in |input_proto_file| (e.g. TracePacket, TrackEvent,
// InternedData), resolving imports relative to |input_include_dir|, and appends
// all helper enums and submessages.
base::Status MergeExtensions(
    const std::string& input_proto_file,
    const std::string& input_include_dir,
    const std::vector<std::string>& extension_proto_files,
    std::string* out);

// CLI entry point for the extension_proto_merger tool.
int ExtensionProtoMergerMain(int argc, char** argv);

}  // namespace proto_merger
}  // namespace perfetto

#endif  // SRC_TOOLS_PROTO_MERGER_EXTENSION_PROTO_MERGER_H_
